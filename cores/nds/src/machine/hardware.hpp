#pragma once

#include "cpu/cp15.hpp"
#include "gpu/lcd_timing.hpp"
#include "interrupts/interrupt_controller.hpp"
#include "ipc/ipc.hpp"
#include "machine/scheduler.hpp"
#include "math/divider.hpp"
#include "math/square_root.hpp"
#include "memory/memory.hpp"
#include "timers/timers.hpp"

#include <array>
#include <cstdint>

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
    // VRAMCNT_A à VRAMCNT_I (0x04000240-0x04000249, sans 0x04000247) : gardés
    // pour la projection des banques, qui viendra avec la vidéo.
    std::array<std::uint8_t, 10> vram_control{};

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
};

} // namespace ravenemu::nds
