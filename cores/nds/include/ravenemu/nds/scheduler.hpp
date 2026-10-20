#pragma once

#include <cstdint>

namespace ravenemu::nds {

/** Horloge fonctionnelle expérimentale, à timer_clock_hz.
 * Un cycle commun offre deux pas ARM9 et un pas ARM7, sans encore calculer
 * les durées propres aux instructions, aux transferts ou aux attentes mémoire.
 * La phase conserve le travail terminé si le pas suivant échoue.
 */
struct SchedulerState {
    enum class Phase { arm9_first, arm7, arm9_second, peripherals };
    std::uint64_t cycles{}; // cycles communs terminés par run_cycles/run_frame uniquement
    Phase phase{Phase::arm9_first};
    bool operator==(const SchedulerState&) const = default;
};

} // namespace ravenemu::nds
