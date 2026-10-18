#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace ravenemu::nds {

enum class Processor { arm9, arm7 };

/** Bus RAM et BIOS NDS, sans timings ni comportement open-bus.
 * Les accès non mappés ou non alignés lèvent ravenemu::Error.
 * Les rotations des lectures ARM non alignées relèveront du CPU.
 */
class Memory {
public:
    Memory();
    // ARM7 : 16 Kio aux vecteurs bas ; ARM9 : 4 Kio aux vecteurs hauts NDS.
    void load_bios(Processor cpu, std::span<const std::uint8_t> bytes);
    void clear_ram() noexcept; // conserve les images BIOS
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
    [[nodiscard]] std::uint32_t read(Processor cpu, std::uint32_t address, unsigned width) const;
    void write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width);
    std::vector<std::uint8_t> ram_;
    std::vector<std::uint8_t> arm9_bios_;
    std::vector<std::uint8_t> arm7_bios_;
    std::uint8_t wram_control_{};
};

} // namespace ravenemu::nds
