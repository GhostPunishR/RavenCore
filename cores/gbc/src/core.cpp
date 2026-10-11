#include <ravenemu/gbc/core.hpp>
#include <ravenemu/gb/core.hpp>

namespace ravenemu::gbc {
std::unique_ptr<Core> make_core(std::span<const std::uint8_t> boot_rom) {
    return gb::make_core(gb::HardwareModel::cgb, boot_rom);
}
} // namespace ravenemu::gbc
