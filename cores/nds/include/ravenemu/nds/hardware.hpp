#pragma once

#include <cstdint>

namespace ravenemu::nds {

enum class Processor { arm9, arm7 };

// Horloge des timers ARM9 et ARM7 (pas la fréquence d'exécution des CPU).
inline constexpr std::uint32_t timer_clock_hz = 33513982;
// Rapport nominal des horloges ; pas encore une durée par instruction.
inline constexpr std::uint32_t arm7_clock_hz = timer_clock_hz;
inline constexpr std::uint32_t arm9_clock_hz = 2U * timer_clock_hz;

} // namespace ravenemu::nds
