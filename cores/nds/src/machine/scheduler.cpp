#include <ravenemu/nds/machine.hpp>
#include <ravenemu/core.hpp>

#include <limits>
#include <stdexcept>

namespace ravenemu::nds {

void Machine::run_cycles(std::uint64_t cycles) {
    if (!entries_) throw Error("NDS : aucune cartouche chargée");
    if (cycles > std::numeric_limits<std::uint64_t>::max() - scheduler_.cycles)
        throw std::overflow_error("NDS : horloge de l'ordonnanceur débordante");
    while (cycles) {
        // Changer de phase après le succès seulement : un diagnostic sur
        // ARM7 ne réexécute pas le premier pas ARM9 lors de la reprise.
        switch (scheduler_.phase) {
        case SchedulerState::Phase::arm9_first:
            arm9_.step(memory_);
            scheduler_.phase = SchedulerState::Phase::arm7;
            break;
        case SchedulerState::Phase::arm7:
            arm7_.step(memory_);
            scheduler_.phase = SchedulerState::Phase::arm9_second;
            break;
        case SchedulerState::Phase::arm9_second:
            arm9_.step(memory_);
            scheduler_.phase = SchedulerState::Phase::peripherals;
            break;
        case SchedulerState::Phase::peripherals:
            memory_.advance_cycles(1);
            ++scheduler_.cycles;
            --cycles;
            scheduler_.phase = SchedulerState::Phase::arm9_first;
            break;
        }
    }
}

void Machine::run_frame() {
    run_cycles(memory_.cycles_until_frame());
}

} // namespace ravenemu::nds
