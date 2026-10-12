#pragma once

#include "memory/memory_timing.hpp"
#include "cartridge/cartridge.hpp"
#include "interrupts/interrupt_controller.hpp"
#include "input/keypad.hpp"
#include "diagnostics/diagnostics.hpp"

namespace ravenemu::gba {

class Ppu;
class Timers;
class DmaController;
class Apu;

int ppu_dispstat_low(const Ppu*) noexcept;
int ppu_vcount(const Ppu*) noexcept;
void ppu_affine_reference_write(Ppu*, int) noexcept;
void dma_control_write(DmaController*, int, int);

class Bus {
public:
    explicit Bus(Cartridge& cartridge);

    int read8(std::int32_t address);
    int read16(std::int32_t address);
    std::int32_t read32(std::int32_t address);
    // Lecture d'instruction : chemin court en ligne pour la RAM, la ROM et la
    // mémoire vidéo ; le BIOS (latch) et le reste passent par le chemin lent.
    int fetch16(std::int32_t address) {
        const auto aligned = u32(address) & ~1U;
        if (const auto* data = fetch_window_hit(aligned, 2)) return static_cast<int>(load_le(data, 2));
        account(i32(aligned), 2, true);
        if ((aligned >> 24U) != 0U) {
            executing_bios = false;
            if (const auto* data = direct_read(aligned, 2)) { open_fetch_window(aligned); return static_cast<int>(load_le(data, 2)); }
        }
        return fetch16_slow(i32(aligned));
    }
    std::int32_t fetch32(std::int32_t address) {
        const auto aligned = u32(address) & ~3U;
        if (const auto* data = fetch_window_hit(aligned, 4)) return i32(load_le(data, 4));
        account(i32(aligned), 4, true);
        if ((aligned >> 24U) != 0U) {
            executing_bios = false;
            if (const auto* data = direct_read(aligned, 4)) { open_fetch_window(aligned); return i32(load_le(data, 4)); }
        }
        return fetch32_slow(i32(aligned));
    }
    // À rappeler quand les attentes changent (WAITCNT) ou que l'état est restauré.
    void close_fetch_window() noexcept { fetch_window_ = {}; }
    void write8(std::int32_t address, int value);
    void write16(std::int32_t address, int value);
    void write32(std::int32_t address, std::int32_t value);
    int take_wait_cycles() noexcept { return std::exchange(wait_cycles, 0); }
    void break_access_sequence() noexcept { next_sequential_address = -1; }
    void reset_affine_matrices();
    void sync_timing_from_io();
    [[nodiscard]] Eeprom* eeprom() noexcept { return dynamic_cast<Eeprom*>(cartridge.save()); }
    [[nodiscard]] SaveMemory* save_memory() noexcept { return cartridge.save(); }

    Cartridge& cartridge;
    Keypad keypad;
    std::array<std::uint8_t, 0x4000> bios{};
    std::array<std::uint8_t, 0x40000> ewram{};
    std::array<std::uint8_t, 0x8000> iwram{};
    std::array<std::uint8_t, 0x400> io{};
    std::array<std::uint8_t, 0x400> palette{};
    std::array<std::uint8_t, 0x18000> vram{};
    std::array<std::uint8_t, 0x400> oam{};
    std::array<std::uint8_t, 0x10000> fallback_sram{};
    Ppu* ppu{};
    InterruptController* interrupts{};
    Timers* timers{};
    DmaController* dma{};
    Apu* apu{};
    MemoryTiming timing;
    Diagnostics diagnostics;
    int wait_cycles{};
    // Protection du BIOS : hors du BIOS, le lire rend le dernier mot que le
    // pipeline a préchargé depuis le BIOS, pas son contenu.
    static constexpr std::uint32_t bios_latch_after_startup = 0xe129'f000U;
    static constexpr std::uint32_t bios_latch_after_swi = 0xe3a0'2004U;
    std::uint32_t bios_latch{bios_latch_after_startup};
    bool executing_bios{};
    // Appelé avant chaque accès aux registres d'E/S : la machine y rattrape
    // les périphériques, avancés par lots entre deux événements.
    void (*io_sync)(void* context, bool write){};
    void* io_sync_context{};

private:
    static constexpr int io_mask = 0x3ff;
    void account(std::int32_t address, int width, bool fetch = false) noexcept {
        const auto sequential = address == next_sequential_address;
        wait_cycles += timing.cached_wait(address, width, sequential, fetch);
        next_sequential_address = add32(address, width);
    }
    /**
     * Fenêtre de lecture d'instructions : zone contiguë (IWRAM, EWRAM ou ROM
     * d'une région d'attente) où une lecture séquentielle coûte toujours la
     * même attente. Une instruction suivante y est lue sans aiguillage ni
     * table : même résultat que account() + direct_read(), en plus court.
     */
    struct FetchWindow {
        std::uint32_t low{1}; std::uint32_t size{}; const std::uint8_t* base{};
        int wait16{}; int wait32{};
    };
    [[nodiscard]] const std::uint8_t* fetch_window_hit(std::uint32_t aligned, std::uint32_t width) noexcept {
        const auto offset = aligned - fetch_window_.low;
        if (i32(aligned) != next_sequential_address || offset >= fetch_window_.size || fetch_window_.size - offset < width) return nullptr;
        wait_cycles += width == 4 ? fetch_window_.wait32 : fetch_window_.wait16;
        next_sequential_address = i32(aligned + width);
        executing_bios = false;
        return fetch_window_.base + offset;
    }
    void open_fetch_window(std::uint32_t address) noexcept {
        const auto region = address >> 24U;
        FetchWindow window;
        if (region == 0x02U) window = {0x0200'0000U, 0x4'0000U, ewram.data()};
        else if (region == 0x03U) window = {0x0300'0000U, 0x8000U, iwram.data()};
        else if (region >= 0x08U && region <= 0x0dU) {
            // Une région impaire couvre la seconde moitié d'une ROM de 32 Mio ;
            // le GPIO (0xC4..0xC9, région paire) reste sur le chemin complet.
            const auto rom = cartridge.rom();
            const std::size_t start = (region & 1U) != 0 ? 0x0100'0000U : (cartridge.gpio() ? 0x100U : 0U);
            if (rom.size() <= start) return;
            const auto size = static_cast<std::uint32_t>(std::min<std::size_t>(rom.size() - start, 0x0100'0000U - (start & 0xffffU)));
            window = {(region << 24U) + static_cast<std::uint32_t>(start & 0xffffU), size, rom.data() + start};
        } else return;
        window.wait16 = timing.cached_wait(i32(window.low), 2, true, true);
        window.wait32 = timing.cached_wait(i32(window.low), 4, true, true);
        fetch_window_ = window;
    }
    FetchWindow fetch_window_{};
    int fetch16_slow(std::int32_t aligned);
    std::int32_t fetch32_slow(std::int32_t aligned);
    // Assemblage petit-boutiste portable ; les compilateurs en font une seule
    // lecture sur les cibles petit-boutistes.
    static std::uint32_t load_le(const std::uint8_t* data, std::uint32_t width) noexcept {
        if (width == 2) return std::uint32_t{data[0]} | std::uint32_t{data[1]} << 8U;
        return std::uint32_t{data[0]} | std::uint32_t{data[1]} << 8U | std::uint32_t{data[2]} << 16U |
               std::uint32_t{data[3]} << 24U;
    }
    static void store_le(std::uint8_t* data, std::uint32_t value, std::uint32_t width) noexcept {
        for (std::uint32_t index = 0; index < width; ++index) data[index] = static_cast<std::uint8_t>(value >> (8U * index));
    }
    void track_bios_fetch(std::int32_t address) noexcept;
    void sync_io(std::int32_t address, bool write) {
        if (io_sync && (u32(address) >> 24U) == 0x04U) io_sync(io_sync_context, write);
    }
    [[nodiscard]] int read8_raw(std::int32_t address);
    void write8_raw(std::int32_t address, int value);
    void write16_raw(std::int32_t address, int value);
    [[nodiscard]] int read_io(int offset);
    [[nodiscard]] int read_timer_byte(int offset);
    [[nodiscard]] int io_half(int offset) const noexcept;
    void write_io_half(int offset, int value) noexcept;
    void propagate_io_write(int offset, std::int32_t value, int bytes);
    void handle_io_write(int offset, int value);
    [[nodiscard]] bool eeprom_window(std::int32_t address) const noexcept;
    [[nodiscard]] bool gpio_register(std::int32_t address) const noexcept;
    // SRAM et Flash sont sur un bus de 8 bits : un accès de 16 ou 32 bits lit
    // l'octet visé répété, et n'écrit que l'octet que sélectionne l'adresse.
    // Accès direct aux mémoires sans effet de bord (RAM, vidéo, ROM hors
    // GPIO) : un mot se lit d'un bloc au lieu de quatre lectures d'octet qui
    // repassent chacune par l'aiguillage des régions. `width` octets alignés.
    [[nodiscard]] const std::uint8_t* direct_read(std::uint32_t address, std::uint32_t width) const noexcept {
        switch (address >> 24U) {
        case 0x02: return ewram.data() + (address & 0x3ffffU);
        case 0x03: return iwram.data() + (address & 0x7fffU);
        case 0x05: return palette.data() + (address & 0x3ffU);
        case 0x06: return vram.data() + vram_offset(i32(address));
        case 0x07: return oam.data() + (address & 0x3ffU);
        case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: {
            const auto offset = address & 0x01ff'ffffU;
            const auto rom = cartridge.rom();
            if (offset + width > rom.size()) return nullptr;
            if (const auto* port = cartridge.gpio(); port && port->readable() && offset < 0xd0U) return nullptr;
            return rom.data() + offset;
        }
        default: return nullptr;
        }
    }
    [[nodiscard]] std::uint8_t* direct_write(std::uint32_t address) noexcept;
    [[nodiscard]] static bool save_bus(std::int32_t address) noexcept {
        const auto region = u32(address) >> 24U;
        return region == 0x0eU || region == 0x0fU;
    }
    [[nodiscard]] static int rom_offset(std::int32_t address) noexcept { return static_cast<int>(u32(address) & 0x01ff'ffffU); }
    [[nodiscard]] static int vram_offset(std::int32_t address) noexcept {
        auto offset = static_cast<int>(u32(address) & 0x1ffffU);
        if (offset >= 0x18000) offset -= 0x8000;
        return offset;
    }
    std::int32_t next_sequential_address{-1};
};

} // namespace ravenemu::gba
