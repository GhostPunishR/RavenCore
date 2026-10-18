#include "machine/machine.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

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

void corrupt_timer_state_is_rejected() {
    ravenemu::gba::InterruptController interrupts;
    ravenemu::gba::Timers timers(interrupts);
    timers.reload_write(0, 0xfff0);
    timers.control_write(0, 0xc3); // actif, IRQ, pré-diviseur 1024
    timers.tick(5000);
    const auto valid = timers.export_state();
    timers.import_state(valid);
    check(timers.export_state() == valid, "état timers GBA valide non restauré à l'identique");

    // Index 0-3 compteurs, 4-7 recharges, 8-11 contrôles, 12-15 pré-diviseurs.
    // Une recharge à 0x10000 donnait une période nulle, donc une division par
    // zéro au débordement suivant.
    for (const auto& [index, value] : std::array<std::pair<std::size_t, std::int32_t>, 8>{{
             {4, 0x10000}, {4, -1}, {0, 0x10000}, {0, -1},
             {8, 0x10000}, {8, -1}, {12, 1024}, {12, -1}}}) {
        auto corrupt = valid;
        corrupt[index] = value;
        expect_failure<ravenemu::SaveStateError>(
            [&] { timers.import_state(corrupt); }, "état timers GBA hors limites accepté");
        check(timers.export_state() == valid, "état timers GBA refusé mais appliqué");
    }
}

// Cartouche de 8 Kio, `code` (Thumb) à 0x08000100.
ravenemu::gba::RomImage thumb_cartridge(std::initializer_list<std::uint16_t> code = {}) {
    std::vector<std::uint8_t> rom(0x2000);
    rom[0xb2] = 0x96;
    auto at = std::size_t{0x100};
    for (const auto half : code) {
        rom[at++] = static_cast<std::uint8_t>(half);
        rom[at++] = static_cast<std::uint8_t>(half >> 8U);
    }
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(rom));
}

void cartridge_prefetch_timing() {
    using ravenemu::gba::i32;
    ravenemu::gba::Machine machine(thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; });
    auto& bus = machine.bus;
    // WAITCNT = 0x4000 : WS0 à 4 attentes hors séquence, 2 à la suite, tampon
    // actif. Un demi-mot coûte 5 cycles hors séquence, 3 à la suite (GBATEK).
    bus.write16(i32(0x04000204U), 0x4000);
    const auto fetch = [&](std::uint32_t address) {
        bus.take_wait_cycles();
        bus.fetch16(i32(address));
        return bus.take_wait_cycles();
    };
    const auto read = [&](std::uint32_t address) {
        bus.take_wait_cycles();
        bus.read16(i32(address));
        return bus.take_wait_cycles();
    };
    const auto idle = [&](int cycles) { bus.idle(cycles); bus.take_wait_cycles(); };

    check(fetch(0x08000200U) == 5, "préchargement : premier accès non séquentiel mal compté");
    idle(6);  // deux demi-mots lus d'avance
    check(fetch(0x08000202U) == 1, "préchargement : instruction du tampon à plus d'un cycle");
    check(fetch(0x08000204U) == 1, "préchargement : seconde instruction du tampon à plus d'un cycle");
    check(fetch(0x08000206U) == 1, "préchargement : demi-mot en cours non payé au reste de sa lecture");
    check(fetch(0x08000208U) == 3, "préchargement : tampon vide, lecture séquentielle mal comptée");

    fetch(0x08000400U);
    idle(100);
    for (std::uint32_t index = 1; index <= 8; ++index)
        check(fetch(0x08000400U + 2U * index) == 1, "préchargement : le tampon ne garde pas huit demi-mots");

    fetch(0x08000600U);
    idle(6);
    check(fetch(0x08000700U) == 5, "préchargement : un saut n'a pas vidé le tampon");

    // Un accès de données à la cartouche arrête l'unité ; la lecture d'après repart de zéro.
    fetch(0x08000800U);
    idle(6);
    check(read(0x08001000U) == 5, "préchargement : lecture de données mal comptée");
    check(fetch(0x08000802U) == 5, "préchargement : l'accès aux données n'a pas arrêté l'unité");

    // Tombé sur le dernier cycle d'un demi-mot en cours, il attend ce cycle.
    fetch(0x08000a00U);
    idle(2);
    check(read(0x08001000U) == 6, "préchargement : dernier cycle du demi-mot en cours non attendu");

    // Les accès à la RAM interne laissent l'unité lire.
    fetch(0x08000c00U);
    for (int index = 0; index < 3; ++index) bus.read32(i32(0x03000000U));
    check(fetch(0x08000c02U) == 1, "préchargement : arrêté par un accès à la RAM interne");

    bus.write16(i32(0x04000204U), 0);
    check(fetch(0x08000e00U) == 5 && fetch(0x08000e02U) == 3, "préchargement coupé : attentes de la ROM fausses");
    idle(10);
    check(fetch(0x08000e04U) == 3, "préchargement coupé : l'unité a lu d'avance");
}

void prefetch_disable_bug() {
    // LSL r0, r1 (registre : un cycle interne), puis NOP. Sans préchargement,
    // l'instruction qui suit un cycle interne en ROM est relue hors séquence
    // (GBATEK, « Prefetch Disable Bug »).
    const auto cycles = [](int wait_control) {
        ravenemu::gba::Machine machine(thumb_cartridge({0x4088, 0x46c0, 0x46c0, 0x46c0, 0xe7fe}),
                                       ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; });
        machine.bus.write16(ravenemu::gba::i32(0x04000204U), wait_control);
        machine.cpu.reset(ravenemu::gba::i32(0x08000100U));
        machine.cpu.state.thumb = true;
        machine.bus.take_wait_cycles();
        std::array<int, 3> steps{};
        for (auto& step : steps) step = machine.cpu.step();
        return steps;
    };
    // LSL : remplissage du pipeline (5 + 3), lecture suivante (3), cycle interne (1).
    check(cycles(0) == std::array{12, 5, 3}, "préchargement coupé : lecture après cycle interne restée séquentielle");
    check(cycles(0x4000) == std::array{12, 2, 3}, "préchargement : cycle interne non mis à profit par l'unité");
}

void prefetch_state_is_validated() {
    ravenemu::gba::Machine machine(thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; });
    auto& bus = machine.bus;
    using Prefetch = ravenemu::gba::Bus::Prefetch;
    const auto rejected = [&](const Prefetch& value, const char* message) {
        expect_failure<ravenemu::SaveStateError>([&] { bus.restore_prefetch(value); }, message);
    };
    rejected({0x08000102U, 0, 0, 1, true}, "unité active acceptée sans préchargement");
    bus.write16(ravenemu::gba::i32(0x04000204U), 0x4000);
    bus.restore_prefetch({0x08000102U, 3, 1, 1, true});
    check(bus.prefetch().count == 3 && bus.prefetch().progress == 1 && bus.prefetch().halfword_cycles == 3,
          "état du préchargement mal restauré");
    rejected({0x08000102U, 9, 0, 1, true}, "plus de huit demi-mots acceptés");
    rejected({0x08000103U, 0, 0, 1, true}, "adresse impaire acceptée");
    rejected({0x08000102U, 0, 3, 1, true}, "lecture en cours plus longue qu'un demi-mot acceptée");
    rejected({0x08000102U, 2, 0, 1, false}, "unité arrêtée avec des demi-mots acceptée");
}

void timer_counts_two_cycles_after_enable() {
    ravenemu::gba::InterruptController interrupts;
    ravenemu::gba::Timers timers(interrupts);
    timers.control_write(0, 0x80);  // actif, pré-diviseur 1
    timers.tick(1);
    check(timers.counter(0) == 0, "timer GBA parti avant sa latence de démarrage");
    timers.tick(9);
    check(timers.counter(0) == 8, "timer GBA : latence de démarrage de 2 cycles non respectée");
}

void io_registers_are_not_mirrored() {
    using ravenemu::gba::i32;
    ravenemu::gba::Machine machine(thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; });
    auto& bus = machine.bus;
    bus.write16(i32(0x04000200U), 0x0001);
    // Trace de débogage mGBA : écrite hors des registres, elle se perd.
    bus.write16(i32(0x04fff600U), 0x6954);
    bus.write32(i32(0x04010200U), 0x7fff);
    check(bus.read16(i32(0x04000200U)) == 0x0001, "écriture hors des registres d'E/S reportée sur IE");
    check(static_cast<std::uint32_t>(bus.read32(i32(0x04000800U))) == 0x0d000020U &&
              static_cast<std::uint32_t>(bus.read32(i32(0x04020800U))) == 0x0d000020U,
          "registre de contrôle mémoire 0x04000800 mal lu");
}

// Un état restauré au milieu d'un transfert que le câble a déjà terminé se
// termine comme sans câble, sans clore à sa place le transfert suivant.
void restored_transfer_stays_out_of_the_next_one() {
    using ravenemu::gba::i32;
    std::vector<std::uint8_t> rom(0x2000);
    rom[0xb2] = 0x96;
    for (unsigned byte = 0; byte < 4; ++byte) rom[byte] = static_cast<std::uint8_t>(0xeafffffeU >> (8U * byte));  // b .
    const auto image = std::make_shared<const std::vector<std::uint8_t>>(rom);
    const auto clock = [] { return std::int64_t{0}; };
    ravenemu::gba::Machine parent(image, ravenemu::GbaSaveType::none, false, clock);
    ravenemu::gba::Machine child(image, ravenemu::GbaSaveType::none, false, clock);
    ravenemu::gba::LinkHub hub;
    hub.attach(0, parent.serial, parent.serial.clock());
    hub.attach(1, child.serial, child.serial.clock());
    const auto io16 = [](ravenemu::gba::Machine& machine, std::uint32_t offset) {
        return machine.bus.io[offset] | machine.bus.io[offset + 1] << 8;
    };
    for (auto* machine : {&parent, &child}) {
        machine->bus.write16(i32(0x04000134U), 0);       // RCNT
        machine->bus.write16(i32(0x04000128U), 0x2003);  // multijoueur, 115200 bauds
    }
    parent.bus.write16(i32(0x0400012aU), 0xaaaa);
    child.bus.write16(i32(0x0400012aU), 0xbbbb);
    parent.bus.write16(i32(0x04000128U), 0x2083);  // départ : 5755 cycles à deux
    parent.run_for(5000);
    child.run_for(5000);
    const auto remaining = child.serial.export_remaining();
    const auto saved = child.serial.export_link_state();
    parent.run_for(1000);
    child.run_for(1000);
    check(io16(child, 0x120) == 0xaaaa && io16(child, 0x122) == 0xbbbb, "câble GBA : premier transfert non reçu");

    // L'enfant revient au milieu du premier transfert ; le parent, en avance,
    // lance le second, dont l'enfant n'atteint le départ qu'après sa fin restaurée.
    child.serial.import_remaining(remaining);
    child.serial.import_link_state(saved);
    parent.run_for(2000);
    parent.bus.write16(i32(0x0400012aU), 0xcccc);
    parent.bus.write16(i32(0x04000128U), 0x2083);
    child.run_for(1000);
    check(io16(child, 0x120) == 0xbbbb && io16(child, 0x122) == 0xffff,
          "câble GBA : transfert restauré non terminé comme sans câble");
    // L'enfant change sa donnée avant le départ du second transfert, qui part
    // à 8000 cycles sur son horloge et dure 5755 : le second doit la lire.
    child.bus.write16(i32(0x0400012aU), 0xdddd);
    parent.run_for(6000);
    child.run_for(7000);
    check(io16(parent, 0x120) == 0xcccc && io16(parent, 0x122) == 0xdddd &&
              io16(child, 0x120) == 0xcccc && io16(child, 0x122) == 0xdddd,
          "câble GBA : le transfert restauré a clos le suivant");
    hub.detach_all();
}

// Une IRQ levée entre deux NOP d'un cycle (IWRAM) : le CPU en exécute encore
// quatre, le temps que la ligne se synchronise, puis entre dans l'exception.
// L'entrée coûte 2 cycles de plus que le `b 0x128` du vecteur 0x18 : pipeline
// rechargé (2 cycles), plus la lecture du mot suivant (1).
void irq_waits_four_cycles_then_enters() {
    using ravenemu::gba::i32;
    using ravenemu::gba::InterruptController;
    ravenemu::gba::Machine machine(
        thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    constexpr std::uint32_t code = 0x03000100U;
    for (std::uint32_t index = 0; index < 16; ++index)
        machine.bus.write32(i32(code + 4U * index), i32(0xe1a0'0000U));  // mov r0, r0
    machine.cpu.reset(i32(code));
    machine.cpu.state.irq_disabled = false;
    machine.interrupts.set_enable(1 << InterruptController::vblank);
    machine.interrupts.set_master_enable(true);
    machine.run_for(1);
    machine.run_for(1);
    machine.interrupts.request(InterruptController::vblank);
    for (int nop = 0; nop < 4; ++nop) {
        const auto pc = machine.cpu.state.regs[15];
        check(machine.run_for(1) == 1 && machine.cpu.state.regs[15] == pc + 4,
              "IRQ prise avant que sa ligne soit synchronisée");
    }
    check(machine.run_for(1) == 2 + 3 && machine.cpu.state.regs[15] == i32(0x128U),
          "IRQ : entrée, puis saut du vecteur 0x18 vers le gestionnaire en 0x128 attendus");
}

// Les 2 cycles d'entrée dans l'IRQ sont internes : comme les lectures du
// vecteur dans le BIOS, ils laissent l'unité de préchargement lire la ROM du
// jeu interrompu, qui retrouvera ces demi-mots au retour.
void irq_entry_lets_the_cartridge_prefetch_run() {
    using ravenemu::gba::i32;
    using ravenemu::gba::InterruptController;
    ravenemu::gba::Machine machine(
        thumb_cartridge({0x46c0, 0x46c0, 0x46c0, 0x46c0, 0x46c0, 0x46c0, 0x46c0, 0x46c0, 0x46c0, 0x46c0}),
        ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    machine.bus.write16(i32(0x0400'0204U), 0x4000);  // tampon actif, 3 cycles par demi-mot
    machine.cpu.reset(i32(0x0800'0100U));
    machine.cpu.state.thumb = true;
    machine.cpu.state.irq_disabled = false;
    machine.interrupts.set_enable(1 << InterruptController::vblank);
    machine.interrupts.set_master_enable(true);
    for (int nop = 0; nop < 2; ++nop) machine.run_for(1);
    machine.interrupts.request(InterruptController::vblank);
    const auto read_ahead = [&] {
        const auto& unit = machine.bus.prefetch();
        return unit.count * unit.halfword_cycles + unit.progress;
    };
    for (int step = 0; step < 8 && machine.cpu.state.regs[15] != i32(0x128U); ++step) {
        const auto before = read_ahead();
        const auto cycles = machine.run_for(1);
        if (machine.cpu.state.regs[15] != i32(0x128U)) continue;
        check(machine.bus.prefetch().active && cycles == 2 + 3 && read_ahead() == before + cycles,
              "IRQ : l'unité de préchargement n'a pas lu pendant l'entrée");
    }
    check(machine.cpu.state.regs[15] == i32(0x128U), "IRQ non prise pendant du code en ROM");
}

// Le CPU arrêté (Halt) que réveille une IRQ ne reprend le code du jeu
// qu'après le gestionnaire : il attend dans le BIOS que la ligne soit
// synchronisée. L'IRQ vient du timer 0, dont le débordement survient pendant
// le Halt ; le gestionnaire copie r1, que le jeu met à 1 après le Halt.
void halt_lets_the_handler_run_first() {
    using ravenemu::gba::i32;
    using ravenemu::gba::InterruptController;
    ravenemu::gba::Machine machine(
        thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 6> program{{
        {0x0300'0100U, 0xef02'0000U},  // swi Halt
        {0x0300'0104U, 0xe3a0'1001U},  // mov r1, #1
        {0x0300'0108U, 0xeaff'fffeU},  // b .
        {0x0300'0200U, 0xe1a0'2001U},  // gestionnaire : mov r2, r1
        {0x0300'0204U, 0xeaff'fffeU},  // b .
        {0x0300'7ffcU, 0x0300'0200U},  // adresse du gestionnaire
    }};
    for (const auto& [address, word] : program) machine.bus.write32(i32(address), i32(word));
    machine.bus.write16(i32(0x0400'0100U), 0xff00);  // TM0 : 256 cycles,
    machine.bus.write16(i32(0x0400'0102U), 0xc0);    // IRQ au débordement
    machine.bus.take_wait_cycles();
    machine.cpu.reset(i32(0x0300'0100U));
    machine.cpu.state.irq_disabled = false;
    machine.cpu.state.regs[2] = 0xff;
    machine.interrupts.set_enable(1 << InterruptController::timer0);
    machine.interrupts.set_master_enable(true);
    machine.run_for(16);
    check(machine.cpu.state.halted, "Halt n'a pas arrêté le CPU");
    machine.run_for(400);
    check(machine.cpu.state.regs[2] == 0 && machine.cpu.state.regs[15] == i32(0x0300'0204U),
          "après un Halt, le code du jeu a repris avant le gestionnaire d'IRQ");
}

// Programme en IWRAM (llvm-mc) : un gestionnaire d'IRQ qui acquitte IF et
// compte dans r4, et des appels du BIOS. En 0x03000100, VBlankIntrWait puis
// lecture de VCOUNT dans r6 et r1 = 1 ; en 0x0300012C, CpuFastSet de 16 Kio
// en EWRAM puis r3 = 1 ; en 0x03000180, la même copie en Thumb.
ravenemu::gba::Machine& with_irq_counting_program(ravenemu::gba::Machine& machine) {
    using ravenemu::gba::i32;
    constexpr std::array<std::uint32_t, 17> code{
        0xe3a00301, 0xef050000, 0xe1d060b6, 0xe3a01001, 0xeafffffe,              // attente du VBlank
        0xe3a00301, 0xe2800c02, 0xe1d020b2, 0xe1c020b2, 0xe2844001, 0xe12fff1e,  // gestionnaire
        0xe3a00402, 0xe2801801, 0xe3a02a01, 0xef0c0000, 0xe3a03001, 0xeafffffe,  // copie
    };
    for (std::size_t index = 0; index < code.size(); ++index)
        machine.bus.write32(i32(0x0300'0100U + 4U * static_cast<std::uint32_t>(index)), i32(code[index]));
    constexpr std::array<std::uint16_t, 10> thumb{
        0x2002, 0x0600, 0x2101, 0x0409, 0x1841, 0x2201, 0x0312, 0xdf0c, 0x2301, 0xe7fe};
    for (std::size_t index = 0; index < thumb.size(); ++index)
        machine.bus.write16(i32(0x0300'0180U + 2U * static_cast<std::uint32_t>(index)), thumb[index]);
    machine.bus.write32(i32(0x0300'7ffcU), i32(0x0300'0114U));
    machine.bus.take_wait_cycles();
    machine.cpu.state.irq_disabled = false;
    return machine;
}

// IntrWait (GBATEK) : toute IRQ réveille le BIOS, le gestionnaire du jeu
// s'exécute, puis l'attente reprend jusqu'à l'IRQ demandée. Un timer à IRQ
// toutes les 4096 cycles doit donc être servi pendant VBlankIntrWait (c'est
// ce qui manquait à Pokémon Rubis pour ses transferts série).
void intr_wait_lets_other_irqs_through() {
    using ravenemu::gba::i32;
    using ravenemu::gba::InterruptController;
    ravenemu::gba::Machine machine(
        thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    with_irq_counting_program(machine);
    machine.bus.write16(i32(0x0400'0004U), 0x0008);  // DISPSTAT : IRQ de VBlank
    machine.bus.write16(i32(0x0400'0100U), 0xf000);  // TM0 : 4096 cycles,
    machine.bus.write16(i32(0x0400'0102U), 0xc0);    // IRQ au débordement
    machine.bus.take_wait_cycles();
    machine.cpu.reset(i32(0x0300'0100U));
    machine.cpu.state.irq_disabled = false;
    machine.interrupts.set_enable(1 << InterruptController::vblank | 1 << InterruptController::timer0);
    machine.run_for(240'000);
    check(machine.cpu.state.regs[1] == 1 && machine.cpu.state.regs[6] == 160,
          "VBlankIntrWait : le jeu n'a pas repris au VBlank");
    check(machine.cpu.state.regs[4] >= 40, "VBlankIntrWait : IRQ du timer non servies pendant l'attente");

    // Le gestionnaire revient sur le SWI : l'attente reprend sans effacer un
    // VBlank noté entre-temps.
    machine.cpu.state.halted = false;
    machine.bios.handle_swi(0x05);
    check(machine.cpu.state.halted, "VBlankIntrWait n'a pas arrêté le CPU");
    machine.cpu.state.halted = false;
    machine.bios.interrupt_raised(1 << InterruptController::vblank);
    machine.bios.handle_swi(0x05);
    check(!machine.cpu.state.halted && !machine.bios.wait_state(),
          "VBlankIntrWait repris : VBlank noté pendant le gestionnaire perdu");
}

// Le BIOS d'origine exécute CpuSet, CpuFastSet ou une décompression avec les
// IRQ de l'appelant : elles l'interrompent. RavenCore fait la copie d'un coup,
// mais en garde le temps, que les IRQ peuvent interrompre.
void long_bios_call_lets_irqs_through() {
    using ravenemu::gba::i32;
    using ravenemu::gba::InterruptController;
    const auto copy = [](bool irqs, bool timer, bool thumb = false) {
        auto machine = std::make_unique<ravenemu::gba::Machine>(
            thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
        );
        with_irq_counting_program(*machine);
        machine->bus.write32(i32(0x0200'0ffcU), i32(0x1234'5678U));
        machine->bus.write16(i32(0x0400'0100U), 0xfc00);  // TM0 : 1024 cycles
        machine->bus.write16(i32(0x0400'0102U), 0xc0);
        machine->bus.take_wait_cycles();
        machine->cpu.reset(i32(thumb ? 0x0300'0180U : 0x0300'012cU));
        machine->cpu.state.thumb = thumb;
        machine->cpu.state.irq_disabled = !irqs;
        machine->interrupts.set_enable(timer ? 1 << InterruptController::timer0 : 0);
        machine->interrupts.set_master_enable(true);
        auto cycles = 0;
        while (machine->cpu.state.regs[3] != 1 && cycles < 400'000) cycles += machine->run_for(1);
        check(machine->bus.read32(i32(0x0201'0ffcU)) == i32(0x1234'5678U), "CpuFastSet : copie absente");
        return std::pair{cycles, machine->cpu.state.regs[4]};
    };
    const auto [alone, none] = copy(true, false);
    check(none == 0 && alone > 40'000, "CpuFastSet : temps de la copie perdu");
    const auto [interrupted, served] = copy(true, true);
    check(served >= 30 && interrupted > alone, "CpuFastSet : IRQ du timer non servies pendant la copie");
    const auto [masked, blocked] = copy(false, true);
    check(blocked == 0 && masked == alone, "CpuFastSet : IRQ servies alors que le jeu les masque");
    // En Thumb, le gestionnaire revient au point de reprise Thumb du BIOS.
    const auto [thumb_alone, thumb_none] = copy(true, false, true);
    const auto [thumb_interrupted, thumb_served] = copy(true, true, true);
    check(thumb_none == 0 && thumb_alone > 40'000 && thumb_served >= 30 && thumb_interrupted > thumb_alone,
          "CpuFastSet en Thumb : IRQ non servies ou temps perdu");

    ravenemu::gba::Machine machine(
        thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    for (const auto& invalid : {std::array<std::int32_t, 2>{-1, 0}, std::array<std::int32_t, 2>{1 << 29, 0x0300'0100},
                                std::array<std::int32_t, 2>{0, 0x0300'0100}})
        expect_failure<ravenemu::SaveStateError>([&] { machine.restore_bios_busy(invalid); },
                                                 "temps du BIOS invalide accepté");
}

// Une recharge écrite ne sert qu'au cycle suivant : le débordement du cycle
// même de l'écriture recharge encore l'ancienne valeur.
void timer_reload_applies_one_cycle_after_write() {
    ravenemu::gba::InterruptController interrupts;
    ravenemu::gba::Timers timers(interrupts);
    timers.reload_write(0, 0xffff);
    timers.control_write(0, 0x80);  // même accès 32 bits : le timer part de la recharge écrite
    check(timers.counter(0) == 0xffff, "timer GBA : recharge écrite dans le même accès ignorée");
    timers.tick(2);  // latence de démarrage
    timers.reload_write(0, 0x1234);
    timers.tick(1);
    check(timers.counter(0) == 0xffff, "timer GBA : recharge servie dans le cycle même de son écriture");
    timers.tick(1);
    check(timers.counter(0) == 0x1234, "timer GBA : recharge non servie au cycle suivant");
}

// Un état pris juste après l'écriture d'une recharge (un DMA peut finir là)
// la garde en attente : reprise, elle ne sert qu'au cycle suivant.
void pending_timer_reload_survives_a_state() {
    ravenemu::gba::InterruptController interrupts;
    ravenemu::gba::Timers timers(interrupts);
    timers.reload_write(0, 0xffff);
    timers.control_write(0, 0x80);
    timers.tick(2);
    timers.reload_write(0, 0x1234);
    const auto pending = timers.export_pending_reloads();
    check(pending == std::array<std::int32_t, 4>{0x1234, -1, -1, -1}, "timer GBA : recharge en attente non exportée");
    ravenemu::gba::Timers restored(interrupts);
    restored.reload_write(0, 0xffff);
    restored.control_write(0, 0x80);
    restored.tick(3);  // la recharge 0xFFFF est en service
    restored.import_pending_reloads(pending);
    restored.tick(1);
    check(restored.counter(0) == 0xffff, "timer GBA : recharge importée servie trop tôt");
    restored.tick(1);
    check(restored.counter(0) == 0x1234, "timer GBA : recharge importée perdue");
    const std::array<std::int32_t, 4> invalid{0x10000, -1, -1, -1};
    expect_failure<ravenemu::SaveStateError>([&] { restored.import_pending_reloads(invalid); },
                                             "timer GBA : recharge hors bornes acceptée");
}

// RavenCore note une IRQ dans le mot du BIOS (0x03007FF8) à la place du
// gestionnaire du jeu : ce n'est pas un accès du CPU, il ne coûte aucun cycle.
void bios_interrupt_flags_cost_no_cycles() {
    ravenemu::gba::Machine machine(
        thumb_cartridge(), ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; }
    );
    machine.bus.take_wait_cycles();
    machine.interrupts.request(ravenemu::gba::InterruptController::timer0);
    check(machine.bus.take_wait_cycles() == 0, "IRQ notée par le BIOS : cycles comptés au CPU");
    check((machine.bus.iwram[0x7ff8] & 0x08U) != 0, "IRQ de timer non notée en 0x03007FF8");
}

} // namespace

int main() {
    corrupt_timer_state_is_rejected();
    cartridge_prefetch_timing();
    prefetch_disable_bug();
    prefetch_state_is_validated();
    irq_waits_four_cycles_then_enters();
    irq_entry_lets_the_cartridge_prefetch_run();
    halt_lets_the_handler_run_first();
    intr_wait_lets_other_irqs_through();
    long_bios_call_lets_irqs_through();
    timer_reload_applies_one_cycle_after_write();
    pending_timer_reload_survives_a_state();
    bios_interrupt_flags_cost_no_cycles();
    timer_counts_two_cycles_after_enable();
    io_registers_are_not_mirrored();
    restored_transfer_stays_out_of_the_next_one();
    frame_overshoot_does_not_accumulate();
    long_dma_stops_at_frame_boundary();
    dma_effects_follow_elapsed_cycles();
    vblank_dma_starts_without_copying_immediately();
    sound_fifo_dma_progresses_by_word();
    lower_priority_dma_waits_for_current_transfer();
    sound_dma_preempts_and_resumes_lower_priority_transfer();
    return 0;
}
