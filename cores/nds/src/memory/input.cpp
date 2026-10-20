#include <ravenemu/nds/memory.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {

void Memory::apply_input_interrupts(Input::Events events) noexcept {
    arm9_interrupts_.request(events.arm9);
    arm7_interrupts_.request(events.arm7);
}

void Memory::set_input(InputState state) noexcept { apply_input_interrupts(input_.set_state(state)); }
void Memory::set_key(Key key, bool pressed) { apply_input_interrupts(input_.set_key(key, pressed)); }

void Memory::set_touch_position(std::uint16_t x, std::uint16_t y, bool pressed) {
    auto next = input_.state();
    if (pressed) {
        const auto& settings = firmware().settings();
        if (!settings || !settings->touch_calibration) throw Error("NDS : firmware sans étalonnage tactile valide");
        const auto sample = settings->touch_calibration->sample_at(x, y);
        next.touch.x = sample.x;
        next.touch.y = sample.y;
    }
    next.pen_down = pressed;
    set_input(next);
}

} // namespace ravenemu::nds
