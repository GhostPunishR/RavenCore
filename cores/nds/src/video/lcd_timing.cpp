#include <ravenemu/nds/lcd_timing.hpp>

namespace ravenemu::nds {
namespace {
// Franchissement dans (départ, arrivée], y compris pour UINT64_MAX cycles.
bool crosses(std::uint32_t phase, std::uint32_t period, std::uint32_t event, std::uint64_t cycles) noexcept {
    const auto distance = (event + period - phase % period) % period;
    return cycles >= (distance == 0 ? period : distance);
}
} // namespace

void LcdTiming::reset() noexcept { phase_ = 0; controls_ = {}; }
std::uint16_t LcdTiming::vcount() const noexcept { return static_cast<std::uint16_t>(phase_ / line_cycles); }

std::uint16_t LcdTiming::compare(std::uint16_t control) noexcept {
    return static_cast<std::uint16_t>((control >> 8U) | ((control & 0x80U) << 1U));
}

std::uint16_t LcdTiming::status(Processor cpu) const noexcept {
    const auto control = controls_[cpu == Processor::arm9 ? 0 : 1];
    const auto line = vcount();
    const auto hblank_flag_cycle = cpu == Processor::arm9 ? 1606U : 1613U;
    return static_cast<std::uint16_t>(control | (line >= visible_lines && line < total_lines - 1 ? 1U : 0U) |
        (cycle_in_line() >= hblank_flag_cycle ? 2U : 0U) | (line == compare(control) ? 4U : 0U));
}

void LcdTiming::write_status(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept {
    auto& control = controls_[cpu == Processor::arm9 ? 0 : 1];
    const auto writable = mask & 0xffb8U;
    control = static_cast<std::uint16_t>((control & ~writable) | (value & writable));
}

LcdTiming::Events LcdTiming::advance_cycles(std::uint64_t cycles) noexcept {
    Events result;
    result.vblank = crosses(phase_, frame_cycles, visible_lines * line_cycles, cycles);
    const bool hblank_irq = crosses(phase_, line_cycles, visible_cycles, cycles);
    auto next_visible_hblank = phase_ / line_cycles * line_cycles + visible_cycles;
    if (next_visible_hblank <= phase_) next_visible_hblank += line_cycles;
    if (next_visible_hblank >= visible_lines * line_cycles) next_visible_hblank = frame_cycles + visible_cycles;
    result.hblank = cycles >= next_visible_hblank - phase_;
    const auto sources = [&](std::uint16_t control) {
        const auto target = compare(control);
        return (result.vblank && (control & 8U) ? 1U : 0U) |
            (hblank_irq && (control & 0x10U) ? 2U : 0U) |
            (target < total_lines && (control & 0x20U) && crosses(phase_, frame_cycles, target * line_cycles, cycles) ? 4U : 0U);
    };
    result.arm9 = sources(controls_[0]);
    result.arm7 = sources(controls_[1]);
    // Réduire avant d'additionner pour accepter UINT64_MAX sans débordement.
    phase_ = static_cast<std::uint32_t>((phase_ + cycles % frame_cycles) % frame_cycles);
    return result;
}

} // namespace ravenemu::nds
