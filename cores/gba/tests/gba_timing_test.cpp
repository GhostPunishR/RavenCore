#include "machine/machine.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <cstdint>
#include <memory>
#include <vector>

using ravenemu::testing::check;

namespace {

void frame_overshoot_does_not_accumulate() {
    // Cette ROM n'utilise ni DMA ni pause : seule une instruction peut dépasser
    // la frontière. La borne reste volontairement bien supérieure à son coût.
    constexpr int maximum_single_step_overshoot = 64;
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(
        ravenemu::testing::minimal_gba_rom()
    );
    ravenemu::gba::Machine machine(
        rom, ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );

    for (int frame = 0; frame < 120; ++frame) {
        machine.run_frame();
        const auto ppu_state = machine.ppu.state_fields();
        check(ppu_state[0] == 0, "la cadence GBA a dépassé la frontière de trame");
        check(
            ppu_state[1] >= 0 && ppu_state[1] < maximum_single_step_overshoot,
            "le dépassement de cycles GBA s'accumule entre les trames"
        );
    }
}

void long_dma_stops_at_frame_boundary() {
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(
        ravenemu::testing::minimal_gba_rom()
    );
    ravenemu::gba::Machine machine(
        rom, ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );

    const auto pc_before = machine.cpu.state.regs[15];
    machine.bus.write32(ravenemu::gba::i32(0x040000d4U), ravenemu::gba::i32(0x02000000U));
    machine.bus.write32(ravenemu::gba::i32(0x040000d8U), ravenemu::gba::i32(0x02020000U));
    machine.bus.write16(ravenemu::gba::i32(0x040000dcU), 32768);
    machine.bus.write16(ravenemu::gba::i32(0x040000deU), 0x8400); // canal 3, immédiat, 32 bits

    machine.run_frame();

    const auto ppu_state = machine.ppu.state_fields();
    check(ppu_state[0] == 0 && ppu_state[1] == 0, "le DMA a franchi la frontière de trame");
    check(machine.dma.active(), "le DMA long n'a pas conservé son transfert en cours");
    check(machine.cpu.state.regs[15] == pc_before, "le CPU a repris le bus avant la fin du DMA");
}

void dma_effects_follow_elapsed_cycles() {
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(
        ravenemu::testing::minimal_gba_rom()
    );
    ravenemu::gba::Machine machine(
        rom, ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    constexpr auto source = 0x03000100U;
    constexpr auto destination = 0x03000200U;
    constexpr auto first = 0x12345678U;
    constexpr auto second = 0x0badf00dU;
    const auto pc_before = machine.cpu.state.regs[15];

    machine.bus.write32(ravenemu::gba::i32(source), ravenemu::gba::i32(first));
    machine.bus.write32(ravenemu::gba::i32(source + 4U), ravenemu::gba::i32(second));
    machine.bus.write32(ravenemu::gba::i32(0x040000b0U), ravenemu::gba::i32(source));
    machine.bus.write32(ravenemu::gba::i32(0x040000b4U), ravenemu::gba::i32(destination));
    machine.bus.write16(ravenemu::gba::i32(0x040000b8U), 2);
    machine.bus.write16(
        ravenemu::gba::i32(0x040000baU),
        0xc400 // activé, IRQ de fin, 32 bits, déclenchement immédiat
    );

    check(machine.dma.active(), "le DMA doit prendre le bus dès son déclenchement");
    check(machine.bus.read32(ravenemu::gba::i32(destination)) == 0, "le premier mot DMA est visible trop tôt");
    check((machine.interrupts.flags & (1 << ravenemu::gba::InterruptController::dma0)) == 0,
          "l'interruption DMA a été levée avant la fin");
    check((machine.bus.io[0x0bb] & 0x80U) != 0, "le canal DMA a été désarmé avant la fin");

    machine.run_frame(4); // prise du bus (2), puis lecture/écriture du premier mot
    check(machine.bus.read32(ravenemu::gba::i32(destination)) == ravenemu::gba::i32(first),
          "le premier mot DMA n'est pas devenu visible");
    check(machine.bus.read32(ravenemu::gba::i32(destination + 4U)) == 0,
          "le second mot DMA est visible avant ses cycles");
    check(machine.dma.active(), "le DMA s'est terminé après le premier mot");
    check((machine.interrupts.flags & (1 << ravenemu::gba::InterruptController::dma0)) == 0,
          "l'interruption DMA a été levée entre les mots");

    machine.run_frame(1); // lecture du second mot, sans écriture
    const auto dma_state = machine.dma.export_state();
    machine.bus.write32(ravenemu::gba::i32(source + 4U), 0);
    machine.dma.import_state(dma_state);
    check(machine.bus.read32(ravenemu::gba::i32(destination + 4U)) == 0,
          "le second mot DMA est visible avant la fin de son écriture");

    machine.run_frame(1);
    check(machine.bus.read32(ravenemu::gba::i32(destination + 4U)) == ravenemu::gba::i32(second),
          "la valeur lue par le DMA n'a pas survécu à la restauration");
    check(!machine.dma.active(), "le DMA retient encore le bus après son dernier mot");
    check((machine.interrupts.flags & (1 << ravenemu::gba::InterruptController::dma0)) != 0,
          "l'interruption DMA n'a pas été levée à la fin");
    check((machine.bus.io[0x0bb] & 0x80U) == 0, "le canal DMA est resté armé après la fin");
    check(machine.cpu.state.regs[15] == pc_before, "le CPU a avancé pendant le DMA");
}

void vblank_dma_starts_without_copying_immediately() {
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(
        ravenemu::testing::minimal_gba_rom()
    );
    ravenemu::gba::Machine machine(
        rom, ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    machine.bus.write32(ravenemu::gba::i32(0x03000100U), ravenemu::gba::i32(0x12345678U));
    machine.bus.write32(ravenemu::gba::i32(0x040000b0U), ravenemu::gba::i32(0x03000100U));
    machine.bus.write32(ravenemu::gba::i32(0x040000b4U), ravenemu::gba::i32(0x03000200U));
    machine.bus.write16(ravenemu::gba::i32(0x040000b8U), 1);
    machine.bus.write16(ravenemu::gba::i32(0x040000baU), 0x9400); // VBlank, 32 bits

    machine.ppu.tick(161 * 1232);
    check(machine.dma.active(), "le VBlank n'a pas déclenché le DMA");
    check(machine.bus.read32(ravenemu::gba::i32(0x03000200U)) == 0,
          "le DMA VBlank a copié avant que ses cycles ne s'écoulent");

    machine.run_frame(4);
    check(machine.bus.read32(ravenemu::gba::i32(0x03000200U)) == ravenemu::gba::i32(0x12345678U),
          "le DMA VBlank n'a pas copié son mot");
}

void sound_fifo_dma_progresses_by_word() {
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(
        ravenemu::testing::minimal_gba_rom()
    );
    ravenemu::gba::Machine machine(
        rom, ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    for (unsigned word = 0; word < 4; ++word) {
        machine.bus.write32(
            ravenemu::gba::i32(0x02000000U + word * 4U),
            ravenemu::gba::i32(0x40404040U)
        );
    }
    machine.bus.write32(ravenemu::gba::i32(0x040000bcU), ravenemu::gba::i32(0x02000000U));
    machine.bus.write32(ravenemu::gba::i32(0x040000c0U), ravenemu::gba::i32(0x040000a0U));
    machine.bus.write16(ravenemu::gba::i32(0x040000c4U), 4);
    machine.bus.write16(ravenemu::gba::i32(0x040000c6U), 0xb400); // spécial, 32 bits

    machine.dma.trigger_sound_fifo(0);
    check(machine.apu.fifo_size(0) == 0, "la FIFO DMA a été remplie au déclenchement");
    machine.run_frame(29);
    check(machine.apu.fifo_size(0) == 12, "la FIFO DMA n'a pas progressé mot par mot");
    check(machine.dma.active(), "le DMA FIFO s'est terminé avant sa dernière écriture");
    machine.run_frame(1);
    check(machine.apu.fifo_size(0) == 16, "le DMA FIFO n'a pas écrit son dernier mot");
    check(!machine.dma.active(), "le DMA FIFO retient encore le bus");
    check((machine.bus.io[0x0c7] & 0x80U) != 0, "le canal FIFO a été désarmé");
}

void lower_priority_dma_waits_for_current_transfer() {
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(
        ravenemu::testing::minimal_gba_rom()
    );
    ravenemu::gba::Machine machine(
        rom, ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    machine.bus.write32(ravenemu::gba::i32(0x03000100U), ravenemu::gba::i32(0x11111111U));
    machine.bus.write32(ravenemu::gba::i32(0x03000110U), ravenemu::gba::i32(0x33333333U));

    // Le canal 0 prend le bus avant que le canal 3, moins prioritaire, soit demandé.
    machine.bus.write32(ravenemu::gba::i32(0x040000b0U), ravenemu::gba::i32(0x03000100U));
    machine.bus.write32(ravenemu::gba::i32(0x040000b4U), ravenemu::gba::i32(0x03000200U));
    machine.bus.write16(ravenemu::gba::i32(0x040000b8U), 1);
    machine.bus.write16(ravenemu::gba::i32(0x040000baU), 0x8400);
    machine.bus.write32(ravenemu::gba::i32(0x040000d4U), ravenemu::gba::i32(0x03000110U));
    machine.bus.write32(ravenemu::gba::i32(0x040000d8U), ravenemu::gba::i32(0x03000210U));
    machine.bus.write16(ravenemu::gba::i32(0x040000dcU), 1);
    machine.bus.write16(ravenemu::gba::i32(0x040000deU), 0x8400);

    const auto queued_state = machine.dma.export_state();
    machine.dma.import_state(queued_state);
    machine.run_frame(4);
    check(machine.bus.read32(ravenemu::gba::i32(0x03000200U)) == ravenemu::gba::i32(0x11111111U),
          "le premier DMA en file n'a pas terminé");
    check(machine.bus.read32(ravenemu::gba::i32(0x03000210U)) == 0,
          "le DMA en attente a copié pendant le canal actif");
    check(machine.dma.active(), "le DMA en attente n'a pas pris le bus");

    machine.run_frame(4);
    check(machine.bus.read32(ravenemu::gba::i32(0x03000210U)) == ravenemu::gba::i32(0x33333333U),
          "le DMA en attente n'a pas été exécuté");
    check(!machine.dma.active(), "la file DMA n'est pas vide");
}

void sound_dma_preempts_and_resumes_lower_priority_transfer() {
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(
        ravenemu::testing::minimal_gba_rom()
    );
    ravenemu::gba::Machine machine(
        rom, ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    for (unsigned word = 0; word < 4; ++word) {
        machine.bus.write32(
            ravenemu::gba::i32(0x02000000U + word * 4U),
            ravenemu::gba::i32(0x40404040U)
        );
    }
    machine.bus.write32(ravenemu::gba::i32(0x03000100U), ravenemu::gba::i32(0x33333333U));

    machine.bus.write32(ravenemu::gba::i32(0x040000bcU), ravenemu::gba::i32(0x02000000U));
    machine.bus.write32(ravenemu::gba::i32(0x040000c0U), ravenemu::gba::i32(0x040000a0U));
    machine.bus.write16(ravenemu::gba::i32(0x040000c4U), 4);
    machine.bus.write16(ravenemu::gba::i32(0x040000c6U), 0xb400); // canal 1, FIFO A

    machine.bus.write32(ravenemu::gba::i32(0x040000d4U), ravenemu::gba::i32(0x03000100U));
    machine.bus.write32(ravenemu::gba::i32(0x040000d8U), ravenemu::gba::i32(0x03000200U));
    machine.bus.write16(ravenemu::gba::i32(0x040000dcU), 1);
    machine.bus.write16(ravenemu::gba::i32(0x040000deU), 0x8400);
    machine.dma.trigger_sound_fifo(0);

    // Le canal 3 termine sa prise du bus puis cède au canal audio prioritaire.
    machine.run_frame(2);
    const auto preempted_state = machine.dma.export_state();
    machine.dma.import_state(preempted_state);
    machine.run_frame(30);

    check(machine.apu.fifo_size(0) == 16, "le DMA audio prioritaire n'a pas rempli la FIFO");
    check(machine.bus.read32(ravenemu::gba::i32(0x03000200U)) == 0,
          "le DMA suspendu a continué pendant le canal prioritaire");
    check(machine.dma.active(), "le DMA suspendu n'a pas repris le bus");

    machine.run_frame(2);
    check(machine.bus.read32(ravenemu::gba::i32(0x03000200U)) == ravenemu::gba::i32(0x33333333U),
          "le DMA suspendu n'a pas repris à sa phase exacte");
    check(!machine.dma.active(), "le DMA repris retient encore le bus");
}

} // namespace

int main() {
    frame_overshoot_does_not_accumulate();
    long_dma_stops_at_frame_boundary();
    dma_effects_follow_elapsed_cycles();
    vblank_dma_starts_without_copying_immediately();
    sound_fifo_dma_progresses_by_word();
    lower_priority_dma_waits_for_current_transfer();
    sound_dma_preempts_and_resumes_lower_priority_transfer();
    return 0;
}
