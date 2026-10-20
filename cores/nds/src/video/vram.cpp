#include <ravenemu/nds/video.hpp>

#include <algorithm>
#include "vram_layout.hpp"

namespace ravenemu::nds {
VideoMemory::VideoMemory() : bytes_(0xa4000) {}
void VideoMemory::reset() noexcept {
    std::fill(bytes_.begin(), bytes_.end(), 0);
    controls_ = {}; palettes_ = {}; oam_ = {}; registers_ = {}; power_ = 0;
    capture_control_ = 0; capture_active_ = false;
}

void VideoMemory::write_register(unsigned engine, unsigned offset, std::uint8_t value) noexcept {
    if (offset == 0x6c) value &= 0x1fU;
    else if (offset == 0x6d) value &= 0xc0U;
    else if (offset == 0x6e || offset == 0x6f) value = 0;
    else if (!(power_ & (engine == 0 ? 2U : 0x200U))) return;
    if (offset >= 0x10 && offset < 0x20 && (offset & 1U)) value &= 1U;
    if (offset == 0x2b || offset == 0x2f || offset == 0x3b || offset == 0x3f) value &= 15U;
    if (offset == 0x48 || offset == 0x49 || offset == 0x4a || offset == 0x4b || offset == 0x51) value &= 0x3fU;
    if (offset == 0x52 || offset == 0x53 || offset == 0x54) value &= 0x1fU;
    if (offset == 0x4e || offset == 0x4f || offset == 0x55) value = 0;
    registers_[engine][offset] = value;
}

std::uint8_t VideoMemory::control(VramBank bank) const noexcept {
    const auto number = static_cast<unsigned>(bank);
    return number < controls_.size() ? controls_[number] : 0;
}

void VideoMemory::set_control(VramBank bank, std::uint8_t value) noexcept {
    const auto number = static_cast<unsigned>(bank);
    if (number < controls_.size()) controls_[number] = value & detail::vram_masks[number];
}

std::uint8_t VideoMemory::arm7_status() const noexcept {
    return static_cast<std::uint8_t>(((controls_[2] & 0x87U) == 0x82U ? 1U : 0U) |
                                     ((controls_[3] & 0x87U) == 0x82U ? 2U : 0U));
}

std::optional<std::uint32_t> VideoMemory::index(unsigned bank, Processor cpu, std::uint32_t address) const noexcept {
    const auto control = controls_[bank];
    if (!(control & 0x80U)) return std::nullopt;
    const auto mode = control & 7U;
    const auto offset = (control >> 3U) & 3U;
    std::optional<std::uint32_t> base;
    if (cpu == Processor::arm7) {
        if ((bank == 2 || bank == 3) && mode == 2 && offset < 2) base = 0x06000000U + offset * 0x20000U;
    } else if (mode == 0) base = 0x06800000U + detail::vram_starts[bank];
    else if (mode == 1) {
        if (bank < 4) base = 0x06000000U + offset * 0x20000U;
        else if (bank == 4) base = 0x06000000U;
        else if (bank == 5 || bank == 6) base = 0x06000000U + (offset & 1U) * 0x4000U + (offset & 2U) * 0x8000U;
        else if (bank == 7) base = 0x06200000U;
        else base = 0x06208000U;
    } else if (mode == 2) {
        if (bank < 2 && offset < 2) base = 0x06400000U + offset * 0x20000U;
        else if (bank == 4) base = 0x06400000U;
        else if (bank == 5 || bank == 6) base = 0x06400000U + (offset & 1U) * 0x4000U + (offset & 2U) * 0x8000U;
        else if (bank == 8) base = 0x06600000U;
    } else if (mode == 4) {
        if (bank == 2) base = 0x06200000U;
        else if (bank == 3) base = 0x06600000U;
    }
    if (!base || address < *base || address - *base >= detail::vram_sizes[bank]) return std::nullopt;
    return detail::vram_starts[bank] + address - *base;
}

std::optional<std::uint8_t> VideoMemory::read_byte(Processor cpu, std::uint32_t address) const noexcept {
    if (cpu == Processor::arm9) {
        if (address >= 0x05000000 && address < 0x05000800) {
            const auto offset = address - 0x05000000U;
            return (power_ & (offset < 0x400 ? 2U : 0x200U)) ? palettes_[offset] : 0;
        }
        if (address >= 0x07000000 && address < 0x07000800) return oam_[address - 0x07000000U];
    }
    if (address < 0x06000000 || address >= 0x068a4000) return std::nullopt;
    std::optional<std::uint8_t> value;
    for (unsigned bank = 0; bank < controls_.size(); ++bank)
        if (const auto physical = index(bank, cpu, address)) value = static_cast<std::uint8_t>(value.value_or(0) | bytes_[*physical]);
    return value;
}

void VideoMemory::write_byte(Processor cpu, std::uint32_t address, std::uint8_t value) noexcept {
    if (cpu == Processor::arm9) {
        if (address >= 0x05000000 && address < 0x05000800) {
            const auto offset = address - 0x05000000U;
            if (power_ & (offset < 0x400 ? 2U : 0x200U)) palettes_[offset] = value;
            return;
        }
        if (address >= 0x07000000 && address < 0x07000800) { oam_[address - 0x07000000U] = value; return; }
    }
    for (unsigned bank = 0; bank < controls_.size(); ++bank)
        if (const auto physical = index(bank, cpu, address)) bytes_[*physical] = value;
}

} // namespace ravenemu::nds
