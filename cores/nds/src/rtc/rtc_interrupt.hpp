#pragma once

#include "interrupts/interrupt_controller.hpp"
#include "machine/scheduler.hpp"
#include "rtc/real_time_clock.hpp"

#include <cstdint>

// La broche /INT de l'horloge est reliée à SI (GBATEK, « DS Serial Port ») ;
// RCNT (0x04000134) la lit en mode général (« SIO General-Purpose Mode ») :
// bits 0-3 SC, SD, SI et SO, bits 4-7 leurs directions, bit 8 interruption de
// SI, bits 14-15 mode (10b = général).
// Un front descendant de SI lève l'IRQ 7 de l'ARM7 si RCNT est en mode
// général, SI en entrée (bit 6 à 0) et son interruption permise (bit 8).
// RCNT relit ce qui y est écrit, sauf le bit 2, qui suit la broche quand SI
// est en entrée ; SC, SD et SO ne sont reliés à rien d'autre ici.
namespace ravenemu::nds {

class RtcInterrupt {
public:
    [[nodiscard]] std::uint32_t read_rcnt(const RealTimeClock& rtc, Cycles now) const noexcept {
        if ((rcnt_ & 0x40U) != 0) return rcnt_;
        return (rcnt_ & ~4U) | (rtc.interrupt_line(now) ? 4U : 0U);
    }
    void write_rcnt(std::uint32_t value, std::uint32_t mask) noexcept { rcnt_ = ((rcnt_ & ~mask) | (value & mask)) & 0xc1ffU; }

    // Après un accès à l'horloge, ou à la date que `next_change` annonçait :
    // la broche, l'IRQ sur un front descendant, et le prochain rendez-vous.
    void check(RealTimeClock& rtc, Scheduler& scheduler, InterruptController& irq7, Cycles now) {
        rtc.update(now);
        const auto high = rtc.interrupt_line(now);
        if (line_ && !high && (rcnt_ & 0xc140U) == 0x8100U) irq7.request(irq::rtc);
        line_ = high;
        const auto next = rtc.next_change(now);
        if (next == Scheduler::never) scheduler.cancel(Event::rtc);
        else scheduler.schedule(Event::rtc, next);
    }

private:
    std::uint32_t rcnt_{};
    bool line_{true};
};

} // namespace ravenemu::nds
