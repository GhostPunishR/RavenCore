#pragma once

#include "memory/oam_dma.hpp"
#include "ppu/ppu.hpp"
#include "support/bits.hpp"

namespace ravenemu::cgb {

/**
 * DMA VRAM de la GBC (FF51-FF55) : transfert général (GDMA), qui garde le bus
 * jusqu'à la fin, ou HBlank DMA, un bloc de 16 octets par H-Blank. Cadencé sur
 * l'horloge LCD, un octet tous les 2 points : sa durée physique ne change pas
 * en double vitesse. Le bus lui fournit les lectures de la source.
 */
class Hdma {
public:
    void write_source_high(int value) noexcept { source_ = (source_ & 0x00f0) | (value << 8); }
    void write_source_low(int value) noexcept { source_ = (source_ & 0xff00) | (value & 0xf0); }
    void write_destination_high(int value) noexcept { destination_ = (destination_ & 0x00f0) | ((value & 0x1f) << 8); }
    void write_destination_low(int value) noexcept { destination_ = (destination_ & 0x1f00) | (value & 0xf0); }

    /** FF55 : blocs restants moins un ; bit 7 à 1 hors transfert. */
    [[nodiscard]] int read_status() const noexcept {
        if (length_ <= 0) return 0xff;
        const int remaining = ((length_ + 15) / 16 - 1) & 0x7f;
        return active_ ? remaining : 0x80 | remaining;
    }

    /**
     * Écriture de FF55. `block_now` : écran éteint ou PPU en H-Blank, un
     * HBlank DMA peut copier son premier bloc tout de suite.
     */
    void write_control(int value, bool block_now) noexcept {
        if (active_) {
            if (hblank_ && (value & 0x80) == 0) {
                // Seul bit 7=0 arrête un HBlank DMA. Une écriture avec bit 7
                // à 1 ne recharge ni son adresse ni sa longueur. L'arrêt écrit
                // la longueur : FF55 relit $80 après $00 (SameSuite,
                // hdma_lcd_off et hdma_mode0).
                active_ = false;
                length_ = ((value & 0x7f) + 1) * 16;
                block_bytes_remaining_ = 0;
                dot_accum_ = 0;
            }
            return;
        }
        const bool hblank = (value & 0x80) != 0;
        length_ = ((value & 0x7f) + 1) * 16;
        hblank_ = hblank;
        active_ = true;
        // Lancé pendant un H-Blank, ou écran éteint, un HBlank DMA copie
        // aussitôt un bloc, puis attend le H-Blank suivant ; écran éteint, il
        // n'en vient pas, et le transfert en reste là (SameSuite, hdma_mode0,
        // hdma_lcd_off, gdma_addr_mask).
        const bool copy_now = !hblank || block_now;
        block_bytes_remaining_ = copy_now ? std::min(16, length_) : 0;
        dot_accum_ = 0;
    }

    /** L'écran s'éteint : sans H-Blank futur, un HBlank DMA finit comme un GDMA. */
    void lcd_turned_off() noexcept {
        if (!active_ || !hblank_) return;
        hblank_ = false;
        block_bytes_remaining_ = std::min(16, length_);
        dot_accum_ = 0;
    }

    void notify_hblank() noexcept {
        if (!active_ || !hblank_ || block_bytes_remaining_ > 0) return;
        block_bytes_remaining_ = std::min(16, length_);
        dot_accum_ = 0;
    }

    /** Un bloc en cours, ou un GDMA, tient le bus : le CPU attend. */
    [[nodiscard]] bool cpu_blocked() const noexcept {
        return block_bytes_remaining_ > 0 || (active_ && !hblank_);
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool hblank_mode() const noexcept { return hblank_; }
    [[nodiscard]] bool general_transfer_active() const noexcept { return active_ && !hblank_; }

    /** Avance de `peripheral_cycles` points ; `read` lit un octet de la source. */
    template <typename Read>
    void tick(int peripheral_cycles, Ppu& ppu, Read read) noexcept {
        if (!active_ || block_bytes_remaining_ <= 0 || peripheral_cycles <= 0) return;
        dot_accum_ += peripheral_cycles;
        while (active_ && block_bytes_remaining_ > 0 && dot_accum_ >= 2) {
            dot_accum_ -= 2;
            const bool destination_available = transfer_byte(ppu, read);
            --block_bytes_remaining_;
            --length_;
            if (length_ <= 0 || !destination_available) {
                length_ = std::max(0, length_);
                active_ = false;
                block_bytes_remaining_ = 0;
                break;
            }
            if (block_bytes_remaining_ == 0 && !hblank_) {
                block_bytes_remaining_ = std::min(16, length_);
            }
        }
    }

    void save(BinaryWriter& out) const {
        out.i32(source_);
        out.i32(destination_);
        out.i32(length_);
        out.i32(hblank_ ? 1 : 0);
        out.i32(active_ ? 1 : 0);
        out.i32(block_bytes_remaining_);
        out.i32(dot_accum_);
    }

    void load(BinaryReader& in) {
        source_ = OamDma::bounded(in, 0, 0xffff);
        destination_ = OamDma::bounded(in, 0, 0x2000);
        length_ = OamDma::bounded(in, 0, 0x800);
        hblank_ = OamDma::bounded(in, 0, 1) != 0;
        active_ = OamDma::bounded(in, 0, 1) != 0;
        block_bytes_remaining_ = OamDma::bounded(in, 0, 16);
        dot_accum_ = OamDma::bounded(in, 0, 1);
    }

    [[nodiscard]] bool consistent() const noexcept {
        // Une destination en fin de banque (0x2000) reste valide pendant un
        // transfert : relancé sans la réécrire, il s'arrête au premier octet.
        return !((active_ && length_ == 0) ||
                 block_bytes_remaining_ > length_ ||
                 (!active_ && block_bytes_remaining_ != 0) ||
                 (active_ && !hblank_ && block_bytes_remaining_ == 0));
    }

private:
    template <typename Read>
    [[nodiscard]] bool transfer_byte(Ppu& ppu, Read read) noexcept {
        // La destination couvre les 8 Kio d'une banque de VRAM. Un transfert
        // relancé sans la réécrire après avoir atteint la fin écrivait hors du
        // tableau (banque 1 : vram[0x4000]) et laissait un état invalide.
        if (destination_ >= 0x2000) return false;
        const int value = read(word(source_));
        ppu.vram[static_cast<std::size_t>(ppu.vram_bank() * 0x2000 + destination_)] = static_cast<std::uint8_t>(value);
        source_ = word(source_ + 1);
        ++destination_;
        return destination_ < 0x2000;
    }

    int source_{};
    int destination_{};
    int length_{};
    bool hblank_{};
    bool active_{};
    int block_bytes_remaining_{};
    int dot_accum_{};
};

} // namespace ravenemu::cgb
