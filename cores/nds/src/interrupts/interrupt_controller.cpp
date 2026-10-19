#include <ravenemu/nds/interrupts.hpp>

namespace ravenemu::nds {

InterruptController::InterruptController(Processor processor) noexcept
    // GBATEK, DS Interrupt Control : sources communes 0-6, 8-13, 16-20 ;
    // géométrie (21) ARM9 ; RTC (7), capot/SPI/Wi-Fi (22-24) ARM7. Pas de sources DSi.
    : valid_mask_(processor == Processor::arm9 ? 0x003f3f7fU : 0x019f3fffU) {}

void InterruptController::reset() noexcept {
    enable_ = flags_ = 0;
    master_enable_ = false;
}

} // namespace ravenemu::nds
