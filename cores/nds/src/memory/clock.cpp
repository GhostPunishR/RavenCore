#include <ravenemu/nds/memory.hpp>

#include <algorithm>

namespace ravenemu::nds {

void Memory::advance_clock(std::uint64_t cycles) noexcept {
    math_.advance_cycles(cycles);
    apply_gamecard_events(gamecard_.advance_cycles(cycles));
    if (spi_.advance_cycles(cycles)) arm7_interrupts_.request(1U << 23U);
    arm9_interrupts_.request(advance_timers(arm9_timers_, cycles));
    arm7_interrupts_.request(advance_timers(arm7_timers_, cycles));
    const auto events = lcd_.advance_cycles(cycles);
    arm9_interrupts_.request(events.arm9);
    arm7_interrupts_.request(events.arm7);
    if (events.vblank) {
        video_.finish_frame();
        arm9_dma_.pause(DmaController::Event::display_fifo);
        arm9_dma_.trigger(DmaController::Event::vblank);
        arm7_dma_.trigger(DmaController::Event::vblank);
    }
    if (events.hblank) arm9_dma_.trigger(DmaController::Event::hblank);
}

void Memory::advance_cycles(std::uint64_t cycles) {
    // Ne pas laisser une grande avance dépasser l'octet qui coupe le courant.
    cycles = std::min(cycles, spi_.cycles_until_power_off());
    constexpr auto visible_end = LcdTiming::visible_lines * LcdTiming::line_cycles;
    constexpr unsigned group_cycles = 8 * 6;
    unsigned completed_fifo_frames = 0;
    while (cycles) {
        const auto phase = lcd_.vcount() * LcdTiming::line_cycles + lcd_.cycle_in_line();
        const bool capture = (video_.capture_control_ & 0x80000000U) != 0;
        const bool fifo = video_.wants_fifo(display_control_) || arm9_dma_.listening(DmaController::Event::display_fifo);
        if (!capture && !fifo && !video_.fifo_running_) { advance_clock(cycles); return; }
        if (phase == 0) {
            // Aucun CPU ni DMA n'écrit pendant cet appel. Après deux images,
            // l'anneau stable a nécessairement remplacé toute l'image, même si
            // l'appel a commencé en milieu de balayage. Les images entières
            // suivantes peuvent être coalescées sans rejouer chaque groupe.
            if (!capture && completed_fifo_frames >= 2 && cycles >= LcdTiming::frame_cycles) {
                const auto skip = cycles - cycles % LcdTiming::frame_cycles;
                arm9_dma_.trigger(DmaController::Event::display_fifo);
                advance_clock(skip); cycles -= skip;
                continue;
            }
            if (capture) video_.capture_active_ = true;
            if (fifo) video_.fifo_running_ = true;
        }
        auto target = LcdTiming::frame_cycles;
        if (video_.capture_active_) {
            const auto line = lcd_.vcount() + (lcd_.cycle_in_line() >= LcdTiming::visible_cycles ? 1U : 0U);
            target = line < video_.capture_height() ? line * LcdTiming::line_cycles + LcdTiming::visible_cycles : visible_end;
        }
        if (video_.fifo_running_) {
            if (lcd_.cycle_in_line() == 0) arm9_dma_.trigger(DmaController::Event::display_fifo);
            const auto next = lcd_.cycle_in_line() < LcdTiming::visible_cycles ?
                lcd_.vcount() * LcdTiming::line_cycles + (lcd_.cycle_in_line() / group_cycles + 1) * group_cycles :
                (lcd_.vcount() + 1U) * LcdTiming::line_cycles;
            target = std::min(target, next);
        }
        const auto distance = target - phase;
        if (cycles < distance) { advance_clock(cycles); return; }
        const auto in_line = target % LcdTiming::line_cycles;
        const bool sample = video_.fifo_running_ && in_line > 0 && in_line <= LcdTiming::visible_cycles;
        const bool hblank = in_line == LcdTiming::visible_cycles && target < visible_end;
        if (hblank && (video_.fifo_running_ || video_.capture_active_))
            video_.finish_visible_line(target / LcdTiming::line_cycles, display_control_, graphics_power_);
        else if (sample) video_.sample_fifo_group(target / LcdTiming::line_cycles, in_line / 6 - 8);
        const bool running = video_.fifo_running_;
        advance_clock(distance); cycles -= distance;
        if (running && !video_.fifo_running_) ++completed_fifo_frames;
        if (sample && !hblank) arm9_dma_.trigger(DmaController::Event::display_fifo);
    }
}

} // namespace ravenemu::nds
