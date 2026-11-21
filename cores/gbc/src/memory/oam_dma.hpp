#pragma once

#include "ppu/ppu.hpp"
#include "support/bits.hpp"
#include <ravenemu/gbc/hardware_mode.hpp>

namespace ravenemu::cgb {

/**
 * DMA OAM (FF46) : copie 160 octets vers l'OAM, un octet tous les 4 cycles
 * CPU après un M-cycle de démarrage. Il accélère donc en double vitesse. Le
 * bus lui fournit les lectures de la source ; le PPU reçoit les octets et
 * présente au fetcher d'objets le mot en cours de transfert.
 */
class OamDma {
public:
    [[nodiscard]] int read_register() const noexcept { return register_; }

    void write_register(int high) noexcept {
        register_ = byte(high);
        // Le nouveau transfert ne remplace le moteur courant qu'après le
        // M-cycle de démarrage. Lors d'un restart, l'ancien DMA continue donc
        // pendant cette fenêtre, comme sur le bus physique.
        pending_ = true;
        startup_cycles_ = 4;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    /** Démarrage en attente ou transfert en cours : le bus doit le cadencer. */
    [[nodiscard]] bool busy() const noexcept { return pending_ || active_; }
    [[nodiscard]] int source() const noexcept { return source_; }

    /** Avance de `cpu_cycles` cycles CPU ; `read` lit un octet de la source. */
    template <typename Read>
    void tick(int cpu_cycles, gbc::HardwareMode mode, Ppu& ppu, Read read) noexcept {
        if (cpu_cycles <= 0) return;
        for (int cycle = 0; cycle < cpu_cycles; ++cycle) {
            if (pending_) {
                if (startup_cycles_ > 0) {
                    tick_active(1, ppu, read);
                    --startup_cycles_;
                    continue;
                }
                activate(mode, ppu);
            }
            tick_active(1, ppu, read);
        }
    }

    void save(BinaryWriter& out) const {
        out.i32(register_);
        out.i32(active_ ? 1 : 0);
        out.i32(source_);
        out.i32(index_);
        out.i32(cycle_accum_);
        out.i32(pending_ ? 1 : 0);
        out.i32(startup_cycles_);
    }

    void load(BinaryReader& in) {
        register_ = bounded(in, 0, 0xff);
        active_ = bounded(in, 0, 1) != 0;
        source_ = bounded(in, 0, 0xff00);
        index_ = bounded(in, 0, 0xa0);
        cycle_accum_ = bounded(in, 0, 3);
        pending_ = bounded(in, 0, 1) != 0;
        startup_cycles_ = bounded(in, 0, 4);
    }

    /** Cohérence d'un état rechargé, dans le mode matériel courant. */
    [[nodiscard]] bool consistent(gbc::HardwareMode mode) const noexcept {
        return !((active_ && !pending_ && source_ != source_from_register(mode)) ||
                 (active_ && index_ >= 0xa0) ||
                 (!pending_ && startup_cycles_ != 0));
    }

    /** Après un chargement, remet le port OAM du PPU dans l'état du transfert. */
    void restore_ppu_port(Ppu& ppu) const noexcept {
        if (active_) ppu.restore_oam_dma_bus(index_);
        else ppu.end_oam_dma();
    }

    /** Lecture bornée d'un champ d'état ; même message pour les deux DMA. */
    static int bounded(BinaryReader& in, int minimum, int maximum) {
        const int value = in.i32();
        if (value < minimum || value > maximum) throw SaveStateError("État instantané corrompu (DMA)");
        return value;
    }

private:
    [[nodiscard]] int source_from_register(gbc::HardwareMode mode) const noexcept {
        // Sur les DMG, E0-FF reboucle sur C0-DF (la ligne A13 n'atteint pas
        // le mux source du DMA). Le CGB ne possède pas ce miroir.
        const int high = mode == gbc::HardwareMode::dmg && register_ >= 0xe0
            ? register_ & 0xdf : register_;
        return high << 8;
    }

    void activate(gbc::HardwareMode mode, Ppu& ppu) noexcept {
        pending_ = false;
        source_ = source_from_register(mode);
        index_ = 0;
        cycle_accum_ = 0;
        startup_cycles_ = 0;
        active_ = true;
        ppu.begin_oam_dma();
    }

    template <typename Read>
    void tick_active(int cpu_cycles, Ppu& ppu, Read read) noexcept {
        if (!active_ || cpu_cycles <= 0) return;
        cycle_accum_ += cpu_cycles;
        while (active_ && cycle_accum_ >= 4) {
            cycle_accum_ -= 4;
            const int source = source_ + index_;
            ppu.write_oam_dma_byte(index_, read(source));
            ++index_;
            if (index_ >= 0xa0) {
                active_ = false;
                cycle_accum_ = 0;
                startup_cycles_ = 0;
                ppu.end_oam_dma();
            }
        }
    }

    int register_{0xff};
    bool active_{};
    int source_{};
    int index_{};
    int cycle_accum_{};
    bool pending_{};
    int startup_cycles_{};
};

} // namespace ravenemu::cgb
