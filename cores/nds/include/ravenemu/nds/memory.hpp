#pragma once

#include <ravenemu/nds/hardware.hpp>
#include <ravenemu/nds/interrupts.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace ravenemu::nds {

/** Bus RAM, BIOS et premiers registres MMIO NDS, sans timings ni open-bus.
 * Les accès non mappés ou non alignés lèvent ravenemu::Error.
 * Les rotations des lectures ARM non alignées relèveront du CPU.
 */
class Memory {
public:
    Memory();
    // ARM7 : 16 Kio aux vecteurs bas ; ARM9 : 4 Kio aux vecteurs hauts NDS.
    void load_bios(Processor cpu, std::span<const std::uint8_t> bytes);
    void clear_ram() noexcept; // remet aussi WRAMCNT et les IRQ à zéro ; conserve les BIOS
    // Sources : masque de bits IF, pas un numéro de source.
    void request_interrupt(Processor cpu, std::uint32_t sources) noexcept;
    [[nodiscard]] bool irq_pending(Processor cpu) const noexcept;
    [[nodiscard]] std::uint8_t wram_control() const noexcept { return wram_control_; }
    void set_wram_control(std::uint8_t value) noexcept { wram_control_ = value & 3U; }
    [[nodiscard]] std::uint8_t read8(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read16(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read32(Processor cpu, std::uint32_t address) const;
    void write8(Processor cpu, std::uint32_t address, std::uint8_t value);
    void write16(Processor cpu, std::uint32_t address, std::uint16_t value);
    void write32(Processor cpu, std::uint32_t address, std::uint32_t value);
    // Valide toute la plage avant d'écrire : une erreur ne modifie aucun octet.
    void write_words(Processor cpu, std::uint32_t address, std::span<const std::uint32_t> values);

private:
    [[nodiscard]] std::size_t index(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint8_t> bios_byte(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint8_t> io_byte(Processor cpu, std::uint32_t address) const noexcept;
    void write_io_byte(Processor cpu, std::uint32_t address, std::uint8_t value) noexcept;
    [[nodiscard]] InterruptController& controller(Processor cpu) noexcept;
    [[nodiscard]] const InterruptController& controller(Processor cpu) const noexcept;
    [[nodiscard]] std::uint32_t read(Processor cpu, std::uint32_t address, unsigned width) const;
    void write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width);
    std::vector<std::uint8_t> ram_;
    std::vector<std::uint8_t> arm9_bios_;
    std::vector<std::uint8_t> arm7_bios_;
    InterruptController arm9_interrupts_{Processor::arm9};
    InterruptController arm7_interrupts_{Processor::arm7};
    std::uint8_t wram_control_{};
};

} // namespace ravenemu::nds
