#include <ravenemu/nds/memory.hpp>

namespace ravenemu::nds {

void Memory::advance_cycles(std::uint64_t cycles) noexcept {
    arm9_interrupts_.request(advance_timers(arm9_timers_, cycles));
    arm7_interrupts_.request(advance_timers(arm7_timers_, cycles));
    const auto events = lcd_.advance_cycles(cycles);
    arm9_interrupts_.request(events.arm9);
    arm7_interrupts_.request(events.arm7);
    if (events.vblank) {
        arm9_dma_.trigger(DmaController::Event::vblank);
        arm7_dma_.trigger(DmaController::Event::vblank);
    }
    if (events.hblank) arm9_dma_.trigger(DmaController::Event::hblank);
}

} // namespace ravenemu::nds
