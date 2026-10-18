#include <ravenemu/nds/memory.hpp>
#include <ravenemu/core.hpp>

#include <utility>

namespace ravenemu::nds {

void Memory::load_bios(Processor cpu, std::span<const std::uint8_t> bytes) {
    const auto size = cpu == Processor::arm9 ? 0x1000U : 0x4000U;
    if (bytes.size() != size) throw RomLoadError("NDS : taille de BIOS invalide");
    std::vector<std::uint8_t> prepared(bytes.begin(), bytes.end());
    (cpu == Processor::arm9 ? arm9_bios_ : arm7_bios_) = std::move(prepared);
}

std::optional<std::uint8_t> Memory::bios_byte(Processor cpu, std::uint32_t address) const {
    const auto start = cpu == Processor::arm9 ? 0xffff0000U : 0U;
    const auto size = cpu == Processor::arm9 ? 0x1000U : 0x4000U;
    if (address < start || address - start >= size) return std::nullopt;
    const auto& bytes = cpu == Processor::arm9 ? arm9_bios_ : arm7_bios_;
    if (bytes.empty()) throw Error("NDS : BIOS absent pour la lecture du vecteur");
    return bytes[address - start];
}

} // namespace ravenemu::nds
