#pragma once

#include "cartridge/game_card.hpp"
#include "cpu/cp15.hpp"
#include "dma/dma_channel.hpp"
#include "gpu/engine2d.hpp"
#include "gpu/vram.hpp"
#include "gpu/lcd_timing.hpp"
#include "interrupts/interrupt_controller.hpp"
#include "ipc/ipc.hpp"
#include "machine/scheduler.hpp"
#include "math/divider.hpp"
#include "math/square_root.hpp"
#include "memory/memory.hpp"
#include "rtc/real_time_clock.hpp"
#include "spi/spi_bus.hpp"
#include "timers/timers.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Le matériel du DS hors processeurs : ce que les deux bus partagent.
namespace ravenemu::nds {

struct Hardware {
    Scheduler scheduler;
    InterruptController irq9{irq::arm9_bits};
    InterruptController irq7{irq::arm7_bits};
    Ipc ipc{irq9, irq7};
    Divider divider;
    SquareRoot square_root;
    Timers timers9{irq9};
    Timers timers7{irq7};
    LcdTiming lcd{irq9, irq7};
    Cp15 cp15;
    Memory memory;
    GameCard card;
    SpiBus spi;
    RealTimeClock rtc;

    // KEYINPUT (bits 0-9, 0 = appuyé) ; EXTKEYIN de l'ARM7 : X, Y, crayon
    // posé (bit 6) et couvercle fermé (bit 7), les bits 2, 4 et 5 levés.
    std::uint32_t keys{0x03ffU};
    std::uint32_t extended_keys{0x007fU};
    std::array<std::uint32_t, 2> key_control{};
    // EXMEMCNT : bits 0-6 propres à chaque CPU, bits 7-15 écrits par l'ARM9.
    std::array<std::uint32_t, 2> external_memory{};
    std::array<std::uint32_t, 2> post_boot{};
    std::uint32_t power_control9{};  // POWCNT1
    std::uint32_t power_control7{1}; // POWCNT2 : haut-parleurs
    std::array<DmaChannel, 4> dma9{};
    std::array<DmaChannel, 4> dma7{};
    std::array<std::uint32_t, 4> dma_fill{};  // DMA0FILL à DMA3FILL (ARM9)
    Vram vram;
    Engine2d engine_a{true};
    Engine2d engine_b{false};
    // Les deux écrans en ARGB8888, 256 × 192 chacun : celui du haut, puis
    // celui du bas. POWCNT1 bit 15 met le moteur A en haut.
    static constexpr std::size_t screen_pixels = 256 * 192;
    std::vector<std::uint32_t> frame = std::vector<std::uint32_t>(2 * screen_pixels, 0xff00'0000U);

    // Temps : `now` en cycles système ; chaque CPU compte les siens et
    // avance jusqu'à la fin de la tranche en cours.
    Cycles now{};
    Cycles arm9_cycles{};
    Cycles arm7_cycles{};
    bool arm9_halted{};
    bool arm7_halted{};

    [[nodiscard]] Cycles arm9_time() const noexcept { return arm9_cycles >> 1U; }
    [[nodiscard]] Cycles arm7_time() const noexcept { return arm7_cycles; }
    // Slot GBA attribué à l'ARM7 (EXMEMCNT bit 7 de l'ARM9).
    [[nodiscard]] bool gba_slot_arm7() const noexcept { return (external_memory[0] & 0x80U) != 0; }
    // Slot de la cartouche DS attribué à l'ARM7 (EXMEMCNT bit 11 de l'ARM9).
    [[nodiscard]] bool card_arm7() const noexcept { return (external_memory[0] & 0x800U) != 0; }
};

} // namespace ravenemu::nds
