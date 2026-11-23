#include "machine/machine.hpp"

#include <algorithm>

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
        if (*event == Event::lcd) hw.lcd.on_event(hw.scheduler, now);
    }
    hw.scheduler.schedule(Event::timers9, hw.timers9.next_overflow());
    hw.scheduler.schedule(Event::timers7, hw.timers7.next_overflow());
}

void Machine::sync_cp15() noexcept {
    bus9.cp15_changed = false;
    arm9.vector_base = hw.cp15.vector_base();
    arm9.load_interworking = hw.cp15.load_interworking();
}

} // namespace ravenemu::nds
