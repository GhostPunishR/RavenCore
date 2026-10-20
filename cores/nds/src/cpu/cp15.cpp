#include <ravenemu/nds/cp15.hpp>
#include <ravenemu/core.hpp>

#include <algorithm>

namespace ravenemu::nds {

void Cp15::reset() noexcept {
    control_ = 0x2078;
    data_region_ = instruction_region_ = 0;
    bytes_.fill(0);
}

std::uint32_t Cp15::read(unsigned crn, unsigned crm, unsigned opcode2) const {
    if (crn == 0 && crm == 0 && opcode2 < 8) {
        if (opcode2 == 1) return 0x0f0d2112; // caches I=8 Kio, D=4 Kio
        if (opcode2 == 2) return 0x00140180; // ITCM=32 Kio, DTCM=16 Kio
        return 0x41059461; // ARM946E-S, ARMv5TE, révision 1
    }
    if (crn == 1 && crm == 0 && opcode2 == 0) return control_;
    if (crn == 9 && crm == 1 && opcode2 < 2)
        return opcode2 == 0 ? data_region_ : instruction_region_;
    throw Error("NDS : registre CP15 non implémenté");
}

void Cp15::write(unsigned crn, unsigned crm, unsigned opcode2, std::uint32_t value) {
    if (crn == 1 && crm == 0 && opcode2 == 0) {
        // Ne pas annoncer une protection ou un ordre des octets non émulés.
        if (value & 0x81U) throw Error("NDS : MPU/big-endian CP15 non implémentés");
        control_ = (value & 0x000ff004U) | 0x78U;
        return;
    }
    if (crn == 9 && crm == 1 && opcode2 < 2) {
        const auto size = (value >> 1U) & 31U;
        if (size < 3 || size > 23) throw Error("NDS : taille virtuelle TCM réservée");
        // Les bits de base ITCM sont fixes à zéro sur NDS.
        (opcode2 == 0 ? data_region_ : instruction_region_) = value & (opcode2 == 0 ? 0xfffff03eU : 0x3eU);
        return;
    }
    throw Error("NDS : écriture CP15 non implémentée");
}

std::optional<std::size_t> Cp15::location(std::uint32_t address, bool instruction, bool write) const noexcept {
    // GBATEK : ITCM prioritaire en cas de chevauchement. Le mode de chargement
    // détourne les écritures seulement ; les lectures utilisent le bus sous-jacent.
    const auto contains = [address](std::uint32_t region, bool code) {
        const auto shift = std::min(23U, std::max(3U, (region >> 1U) & 31U));
        const auto size = std::uint64_t{512} << shift; // jusqu'à 4 Gio, sans débordement
        const auto base = code ? 0U : (region & 0xfffff000U);
        return address >= base && std::uint64_t{address - base} < size;
    };
    if ((control_ & 0x40000U) && (write || !(control_ & 0x80000U)) && contains(instruction_region_, true))
        return address & 0x7fffU;
    if (!instruction && (control_ & 0x10000U) && (write || !(control_ & 0x20000U)) && contains(data_region_, false))
        return 0x8000U + ((address - (data_region_ & 0xfffff000U)) & 0x3fffU);
    return std::nullopt;
}

std::optional<std::uint8_t> Cp15::read_byte(std::uint32_t address, bool instruction) const noexcept {
    const auto offset = location(address, instruction, false);
    return offset ? std::optional<std::uint8_t>{bytes_[*offset]} : std::nullopt;
}
bool Cp15::maps_write(std::uint32_t address) const noexcept { return location(address, false, true).has_value(); }
bool Cp15::write_byte(std::uint32_t address, std::uint8_t value) noexcept {
    const auto offset = location(address, false, true);
    if (!offset) return false;
    bytes_[*offset] = value;
    return true;
}

} // namespace ravenemu::nds
