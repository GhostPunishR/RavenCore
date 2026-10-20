#include <ravenemu/nds/input.hpp>

#include <stdexcept>

namespace ravenemu::nds {

bool Input::matches(unsigned cpu) const noexcept {
    const auto selection = controls_[cpu] & 0x3ffU;
    const auto held = state_.pressed_keys & selection;
    return (controls_[cpu] & 0x8000U) ? held == selection : held != 0;
}

Input::Events Input::set_state(InputState state) noexcept {
    const bool opened = state_.lid_closed && !state.lid_closed;
    state.pressed_keys &= 0xfffU;
    state.touch.x &= 0xfffU;
    state.touch.y &= 0xfffU;
    state.touch.z1 &= 0xfffU;
    state.touch.z2 &= 0xfffU;
    state_ = state;
    Events events{0, opened ? 1U << 22U : 0U};
    for (unsigned cpu = 0; cpu < 2; ++cpu) {
        const bool now = matches(cpu);
        if (now && !matched_[cpu] && (controls_[cpu] & 0x4000U))
            (cpu == 0 ? events.arm9 : events.arm7) |= 1U << 12U;
        matched_[cpu] = now;
    }
    return events;
}

Input::Events Input::set_key(Key key, bool pressed) {
    const auto index = static_cast<unsigned>(key);
    if (index >= 12) throw std::invalid_argument("NDS : bouton invalide");
    const auto bit = static_cast<std::uint16_t>(1U << index);
    auto next = state_;
    next.pressed_keys = static_cast<std::uint16_t>(pressed ? next.pressed_keys | bit : next.pressed_keys & ~bit);
    return set_state(next);
}

std::uint16_t Input::extended_input() const noexcept {
    // X/Y, DEBUG et /PENIRQ actifs à zéro ; capot fermé actif à un.
    auto value = 0x7fU & ~((state_.pressed_keys >> 10U) & 3U);
    if (state_.debug_pressed) value &= ~8U;
    if (state_.pen_down) value &= ~0x40U;
    if (state_.lid_closed) value |= 0x80U;
    return static_cast<std::uint16_t>(value);
}

void Input::write_control(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept {
    const auto index = cpu == Processor::arm9 ? 0U : 1U;
    controls_[index] = static_cast<std::uint16_t>(((controls_[index] & ~mask) | (value & mask)) & 0xc3ffU);
    matched_[index] = matches(index);
}

} // namespace ravenemu::nds
