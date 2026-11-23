#include "machine/machine.hpp"

#include <algorithm>
#include <array>

namespace ravenemu::nds {

Machine::Machine(std::vector<std::uint8_t> rom) : header(parse_header(rom)), rom_(std::move(rom)) {
    bus9.swi = {[](void* bios, std::uint32_t comment, bool thumb) {
                    return static_cast<Bios<Arm9, Bus9, true>*>(bios)->call(comment, thumb);
                },
                &bios9};
    bus7.swi = {[](void* bios, std::uint32_t comment, bool thumb) {
                    return static_cast<Bios<Arm7, Bus7, false>*>(bios)->call(comment, thumb);
                },
                &bios7};
    direct_boot();
    hw.lcd.start(hw.scheduler, hw.now);
}

void Machine::run_until(Cycles target) {
    while (hw.now < target) {
        const auto end = std::min({target, hw.scheduler.next(), hw.now + max_slice});
        run_arm9(end);
        run_arm7(end);
        hw.now = end;
        settle(end);
    }
}

void Machine::run_frame() {
    const auto frame = hw.lcd.frames();
    while (hw.lcd.frames() == frame) run_for(LcdTiming::line_cycles);
}

// L'ARM9 ne sort de Halt que si IME le permet (GBATEK, « BIOS Halt Functions »).
void Machine::run_arm9(Cycles end) {
    const auto limit = end * 2;
    while (hw.arm9_cycles < limit) {
        if (hw.arm9_halted) {
            if (!hw.irq9.line()) {
                hw.arm9_cycles = limit;
                return;
            }
            hw.arm9_halted = false;
        }
        arm9.set_irq_line(hw.irq9.line());
        arm9.step();
        if (bus9.cp15_changed) sync_cp15();
    }
}

// L'ARM7 sort de Halt dès que IE ET IF n'est plus nul, quel que soit IME.
void Machine::run_arm7(Cycles end) {
    while (hw.arm7_cycles < end) {
        if (hw.arm7_halted) {
            if (!hw.irq7.pending()) {
                hw.arm7_cycles = end;
                return;
            }
            hw.arm7_halted = false;
        }
        arm7.set_irq_line(hw.irq7.line());
        arm7.step();
    }
}

void Machine::settle(Cycles now) {
    hw.timers9.advance(now);
    hw.timers7.advance(now);
    while (const auto event = hw.scheduler.take_due(now)) {
        if (*event == Event::lcd) on_lcd_event(hw.lcd.on_event(hw.scheduler, now));
    }
    hw.scheduler.schedule(Event::timers9, hw.timers9.next_overflow());
    hw.scheduler.schedule(Event::timers7, hw.timers7.next_overflow());
}

// Débuts de ligne : rendu et DMA de V-Blank ; H-Blank : DMA de H-Blank de
// l'ARM9, sur les lignes visibles seulement (GBATEK).
void Machine::on_lcd_event(LcdTiming::Phase phase) {
    const auto line = hw.lcd.line();
    switch (phase) {
    case LcdTiming::Phase::line_start:
        if (line < LcdTiming::visible_lines) render_line(line);
        if (line == LcdTiming::visible_lines) {
            bus9.trigger_dma(DmaTiming::vblank);
            bus7.trigger_dma(DmaTiming::vblank);
        }
        break;
    case LcdTiming::Phase::hblank9:
        if (line < LcdTiming::visible_lines) bus9.trigger_dma(DmaTiming::hblank);
        break;
    case LcdTiming::Phase::hblank7: break;
    }
}

// Chaque moteur dessine la ligne au début de celle-ci ; POWCNT1 bit 15 place
// le moteur A sur l'écran du haut.
void Machine::render_line(unsigned line) {
    std::array<std::uint16_t, Engine2d::width> a{};
    std::array<std::uint16_t, Engine2d::width> b{};
    hw.engine_a.render_line(line, hw.vram, hw.memory, a);
    hw.engine_b.render_line(line, hw.vram, hw.memory, b);
    const auto a_on_top = (hw.power_control9 & 0x8000U) != 0;
    auto* top = &hw.frame[line * Engine2d::width];
    auto* bottom = &hw.frame[Hardware::screen_pixels + line * Engine2d::width];
    const auto brightness_a = hw.engine_a.master_brightness();
    const auto brightness_b = hw.engine_b.master_brightness();
    for (unsigned x = 0; x < Engine2d::width; ++x) {
        const auto pixel_a = to_argb(a[x], brightness_a);
        const auto pixel_b = to_argb(b[x], brightness_b);
        top[x] = a_on_top ? pixel_a : pixel_b;
        bottom[x] = a_on_top ? pixel_b : pixel_a;
    }
}

void Machine::sync_cp15() noexcept {
    bus9.cp15_changed = false;
    arm9.vector_base = hw.cp15.vector_base();
    arm9.load_interworking = hw.cp15.load_interworking();
}

} // namespace ravenemu::nds
