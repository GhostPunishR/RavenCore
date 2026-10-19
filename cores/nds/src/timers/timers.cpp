#include <ravenemu/nds/memory.hpp>

namespace ravenemu::nds {

Memory::TimerBank& Memory::timers(Processor cpu) noexcept {
    return cpu == Processor::arm9 ? arm9_timers_ : arm7_timers_;
}

const Memory::TimerBank& Memory::timers(Processor cpu) const noexcept {
    return cpu == Processor::arm9 ? arm9_timers_ : arm7_timers_;
}

std::uint8_t Memory::timer_byte(Processor cpu, unsigned offset) const noexcept {
    const auto& timer = timers(cpu)[offset / 4U];
    switch (offset & 3U) {
    case 0: return static_cast<std::uint8_t>(timer.counter);
    case 1: return static_cast<std::uint8_t>(timer.counter >> 8U);
    case 2: return timer.control;
    default: return 0;
    }
}

void Memory::write_timer_byte(Processor cpu, unsigned offset, std::uint8_t value) noexcept {
    const auto number = offset / 4U;
    auto& timer = timers(cpu)[number];
    switch (offset & 3U) {
    case 0:
        timer.reload = static_cast<std::uint16_t>((timer.reload & 0xff00U) | value);
        break;
    case 1:
        timer.reload = static_cast<std::uint16_t>((timer.reload & 0x00ffU) | (static_cast<unsigned>(value) << 8U));
        break;
    case 2: {
        const auto control = static_cast<std::uint8_t>(value & (number == 0 ? 0xc3U : 0xc7U));
        if ((control & 0x80U) && !(timer.control & 0x80U)) timer.counter = timer.reload;
        // Modèle déterministe : une modification de marche/source/diviseur repart
        // à phase zéro. Changer uniquement IRQ ne redémarre pas le préscaler.
        if ((control ^ timer.control) & 0x87U) timer.phase = 0;
        timer.control = control;
        break;
    }
    default: break; // octet haut du contrôle réservé
    }
}

std::uint32_t Memory::advance_timers(TimerBank& bank, std::uint64_t cycles) noexcept {
    constexpr std::array<unsigned, 4> divisors{1, 64, 256, 1024};
    std::uint64_t previous_overflows{};
    std::uint32_t sources{};
    for (unsigned i = 0; i < bank.size(); ++i) {
        auto& timer = bank[i];
        if (!(timer.control & 0x80U)) { previous_overflows = 0; continue; }
        std::uint64_t ticks;
        if (timer.control & 4U) ticks = previous_overflows;
        else {
            const auto divisor = divisors[timer.control & 3U];
            // Ne pas additionner cycles et phase : cycles peut valoir UINT64_MAX.
            const auto remainder = cycles % divisor + timer.phase;
            ticks = cycles / divisor + remainder / divisor;
            timer.phase = static_cast<std::uint16_t>(remainder % divisor);
        }
        const auto distance = 0x10000U - static_cast<unsigned>(timer.counter);
        previous_overflows = 0;
        if (ticks < distance) timer.counter = static_cast<std::uint16_t>(timer.counter + ticks);
        else {
            const auto remaining = ticks - distance;
            const auto period = 0x10000U - static_cast<unsigned>(timer.reload);
            previous_overflows = 1 + remaining / period;
            timer.counter = static_cast<std::uint16_t>(timer.reload + remaining % period);
            if (timer.control & 0x40U) sources |= 1U << (3U + i);
        }
    }
    return sources;
}

} // namespace ravenemu::nds
