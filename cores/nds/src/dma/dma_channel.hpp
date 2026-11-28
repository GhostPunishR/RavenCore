#pragma once

#include "interrupts/interrupt_controller.hpp"

#include <array>
#include <cstdint>

// DMA du DS : quatre canaux par CPU, comme ceux du GBA (GBATEK, « DS DMA
// Transfers », « GBA DMA Transfers »). Registres à 0x040000B0 + 12 × canal :
// source, destination, puis DMACNT (nombre d'unités en bits 0-20, contrôle en
// bits 21-31). Un transfert s'exécute d'un bloc au déclenchement, par le bus
// de son CPU, qui en paie les accès : le CPU attend la fin, comme sur la
// console quand il ne travaille pas en TCM.
namespace ravenemu::nds {

enum class DmaTiming : std::uint8_t {
    immediate,
    vblank,
    hblank,
    display_start,
    main_memory_display,
    cartridge,
    gba_slot,
    geometry_fifo,
    wifi,
};

struct DmaChannel {
    std::uint32_t source{};
    std::uint32_t destination{};
    std::uint32_t control{};
    // État interne, recopié des registres à l'activation.
    std::uint32_t current_source{};
    std::uint32_t current_destination{};
    std::uint32_t remaining{};

    [[nodiscard]] bool enabled() const noexcept { return (control & 0x8000'0000U) != 0; }
};

// Modes de départ : 3 bits (27-29) sur l'ARM9, 2 bits (28-29) sur l'ARM7.
constexpr DmaTiming dma_timing(std::uint32_t control, bool arm9, unsigned channel) noexcept {
    if (arm9) return static_cast<DmaTiming>((control >> 27U) & 7U);
    switch ((control >> 28U) & 3U) {
    case 0: return DmaTiming::immediate;
    case 1: return DmaTiming::vblank;
    case 2: return DmaTiming::cartridge;
    default: return (channel & 1U) == 0 ? DmaTiming::wifi : DmaTiming::gba_slot;
    }
}

// Nombre d'unités ; 0 vaut le maximum : 0x200000 sur l'ARM9, 0x4000 (canaux 0
// à 2) ou 0x10000 (canal 3) sur l'ARM7.
constexpr std::uint32_t dma_units(std::uint32_t control, bool arm9, unsigned channel) noexcept {
    const auto bits = arm9 ? 21U : (channel == 3 ? 16U : 14U);
    const auto count = control & ((1U << bits) - 1U);
    return count != 0 ? count : 1U << bits;
}

// Transfert d'un canal : unités de 16 ou 32 bits (bit 26), pas de la source
// (bits 23-24) et de la destination (bits 21-22) : +, -, fixe, et pour la
// destination + avec rechargement. Répétition (bit 25) hors départ immédiat ;
// IRQ en fin de transfert (bit 30). `limit` borne les unités d'un départ :
// la DMA de géométrie envoie au plus 112 mots à la fois, le canal restant
// actif jusqu'à la dernière unité.
template <typename BusType>
void run_dma(BusType& bus, DmaChannel& channel, unsigned index, bool arm9, InterruptController& irq,
             std::uint32_t limit = 0xffff'ffffU) {
    const auto words = (channel.control & (1U << 26U)) != 0;
    const auto width = words ? 4U : 2U;
    const auto step = [width](std::uint32_t mode) -> std::uint32_t {
        if (mode == 1) return 0U - width;
        return mode == 2 ? 0U : width;
    };
    const auto source_step = step((channel.control >> 23U) & 3U);
    const auto destination_mode = (channel.control >> 21U) & 3U;
    const auto destination_step = step(destination_mode);
    auto source = channel.current_source & ~(width - 1U);
    auto destination = channel.current_destination & ~(width - 1U);
    const auto units = channel.remaining < limit ? channel.remaining : limit;
    for (std::uint32_t unit = 0; unit < units; ++unit) {
        if (words) bus.dma_write32(destination, bus.dma_read32(source));
        else bus.dma_write16(destination, bus.dma_read16(source));
        source += source_step;
        destination += destination_step;
    }
    channel.current_source = source;
    channel.current_destination = destination;
    channel.remaining -= units;
    if (channel.remaining != 0) return;
    const auto repeat = (channel.control & (1U << 25U)) != 0;
    if (repeat && dma_timing(channel.control, arm9, index) != DmaTiming::immediate) {
        channel.remaining = dma_units(channel.control, arm9, index);
        if (destination_mode == 3) channel.current_destination = channel.destination;
    } else {
        channel.control &= ~0x8000'0000U;
    }
    if ((channel.control & (1U << 30U)) != 0) irq.request(irq::dma0 + index);
}

// Écriture d'un mot des registres de DMA (offset 0x0B0 à 0x0DC). Rend vrai
// si l'écriture active un canal à départ immédiat.
inline bool write_dma_register(std::array<DmaChannel, 4>& channels, std::uint32_t offset, std::uint32_t value,
                               std::uint32_t mask, bool arm9) {
    const auto index = (offset - 0x0b0U) / 12U;
    auto& channel = channels[index];
    const auto merge = [&](std::uint32_t old) { return (old & ~mask) | (value & mask); };
    const auto address_mask = arm9 ? 0x0fff'ffffU : (index == 0 ? 0x07ff'ffffU : 0x0fff'ffffU);
    switch ((offset - 0x0b0U) % 12U) {
    case 0: channel.source = merge(channel.source) & address_mask; return false;
    case 4: channel.destination = merge(channel.destination) & (arm9 || index == 3 ? 0x0fff'ffffU : 0x07ff'ffffU); return false;
    default: break;
    }
    const auto was_enabled = channel.enabled();
    channel.control = merge(channel.control);
    if (!channel.enabled() || was_enabled) return false;
    channel.current_source = channel.source;
    channel.current_destination = channel.destination;
    channel.remaining = dma_units(channel.control, arm9, index);
    return dma_timing(channel.control, arm9, index) == DmaTiming::immediate;
}

} // namespace ravenemu::nds
