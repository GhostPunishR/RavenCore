#include <ravenemu/nds/memory.hpp>

#include <sstream>

namespace ravenemu::nds {
namespace {
std::string fault_message(Processor cpu, const DmaController::Transfer& transfer, const std::string& reason) {
    std::ostringstream out;
    out << "NDS " << (cpu == Processor::arm9 ? "ARM9" : "ARM7") << " DMA" << transfer.channel
        << " source=0x" << std::hex << transfer.source << " destination=0x" << transfer.destination << " : " << reason;
    return out.str();
}
} // namespace

DmaTransferError::DmaTransferError(Processor processor, DmaController::Transfer transfer, const std::string& reason)
    : Error(fault_message(processor, transfer, reason)), processor_(processor), transfer_(transfer) {}

DmaController& Memory::dma(Processor cpu) noexcept { return cpu == Processor::arm9 ? arm9_dma_ : arm7_dma_; }
const DmaController& Memory::dma(Processor cpu) const noexcept { return cpu == Processor::arm9 ? arm9_dma_ : arm7_dma_; }

bool Memory::dma_pending(Processor cpu) const noexcept { return dma(cpu).next().has_value(); }

bool Memory::step_dma(Processor cpu) {
    const auto transfer = dma(cpu).next();
    if (!transfer) return false;
    try {
        if (((transfer->control >> 23U) & 3U) == 3U) throw Error("mode source DMA réservé");
        const auto width = (transfer->control & 0x04000000U) ? 4U : 2U;
        const bool display = cpu == Processor::arm9 && ((transfer->control >> 27U) & 7U) == 4;
        if (display && (width != 4 || transfer->destination != 0x04000068 ||
                        transfer->source < 0x02000000 || transfer->source > 0x02fffffc))
            throw Error("DMA vidéo : mots 32 bits depuis la RAM principale vers DISP_MMEM_FIFO requis");
        const bool fill = cpu == Processor::arm9 && transfer->source >= 0x040000e0 && transfer->source < 0x040000f0;
        // RAM, VRAM affectée au CPU, et DMAFILL ARM9 en source.
        // Valider les deux extrémités avant toute écriture ou avancée de canal.
        for (unsigned i = 0; i < width; ++i) {
            if (!fill && !video_.read_byte(cpu, transfer->source + i)) static_cast<void>(index(cpu, transfer->source + i));
            const auto destination = transfer->destination + i;
            const bool graphics_register = cpu == Processor::arm9 &&
                ((destination >= 0x0400004c && destination <= 0x0400004f) ||
                 (destination >= 0x0400104c && destination <= 0x0400104f) ||
                 (destination >= 0x04000064 && destination <= 0x0400006b));
            if (!graphics_register && !video_.read_byte(cpu, destination)) static_cast<void>(index(cpu, destination));
        }
        const auto value = peek(cpu, transfer->source, width);
        write(cpu, transfer->destination, value, width);
        controller(cpu).request(dma(cpu).complete_unit());
        return true;
    } catch (const Error& error) {
        throw DmaTransferError(cpu, *transfer, error.what());
    }
}

} // namespace ravenemu::nds
