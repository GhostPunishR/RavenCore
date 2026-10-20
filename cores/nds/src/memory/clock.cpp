#include <ravenemu/nds/memory.hpp>

#include <algorithm>

namespace ravenemu::nds {

void Memory::advance_clock(std::uint64_t cycles) noexcept {
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

void Memory::advance_cycles(std::uint64_t cycles) {
    constexpr auto visible_end = LcdTiming::visible_lines * LcdTiming::line_cycles;
    while (cycles && (video_.capture_control_ & 0x80000000U)) {
        const auto phase = lcd_.vcount() * LcdTiming::line_cycles + lcd_.cycle_in_line();
        if (!video_.capture_active_) {
            if (phase != 0) {
                const auto count = std::min(cycles, static_cast<std::uint64_t>(LcdTiming::frame_cycles - phase));
                advance_clock(count); cycles -= count;
                continue;
            }
            video_.capture_active_ = true;
        }
        const auto line = lcd_.vcount() + (lcd_.cycle_in_line() >= LcdTiming::visible_cycles ? 1U : 0U);
        const bool capture = line < video_.capture_height();
        const auto target = capture ? line * LcdTiming::line_cycles + LcdTiming::visible_cycles : visible_end;
        const auto distance = target - phase;
        if (cycles < distance) { advance_clock(cycles); return; }
        // Valider/rendre la ligne avant de franchir son événement : une erreur ne
        // modifie ni ses pixels destination ni les horloges de cette unité.
        if (capture) video_.capture_line(line, display_control_, graphics_power_);
        advance_clock(distance); cycles -= distance;
        if (!capture) {
            video_.capture_control_ &= 0x7fffffffU;
            video_.capture_active_ = false;
        }
    }
    advance_clock(cycles);
}

} // namespace ravenemu::nds
