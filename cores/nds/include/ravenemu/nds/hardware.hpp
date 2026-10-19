#pragma once

#include <cstdint>

namespace ravenemu::nds {

enum class Processor { arm9, arm7 };

// Horloge des timers ARM9 et ARM7 (pas la fréquence d'exécution des CPU).
inline constexpr std::uint32_t timer_clock_hz = 33513982;

} // namespace ravenemu::nds
