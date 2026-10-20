#include <ravenemu/nds/memory.hpp>

namespace ravenemu::nds {

void Memory::apply_input_interrupts(Input::Events events) noexcept {
    arm9_interrupts_.request(events.arm9);
    arm7_interrupts_.request(events.arm7);
}

void Memory::set_input(InputState state) noexcept { apply_input_interrupts(input_.set_state(state)); }
void Memory::set_key(Key key, bool pressed) { apply_input_interrupts(input_.set_key(key, pressed)); }

} // namespace ravenemu::nds
