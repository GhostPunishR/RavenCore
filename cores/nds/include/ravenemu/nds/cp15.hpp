#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ravenemu::nds {

/** Sous-ensemble ARM946E-S : identification, contrôle, MPU, vecteurs et TCM.
 * Modèle cohérent sans caches physiques ; big-endian reste refusé.
 * L'état initial conserve les vecteurs hauts du chargeur direct expérimental.
 */
class Cp15 {
public:
    enum class Access { read, write, execute };
    void reset() noexcept;
    // Permissions ARM9 seulement ; les accès hôte, ARM7 et DMA les contournent.
    [[nodiscard]] bool permits(std::uint32_t address, Access access, bool privileged) const noexcept;
    [[nodiscard]] std::uint32_t read(unsigned crn, unsigned crm, unsigned opcode2) const;
    void write(unsigned crn, unsigned crm, unsigned opcode2, std::uint32_t value);
    [[nodiscard]] std::uint32_t exception_base() const noexcept { return (control_ & 0x2000U) ? 0xffff0000U : 0U; }
    [[nodiscard]] bool load_interworking() const noexcept { return !(control_ & 0x8000U); }
    [[nodiscard]] std::optional<std::uint8_t> read_byte(std::uint32_t address, bool instruction) const noexcept;
    [[nodiscard]] bool maps_write(std::uint32_t address) const noexcept;
    bool write_byte(std::uint32_t address, std::uint8_t value) noexcept;

private:
    [[nodiscard]] std::optional<std::size_t> location(std::uint32_t address, bool instruction, bool write) const noexcept;
    std::uint32_t control_{0x2078};
    std::array<std::uint32_t, 8> protection_regions_{};
    std::array<std::uint32_t, 2> permissions_{}; // nibbles AP : données, instructions
    std::array<std::uint8_t, 2> cacheable_{};
    std::uint8_t bufferable_{};
    std::uint32_t data_region_{};
    std::uint32_t instruction_region_{};
    std::array<std::uint8_t, 0xc000> bytes_{}; // ITCM 32 Kio puis DTCM 16 Kio
};

} // namespace ravenemu::nds
