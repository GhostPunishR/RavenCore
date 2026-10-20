#include <ravenemu/nds/cp15.hpp>

namespace ravenemu::nds {

bool Cp15::permits(std::uint32_t address, Access access, bool privileged) const noexcept {
    if (!(control_ & 1U)) return true;
    for (unsigned i = 8; i-- > 0;) {
        const auto region = protection_regions_[i];
        if (!(region & 1U)) continue;
        const auto size = std::uint64_t{2} << ((region >> 1U) & 31U);
        const auto base = region & 0xfffff000U;
        if (address < base || std::uint64_t{address - base} >= size) continue;
        const auto ap = (permissions_[access == Access::execute ? 1U : 0U] >> (i * 4U)) & 15U;
        switch (ap) {
        case 1: return privileged;
        case 2: return privileged || access != Access::write;
        case 3: return true;
        case 5: return privileged && access != Access::write;
        case 6: return access != Access::write;
        default: return false;
        }
    }
    return false; // aucune région : arrière-plan sans permission, même en privilégié
}

} // namespace ravenemu::nds
