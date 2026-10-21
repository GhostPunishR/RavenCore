#include <ravenemu/nds/memory.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {

void Memory::load_gamecard(const Cartridge& cartridge, Gamecard::Mode mode) {
    gamecard_.load(cartridge,mode);
    pause_gamecard_dma();
}

void Memory::pause_gamecard_dma() noexcept {
    arm9_dma_.pause(DmaController::Event::gamecard);
    arm7_dma_.pause(DmaController::Event::gamecard);
}

void Memory::apply_gamecard_events(Gamecard::Events events) noexcept {
    if (events.completion_irq) controller(gamecard_owner()).request(1U << 19U);
    if (events.ready) dma(gamecard_owner()).trigger(DmaController::Event::gamecard);
}

std::uint32_t Memory::read_gamecard_data(Processor cpu) noexcept {
    if (cpu != gamecard_owner()) return 0;
    const auto read = gamecard_.read_data();
    if (!gamecard_.ready()) pause_gamecard_dma();
    apply_gamecard_events(read.events);
    return read.value;
}

void Memory::validate_exmem_write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width) const {
    if (cpu != Processor::arm9 || !gamecard_.busy()) return;
    for (unsigned i = 0; i < width; ++i) if (address + i == 0x04000205) {
        const auto byte = (value >> (i * 8U)) & 0xffU;
        if ((byte ^ (exmem_shared_ >> 8U)) & 8U)
            throw Error("NDS : changement de propriétaire cartouche pendant un transfert non pris en charge");
    }
}

} // namespace ravenemu::nds
