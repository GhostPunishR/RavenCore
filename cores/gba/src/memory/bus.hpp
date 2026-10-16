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
class Serial;

int ppu_dispstat_low(const Ppu*) noexcept;
int ppu_vcount(const Ppu*) noexcept;
void ppu_affine_reference_write(Ppu*, int) noexcept;
void dma_control_write(DmaController*, int, int);
void serial_control_write(Serial*, int);

class Bus {
public:
    explicit Bus(Cartridge& cartridge);

    int read8(std::int32_t address);
    int read16(std::int32_t address);
    std::int32_t read32(std::int32_t address);
    // Lecture d'instruction : chemin court en ligne pour la RAM, la ROM et la
    // mémoire vidéo ; le BIOS (latch) et le reste passent par le chemin lent.
    int fetch16(std::int32_t address) {
        const auto value = fetch16_value(address);
        last_fetch = u32(value) | u32(value) << 16U;
        return value;
    }
    std::int32_t fetch32(std::int32_t address) {
        const auto value = fetch32_value(address);
        last_fetch = u32(value);
        return value;
    }
    // À rappeler quand les attentes changent (WAITCNT) ou que l'état est restauré.
    void close_fetch_window() noexcept { fetch_window_ = {}; }
    void write8(std::int32_t address, int value);
    void write16(std::int32_t address, int value);
    void write32(std::int32_t address, std::int32_t value);
    // Cycles écoulés depuis le dernier relevé : chaque accès compte 1 cycle
    // plus ses attentes, et idle() ajoute les cycles internes du CPU.
    int take_wait_cycles() noexcept { return std::exchange(wait_cycles, 0); }
    void idle(int cycles) noexcept {
        wait_cycles += cycles;
        prefetch_idle(cycles);
    }
    /**
     * Tampon de préchargement de la cartouche (WAITCNT bit 14, GBATEK
     * « GamePak Prefetch »). Pendant que le CPU n'utilise pas le bus de la
     * cartouche (cycles internes, accès à la RAM ou aux E/S), une unité lit
     * à la suite les demi-mots de code qui suivent la dernière instruction
     * lue en ROM, huit au plus. Une instruction déjà lue coûte alors 1 cycle,
     * celle que l'unité est en train de lire le reste de sa lecture. Un saut
     * vide le tampon ; un accès de données à la cartouche arrête l'unité.
     */
    struct Prefetch {
        std::uint32_t head{};    // prochain demi-mot que l'unité lira
        int count{};             // demi-mots prêts, de head - 2 × count à head - 2
        int progress{};          // cycles déjà passés à lire le demi-mot head
        int halfword_cycles{1};  // durée d'une lecture séquentielle de 16 bits
        bool active{};
    };
    static constexpr int prefetch_capacity = 8;
    [[nodiscard]] const Prefetch& prefetch() const noexcept { return prefetch_; }
    // Après restauration de WAITCNT : la durée d'un demi-mot en découle.
    void restore_prefetch(const Prefetch& value) {
        const auto cycles = timing.sequential_halfword_cycles(value.head);
        if (value.count < 0 || value.count > prefetch_capacity || value.progress < 0 || value.progress >= cycles ||
            (value.head & 1U) != 0 || (!value.active && (value.count != 0 || value.progress != 0)) ||
            (value.active && !timing.prefetch_enabled()))
            throw SaveStateError("État GBA corrompu (préchargement)");
        prefetch_ = value;
        prefetch_.halfword_cycles = cycles;
    }
    void break_access_sequence() noexcept { next_sequential_address = -1; }
    // Adresse qu'un accès séquentiel viserait : conservée dans les états, sans
    // quoi le premier accès après un chargement paierait une attente de trop.
    [[nodiscard]] std::int32_t sequential_address() const noexcept { return next_sequential_address; }
    void restore_sequential_address(std::int32_t address) noexcept { next_sequential_address = address; }
    void reset_affine_matrices();
    void sync_timing_from_io();
    [[nodiscard]] Eeprom* eeprom() noexcept { return eeprom_; }
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
    Serial* serial{};
    MemoryTiming timing;
    Diagnostics diagnostics;
    int wait_cycles{};
    // Protection du BIOS : hors du BIOS, le lire rend le dernier mot que le
    // pipeline a préchargé depuis le BIOS, pas son contenu.
    static constexpr std::uint32_t bios_latch_after_startup = 0xe129'f000U;
    static constexpr std::uint32_t bios_latch_after_swi = 0xe3a0'2004U;
    std::uint32_t bios_latch{bios_latch_after_startup};
    bool executing_bios{};
    // Bus ouvert : une zone non câblée rend le dernier mot que le pipeline a
    // lu (en Thumb, le demi-mot répété), pas une valeur fixe.
    std::uint32_t last_fetch{};
    // Appelé avant chaque accès aux registres d'E/S : la machine y rattrape
    // les périphériques, avancés par lots entre deux événements.
    void (*io_sync)(void* context, bool write){};
    void* io_sync_context{};
    // Écriture de HALTCNT (0x04000301) : la machine arrête le CPU.
    void (*io_halt)(void* context){};

private:
    // La puce de sauvegarde naît avec la cartouche et ne change plus : le bus
    // la classe une fois, au lieu d'un dynamic_cast à chaque accès mémoire.
    Eeprom* eeprom_{};
    SaveMemory* byte_save_{};  // SRAM ou Flash : la puce du bus 8 bits
    static constexpr int io_mask = 0x3ff;
    // Les registres d'E/S n'occupent que 0x04000000..0x040003FF : ils ne se
    // répètent pas dans le reste de la région, où une écriture se perd. Une
    // trace de débogage écrite à 0x04FFF600 (mGBA) écrasait sinon IE.
    static constexpr std::uint32_t memory_control_reset = 0x0d00'0020U;
    [[nodiscard]] static bool io_mapped(std::uint32_t address) noexcept { return (address & 0x00ff'fc00U) == 0; }
    void account(std::int32_t address, int width, bool fetch = false) noexcept {
        auto sequential = address == next_sequential_address;
        next_sequential_address = add32(address, width);
        const auto region = u32(address) >> 24U;
        if (region >= 0x08U && region <= 0x0fU) {
            // Chaque bloc de 128 Kio de la cartouche, comme l'entrée dans sa
            // région, commence par un accès non séquentiel (GBATEK).
            if ((u32(address) & 0x1ffffU) == 0) sequential = false;
            if (fetch && region <= 0x0dU && timing.prefetch_enabled()) {
                wait_cycles += prefetch_fetch(u32(address), width, 1 + timing.cached_wait(address, width, sequential, true),
                                              timing.sequential_halfword_cycles(u32(address)));
                return;
            }
            // Un accès de données qui tombe sur le dernier cycle d'un demi-mot
            // que l'unité de préchargement lit encore attend ce cycle ; plus tôt,
            // la lecture en cours est abandonnée (mesures de la suite mGBA).
            if (prefetch_.active && prefetch_.progress == prefetch_.halfword_cycles - 1) ++wait_cycles;
            wait_cycles += 1 + timing.cached_wait(address, width, sequential, fetch);
            prefetch_stop();
            return;
        }
        const auto cost = 1 + timing.cached_wait(address, width, sequential, fetch);
        wait_cycles += cost;
        prefetch_idle(cost);
    }
    Prefetch prefetch_{};
    void prefetch_idle(int cycles) noexcept {
        auto& unit = prefetch_;
        if (!unit.active || unit.count == prefetch_capacity) return;
        unit.progress += cycles;
        // Au plus huit demi-mots : une boucle courte plutôt qu'une division.
        while (unit.progress >= unit.halfword_cycles) {
            unit.progress -= unit.halfword_cycles;
            unit.head += 2U;
            if (++unit.count == prefetch_capacity) {
                unit.progress = 0;
                return;
            }
        }
    }
    void prefetch_stop() noexcept {
        prefetch_.active = false;
        prefetch_.count = 0;
        prefetch_.progress = 0;
    }
    // Durée de la lecture de l'instruction `address` (width octets) en ROM,
    // tampon actif. `miss_cost` : coût d'une lecture normale (saut, unité
    // arrêtée) ; `halfword_cycles` : durée d'un demi-mot séquentiel à cette adresse.
    int prefetch_fetch(std::uint32_t address, int width, int miss_cost, int halfword_cycles) noexcept {
        auto& unit = prefetch_;
        const auto halfwords = width / 2;
        if (unit.active && address == unit.head - 2U * static_cast<std::uint32_t>(unit.count)) {
            if (unit.count >= halfwords) {
                // Instruction déjà dans le tampon : 1 cycle, pendant lequel
                // l'unité lit toujours (un tampon plein se libère d'abord).
                if (++unit.progress == unit.halfword_cycles) {
                    unit.progress = 0;
                    unit.head += 2U;
                    ++unit.count;
                }
                unit.count -= halfwords;
                return 1;
            }
            // L'unité lit encore la suite : le demi-mot en cours s'achève après
            // le reste de sa lecture, les suivants un par un, et passent
            // directement au CPU.
            const auto beyond = halfwords - unit.count;
            const auto ready = unit.halfword_cycles - unit.progress + (beyond - 1) * unit.halfword_cycles;
            unit.head += 2U * static_cast<std::uint32_t>(beyond);
            unit.count = 0;
            unit.progress = 0;
            return ready;
        }
        // Saut, ou unité arrêtée : lecture normale, puis l'unité repart derrière elle.
        unit = {address + static_cast<std::uint32_t>(width), 0, 0, halfword_cycles, true};
        return miss_cost;
    }
    /**
     * Fenêtre de lecture d'instructions : zone contiguë (IWRAM, EWRAM ou ROM
     * d'une région d'attente) où une lecture séquentielle coûte toujours la
     * même attente. Une instruction suivante y est lue sans aiguillage ni
     * table : même résultat que account() + direct_read(), en plus court.
     */
    struct FetchWindow {
        std::uint32_t low{1}; std::uint32_t size{}; const std::uint8_t* base{};
        int wait16{}; int wait32{};    // lecture séquentielle, cycle d'accès compris
        int nwait16{}; int nwait32{};  // lecture non séquentielle (saut, après une donnée)
        int halfword_cycles{1};        // ROM : durée d'un demi-mot pour le tampon
        bool cart{};      // ROM : le bus de la cartouche est occupé par la lecture
        bool prefetch{};  // ROM avec le tampon de préchargement actif
    };
    [[nodiscard]] const std::uint8_t* fetch_window_hit(std::uint32_t aligned, std::uint32_t width) noexcept {
        const auto& window = fetch_window_;
        const auto offset = aligned - window.low;
        if (offset >= window.size || window.size - offset < width) return nullptr;
        // Après un saut ou un accès de données, la lecture reste dans la fenêtre
        // mais n'est plus séquentielle ; le début d'un bloc de 128 Kio de la
        // cartouche ne l'est jamais.
        const auto sequential = i32(aligned) == next_sequential_address && (!window.cart || (aligned & 0x1ffffU) != 0);
        next_sequential_address = i32(aligned + width);
        const auto cost = width == 4 ? (sequential ? window.wait32 : window.nwait32)
                                     : (sequential ? window.wait16 : window.nwait16);
        if (window.prefetch) wait_cycles += prefetch_fetch(aligned, static_cast<int>(width), cost, window.halfword_cycles);
        else {
            // Préchargement coupé, l'unité est à l'arrêt : seule une lecture
            // hors cartouche la laisserait avancer.
            wait_cycles += cost;
            if (!window.cart) prefetch_idle(cost);
        }
        executing_bios = false;
        return window.base + offset;
    }
    void open_fetch_window(std::uint32_t address) noexcept {
        const auto region = address >> 24U;
        FetchWindow window;
        if (region == 0x02U) window = {0x0200'0000U, 0x4'0000U, ewram.data()};
        else if (region == 0x03U) window = {0x0300'0000U, 0x8000U, iwram.data()};
        else if (region >= 0x08U && region <= 0x0dU) {
            // Fenêtre limitée au bloc de 128 Kio de l'adresse : le premier
            // accès du bloc suivant est non séquentiel. Le GPIO (0xC4..0xC9)
            // reste sur le chemin complet.
            const auto rom = cartridge.rom();
            const auto offset = address & 0x01ff'ffffU;
            const auto block = offset & ~0x1ffffU;
            const auto start = block == 0 && cartridge.gpio() ? 0x100U : block;
            if (offset < start || rom.size() <= start) return;
            const auto end = std::min<std::size_t>(rom.size(), std::size_t{block} + 0x20000U);
            window = {(address & ~0x01ff'ffffU) + start, static_cast<std::uint32_t>(end - start), rom.data() + start};
            window.cart = true;
            window.prefetch = timing.prefetch_enabled();
            window.halfword_cycles = timing.sequential_halfword_cycles(window.low);
        } else return;
        const auto low = i32(window.low);
        window.wait16 = 1 + timing.cached_wait(low, 2, true, true);
        window.wait32 = 1 + timing.cached_wait(low, 4, true, true);
        window.nwait16 = 1 + timing.cached_wait(low, 2, false, true);
        window.nwait32 = 1 + timing.cached_wait(low, 4, false, true);
        fetch_window_ = window;
    }
    FetchWindow fetch_window_{};
    int fetch16_value(std::int32_t address) {
        const auto aligned = u32(address) & ~1U;
        if (const auto* data = fetch_window_hit(aligned, 2)) return static_cast<int>(load_le(data, 2));
        account(i32(aligned), 2, true);
        if ((aligned >> 24U) != 0U) {
            executing_bios = false;
            if (const auto* data = direct_read(aligned, 2)) { open_fetch_window(aligned); return static_cast<int>(load_le(data, 2)); }
        }
        return fetch16_slow(i32(aligned));
    }
    std::int32_t fetch32_value(std::int32_t address) {
        const auto aligned = u32(address) & ~3U;
        if (const auto* data = fetch_window_hit(aligned, 4)) return i32(load_le(data, 4));
        account(i32(aligned), 4, true);
        if ((aligned >> 24U) != 0U) {
            executing_bios = false;
            if (const auto* data = direct_read(aligned, 4)) { open_fetch_window(aligned); return i32(load_le(data, 4)); }
        }
        return fetch32_slow(i32(aligned));
    }
    [[nodiscard]] int open_bus(std::uint32_t address) const noexcept {
        return static_cast<int>((last_fetch >> ((address & 3U) * 8U)) & 0xffU);
    }
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
