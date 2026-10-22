#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/program_fixture.hpp"

#include <array>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t post = 0x04000300, halt = post + 1, base = 0x02000000;
constexpr std::uint32_t ie = 0x04000210, flags = ie + 4, ime = 0x04000208;

void registers_and_boot() {
    Memory memory;
    check(memory.read32(arm9,post) == 0 && memory.read32(arm7,post) == 0, "registres de boot initiaux");
    memory.write32(arm9,post,0xffffffff);
    check(memory.read32(arm9,post) == 3 && memory.read32(arm7,post) == 0, "POSTFLG partagés ou bits réservés non masqués");
    memory.write8(arm9,post,0); check(memory.read8(arm9,post) == 1, "POSTFLG.0 effacé sans reset");
    memory.write16(arm9,post,2); check(memory.read16(arm9,post) == 3, "POSTFLG ARM9 bit 1 non modifiable");
    memory.write32(arm7,post,0xff); check(memory.read8(arm7,post) == 0, "accès hôte ARM7 passe pour du BIOS");
    for (const auto mode : {0x40U,0xc0U}) {
        expect_failure<ravenemu::Error>([&] { memory.write8(arm7,halt,static_cast<std::uint8_t>(mode)); }, "GBA/Sleep accepté");
        expect_failure<ravenemu::Error>([&] { memory.write16(arm7,post,static_cast<std::uint16_t>((mode << 8U) | 1U)); }, "demi-mot Sleep accepté");
        check(memory.read32(arm7,post) == 0, "mode refusé modifie POSTFLG/HALTCNT");
    }
    memory.write8(arm7,halt,0x3f); check(memory.read8(arm7,halt) == 0, "HALTCNT bits réservés");
    memory.clear_ram(); check(memory.read32(arm9,post) == 0, "reset n'efface pas POSTFLG persistant");
    Machine machine; const auto rom = cartridge_image(); machine.load_cartridge(Cartridge{rom});
    check(machine.memory().read32(arm9,post) == 1 && machine.memory().read32(arm7,post) == 1,
          "démarrage direct ne marque pas les deux CPU comme démarrés");
    machine.memory().write8(arm9,post,3); auto copy = machine;
    copy.memory().clear_ram();
    check(machine.memory().read8(arm9,post) == 3 && copy.memory().read8(arm9,post) == 0, "copies POSTFLG partagées");
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(Cartridge{invalid}); }, "chargement invalide accepté");
    check(machine.memory().read8(arm9,post) == 3, "chargement refusé réinitialise POSTFLG");
    machine.load_cartridge(Cartridge{rom});
    check(machine.memory().read8(arm9,post) == 1 && machine.memory().read8(arm7,post) == 1, "rechargement conserve le bit ARM9 privé");
}

void bios_write_origin() {
    // Chaque famille de store doit transporter le PC de l'instruction, y
    // compris STM. Une instruction RAM ne doit pas hériter d'un ancien fetch BIOS.
    for (const bool thumb : {false,true}) for (unsigned kind = 0; kind < 4; ++kind) {
        const std::array<std::uint32_t,4> arm{0xe5c10000,0xe1c100b0,0xe5810000,0xe8a10001};
        const std::array<std::uint16_t,4> thm{0x7008,0x8008,0x6008,0xc101};
        Memory memory; std::vector<std::uint8_t> bios(0x4000);
        const auto pc = thumb ? 0x3ffeU : 0x3ffcU;
        if (thumb) put16(bios,pc,thm[kind]); else put32(bios,pc,arm[kind]);
        memory.load_bios(arm7,bios);
        if (thumb) memory.write16(arm7,base,thm[kind]); else memory.write32(arm7,base,arm[kind]);
        Cpu cpu{arm7}; cpu.reset(base,thumb); cpu.set_register(0,3); cpu.set_register(1,post); cpu.step(memory);
        check(memory.read8(arm7,post) == 0, "store RAM ARM7 modifie POSTFLG");
        cpu.reset(pc,thumb); cpu.set_register(0,3); cpu.set_register(1,post); cpu.step(memory);
        check(memory.read8(arm7,post) == 1, "store BIOS ARM7 ne peut pas fixer POSTFLG");
        cpu.reset(pc,thumb); cpu.set_register(0,0); cpu.set_register(1,post); cpu.step(memory);
        check(memory.read8(arm7,post) == 1, "store BIOS efface POSTFLG.0");
        memory.clear_ram();
        if (thumb) memory.write16(arm7,base,thm[kind]); else memory.write32(arm7,base,arm[kind]);
        cpu.reset(base,thumb); cpu.set_register(0,1); cpu.set_register(1,post); cpu.step(memory);
        check(memory.read8(arm7,post) == 0, "autorisation BIOS fuit après reset et retour en RAM");
    }
    // L'adresse de fetch, pas celle de la donnée source, autorise POSTFLG.
    Memory memory; std::vector<std::uint8_t> bios(0x4000);
    put32(bios,0x100,0xe881000d); // STMIA r1,{r0,r2,r3} : troisième mot vers 04000308h non mappé
    memory.load_bios(arm7,bios);
    Cpu cpu{arm7}; cpu.reset(0x100,false); cpu.set_register(0,0x8001); cpu.set_register(1,post);
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "STM BIOS invalide accepté");
    check(cpu.state() == before && memory.read32(arm7,post) == 0, "STM BIOS refusé modifie les flags ou demande HALT");
    check(memory.read32(arm7,post + 4) == 1, "STM BIOS refusé modifie POWCNT2");
    memory.write32(arm7,base,0xe1a00000); cpu.reset(base,false); cpu.step(memory);
    check(!cpu.state().waiting_for_interrupt && cpu.state().instructions == 1, "STM refusé laisse une demande HALT");
}

void enter_halt(Cpu& cpu, Memory& memory) {
    memory.write32(arm7,base,0xe5c10000); // STRB r0,[r1]
    memory.write32(arm7,base + 4,0xe3a0302a);
    cpu.reset(base,false); cpu.set_register(0,0x80); cpu.set_register(1,halt); cpu.step(memory);
    check(cpu.state().waiting_for_interrupt && cpu.state().instructions == 1 && cpu.state().registers[15] == base + 4 &&
          memory.read8(arm7,halt) == 0x80, "HALTCNT ne termine pas STRB puis suspend le CPU");
}

void halt_masks_and_irq() {
    Memory memory; Cpu cpu{arm7}; enter_halt(cpu,memory);
    const auto asleep = cpu.state();
    for (unsigned i = 0; i < 10; ++i) cpu.step(memory);
    check(cpu.state() == asleep, "HALT exécute des instructions sans événement");
    memory.request_interrupt(arm7,8); cpu.step(memory); check(cpu.state() == asleep, "IF seul réveille HALT");
    memory.write32(arm7,ime,1); cpu.step(memory); check(cpu.state() == asleep, "IME seul réveille HALT");
    memory.write32(arm7,ime,0); memory.write32(arm7,ie,8); cpu.step(memory);
    check(!cpu.state().waiting_for_interrupt && cpu.state().mode == CpuMode::system && cpu.state().instructions == 1 &&
          cpu.state().registers[15] == base + 4 && !memory.irq_pending(arm7), "HALT doit ignorer IME pour le réveil");
    cpu.step(memory); check(cpu.state().registers[3] == 42 && cpu.state().instructions == 2, "reprise après HALT sans IME");
    // IME=1 ne permet pas de prendre une IRQ lorsque CPSR.I reste masqué.
    enter_halt(cpu,memory); memory.write32(arm7,ime,1); cpu.step(memory);
    check(!cpu.state().waiting_for_interrupt && cpu.state().mode == CpuMode::system && cpu.state().irq_disabled,
          "réveil HALT ignore le masque CPSR lors de l'entrée IRQ");
    // IRQ démasquée : entrée normale sans terminer l'instruction suivante.
    memory.clear_ram(); memory.write32(arm7,base,0xe321f05f); memory.write32(arm7,base + 4,0xe5c10000);
    cpu.reset(base,false); cpu.set_register(0,0x80); cpu.set_register(1,halt); cpu.step(memory); cpu.step(memory);
    const auto saved_status = cpu.state().cpsr();
    memory.write32(arm7,ie,8); memory.write32(arm7,ime,1); memory.request_interrupt(arm7,8); cpu.step(memory);
    check(cpu.state().mode == CpuMode::irq && !cpu.state().waiting_for_interrupt && cpu.state().registers[15] == 0x18 &&
          cpu.state().registers[14] == base + 12 && cpu.state().spsr() == saved_status && cpu.state().instructions == 2,
          "IRQ après HALT : LR/SPSR ou compteur incorrect");
    cpu.reset(base,false); check(!cpu.state().waiting_for_interrupt && memory.read8(arm7,halt) == 0x80,
          "reset CPU conserve HALT ou efface implicitement le périphérique");
}

void halt_bus_and_dma() {
    Memory memory; Cpu cpu{arm7}; enter_halt(cpu,memory);
    memory.write32(arm7,0x02001000,0xdeadbeef);
    memory.write32(arm7,0x040000b0,0x02001000); memory.write32(arm7,0x040000b4,0x02002000);
    memory.write32(arm7,0x040000b8,0xc4000001); memory.write32(arm7,ie,0x100);
    cpu.step(memory);
    check(memory.read32(arm7,0x02002000) == 0xdeadbeef && cpu.state().waiting_for_interrupt && cpu.state().instructions == 1,
          "HALT bloque le DMA ou le CPU reprend sur le créneau DMA");
    cpu.step(memory); check(!cpu.state().waiting_for_interrupt && !memory.irq_pending(arm7), "IRQ DMA ne réveille pas HALT avec IME=0");
    cpu.step(memory); check(cpu.state().registers[3] == 42, "reprise après DMA pendant HALT");
    // Une demande hôte est consommée une seule fois et les bits sans fonction ne l'annulent pas.
    memory.clear_ram(); memory.write32(arm7,base,0xe3a0302a); cpu.reset(base,false);
    memory.write8(arm7,halt,0x80); memory.write8(arm7,halt,0);
    auto copy = memory; cpu.step(memory); check(cpu.state().waiting_for_interrupt && cpu.state().instructions == 0, "demande HALT perdue");
    memory.write32(arm7,ie,8); memory.request_interrupt(arm7,8); cpu.step(memory); cpu.step(memory);
    check(cpu.state().registers[3] == 42, "demande HALT rejouée après réveil");
    Cpu other{arm7}; other.reset(base,false); other.step(copy);
    check(other.state().waiting_for_interrupt && !cpu.state().waiting_for_interrupt, "copies partagent la demande HALT");
    // Accès combiné BIOS : POSTFLG et HALT, validés avant tout effet.
    memory.clear_ram(); std::vector<std::uint8_t> bios(0x4000); put32(bios,0x100,0xe1c100b0);
    memory.load_bios(arm7,bios); cpu.reset(0x100,false); cpu.set_register(0,0xc001); cpu.set_register(1,post);
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "BIOS active Sleep non implémenté");
    check(cpu.state() == before && memory.read32(arm7,post) == 0, "demi-mot Sleep refusé fixe POSTFLG");
    cpu.set_register(0,0x8001); cpu.step(memory);
    check(memory.read16(arm7,post) == 0x8001 && cpu.state().waiting_for_interrupt, "demi-mot BIOS POSTFLG/HALT");
}

void scheduler_program() {
    Program program; program.load(0,0x80); program.load(1,halt); program.code.push_back(0xe5c10000);
    program.load(2,0x42); program.store(2,0x02001000);
    const auto rom = cartridge_image({0xeafffffe},program.finish());
    Machine machine; machine.load_cartridge(Cartridge{rom}); machine.run_cycles(3);
    const auto asleep = machine.cpu_state(arm7);
    check(asleep.waiting_for_interrupt && asleep.instructions == 3 && machine.cpu_state(arm9).instructions == 6,
          "programme ARM7 n'entre pas en HALT");
    auto copy = machine;
    machine.run_cycles(20);
    check(machine.cpu_state(arm7) == asleep && machine.cpu_state(arm9).instructions == 46 && machine.scheduler_state().cycles == 23,
          "HALT ARM7 bloque l'ARM9 ou l'horloge commune");
    machine.memory().write32(arm7,ie,8); machine.memory().write16(arm7,0x04000100,0xfffc);
    machine.memory().write16(arm7,0x04000102,0xc0); // timer avec IRQ, IME reste nul
    machine.run_cycles(20);
    check(machine.memory().read32(arm9,0x02001000) == 0x42 && !machine.cpu_state(arm7).waiting_for_interrupt &&
          !machine.memory().irq_pending(arm7), "timer ne réveille pas ARM7 sous l'ordonnanceur avec IME=0");
    check(copy.cpu_state(arm7) == asleep && copy.memory().read32(arm9,0x02001000) == 0, "copies de machine partagent HALT/timers");
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { copy.load_cartridge(Cartridge{invalid}); }, "ROM invalide acceptée pendant HALT");
    check(copy.cpu_state(arm7) == asleep, "chargement refusé réveille ARM7");
    copy.load_cartridge(Cartridge{rom});
    check(!copy.cpu_state(arm7).waiting_for_interrupt && copy.memory().read16(arm7,post) == 1 &&
          copy.scheduler_state().cycles == 0, "rechargement conserve HALT ou oublie POSTFLG");
}

void bios_halt_program() {
    // BIOS synthétique : SWI -> HALTCNT -> retour MOVS pc,lr. L'attente ne
    // détruit ni le mode Supervisor ni le SPSR du SWI.
    std::vector<std::uint8_t> bios(0x4000);
    put32(bios,8,0xea00003c); put32(bios,0x100,0xe59f100c); put32(bios,0x104,0xe3a00080);
    put32(bios,0x108,0xe5c10000); put32(bios,0x10c,0xe1b0f00e); put32(bios,0x114,halt);
    Machine machine; machine.memory().load_bios(arm7,bios);
    machine.load_cartridge(Cartridge{cartridge_image({0xeafffffe},{0xef000000,0xe3a0302a,0xeafffffe})});
    machine.run_steps(arm7,5);
    check(machine.cpu_state(arm7).waiting_for_interrupt && machine.cpu_state(arm7).mode == CpuMode::supervisor &&
          machine.cpu_state(arm7).registers[15] == 0x10c && machine.cpu_state(arm7).spsr() == 0xdf,
          "Halt BIOS ne conserve pas le contexte du SWI");
    machine.memory().write32(arm7,ie,8); machine.memory().request_interrupt(arm7,8); machine.run_steps(arm7,3);
    check(!machine.cpu_state(arm7).waiting_for_interrupt && machine.cpu_state(arm7).mode == CpuMode::system &&
          machine.cpu_state(arm7).registers[3] == 42, "Halt BIOS ne reprend pas puis ne revient pas au programme");
}
}

int main() {
    registers_and_boot(); bios_write_origin(); halt_masks_and_irq(); halt_bus_and_dma(); scheduler_program(); bios_halt_program();
}
