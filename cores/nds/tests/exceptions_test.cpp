#include <ravenemu/nds/cpu.hpp>
#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <vector>

using ravenemu::nds::Cpu;
using ravenemu::nds::CpuMode;
using ravenemu::nds::CpuState;
using ravenemu::nds::CpuExecutionError;
using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t base = 0x02000000;

namespace {

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

std::uint32_t vectors(Processor cpu) { return cpu == arm9 ? 0xffff0000U : 0U; }

std::vector<std::uint8_t> synthetic_bios(Processor cpu) {
    std::vector<std::uint8_t> bytes(cpu == arm9 ? 0x1000 : 0x4000);
    put32(bytes, 4, 0xe1b0f00e);    // UND : MOVS pc,lr
    put32(bytes, 8, 0xea00000c);    // SWI : B +0x40
    put32(bytes, 0x18, 0xea00000c); // IRQ : B +0x50
    put32(bytes, 0x1c, 0xea00000f); // FIQ : B +0x60
    put32(bytes, 0x40, 0xe2800001); // ADD r0,r0,#1
    put32(bytes, 0x44, 0xe1500000); // CMP r0,r0 : modifier Z/C avant le retour
    put32(bytes, 0x48, 0xe1b0f00e); // MOVS pc,lr
    put32(bytes, 0x50, 0xe2800002); // ADD r0,r0,#2
    put32(bytes, 0x54, 0xe25ef004); // SUBS pc,lr,#4
    put32(bytes, 0x60, 0xe2888001); // ADD r8,r8,#1 (banque FIQ)
    put32(bytes, 0x64, 0xe25ef004); // SUBS pc,lr,#4
    return bytes;
}

void steps(Cpu& cpu, Memory& memory, unsigned count) {
    for (unsigned i = 0; i < count; ++i) cpu.step(memory);
}

void banks_test() {
    CpuState state;
    state.registers[8] = 0x88;
    state.registers[12] = 0xcc;
    state.registers[13] = 0x1000;
    state.registers[14] = 0x1004;
    for (const auto mode : {CpuMode::supervisor, CpuMode::irq, CpuMode::abort, CpuMode::undefined, CpuMode::fiq}) {
        state.set_cpsr(static_cast<std::uint32_t>(mode));
        check(state.registers[13] == 0 && state.registers[14] == 0, "nouvelle banque SP/LR non nulle");
        const auto value = static_cast<std::uint32_t>(mode);
        state.registers[13] = 0x2000 + value;
        state.registers[14] = 0x3000 + value;
        state.set_spsr(0xa00000dfU + value);
        if (mode == CpuMode::fiq) {
            check(state.registers[8] == 0 && state.registers[12] == 0, "FIQ ne possède pas ses r8-r12");
            state.registers[8] = 0xf8;
            state.registers[12] = 0xfc;
        } else check(state.registers[8] == 0x88 && state.registers[12] == 0xcc, "r8-r12 communs hors FIQ");
    }
    for (const auto mode : {CpuMode::user, CpuMode::system}) {
        state.set_cpsr(static_cast<std::uint32_t>(mode));
        check(state.registers[13] == 0x1000 && state.registers[14] == 0x1004 &&
              state.registers[8] == 0x88 && state.registers[12] == 0xcc, "banque User/System partagée");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(state.spsr()); }, "SPSR présent en User/System");
    }
    for (const auto mode : {CpuMode::supervisor, CpuMode::irq, CpuMode::abort, CpuMode::undefined, CpuMode::fiq}) {
        state.set_cpsr(static_cast<std::uint32_t>(mode));
        const auto value = static_cast<std::uint32_t>(mode);
        check(state.registers[13] == 0x2000 + value && state.registers[14] == 0x3000 + value &&
              state.spsr() == 0xa00000dfU + value, "perte d'une banque SP/LR/SPSR");
    }
    state.set_cpsr(0xf80000ff);
    check(state.cpsr() == 0xf80000ff, "aller-retour CPSR NZCVQ TIF mode");
    const auto before = state;
    expect_failure<ravenemu::Error>([&] { state.set_cpsr(0); }, "mode CPSR réservé accepté");
    check(state == before, "CPSR invalide a modifié les banques");
}

void psr_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    cpu.reset(base, false);
    cpu.set_register(13, 0x02003000);
    cpu.set_register(14, 0x11110000);
    cpu.set_register(0, 0xd3);
    cpu.set_register(1, 0xa80000d3);
    cpu.set_register(4, 0x600000ff);
    cpu.set_register(6, 0xdf);
    const std::array<std::uint32_t, 8> program{
        0xe121f000, // MSR cpsr_c,r0 -> Supervisor
        0xe129f001, // MSR cpsr_fc,r1
        0xe10f2000, // MRS r2,cpsr
        0xe14f3000, // MRS r3,spsr
        0xe169f004, // MSR spsr_fc,r4
        0xe14f5000, // MRS r5,spsr
        0xe121f006, // MSR cpsr_c,r6 -> System
        0xe328f102, // MSR cpsr_f,#0x80000000 (immédiat tourné)
    };
    for (std::size_t i = 0; i < program.size(); ++i)
        memory.write32(processor, base + static_cast<std::uint32_t>(i * 4), program[i]);
    cpu.step(memory);
    check(cpu.state().mode == CpuMode::supervisor && cpu.state().registers[13] == 0, "MSR mode / banque SP");
    cpu.set_register(13, 0x02004000);
    cpu.set_register(14, 0x22220000);
    steps(cpu, memory, 5);
    check(cpu.state().registers[2] == (processor == arm9 ? 0xa80000d3U : 0xa00000d3U) &&
          cpu.state().registers[3] == 0 && cpu.state().registers[5] == 0x600000ff, "MRS/MSR CPSR/SPSR et Q ARM9");
    cpu.step(memory);
    check(cpu.state().mode == CpuMode::system && cpu.state().registers[13] == 0x02003000 &&
          cpu.state().registers[14] == 0x11110000, "MSR retour à la banque System");
    cpu.step(memory);
    check(cpu.state().cpsr() == 0x800000df, "MSR immédiat : masque des drapeaux");

    cpu.reset(base, false);
    cpu.set_register(0, 0x10);
    cpu.set_register(1, 0xf80000d3);
    memory.write32(processor, base, 0xe121f000); // passer en User
    memory.write32(processor, base + 4, 0xe129f001); // tenter d'écrire contrôle et drapeaux
    steps(cpu, memory, 2);
    check(cpu.state().mode == CpuMode::user && !cpu.state().irq_disabled && !cpu.state().fiq_disabled &&
          cpu.state().cpsr() == (processor == arm9 ? 0xf8000010U : 0xf0000010U),
          "User ne doit pouvoir écrire que les drapeaux");
    memory.write32(processor, base + 8, 0xe14f2000); // SPSR absent en User
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "lecture SPSR en User acceptée");
    check(cpu.state() == before, "MRS refusé a modifié le CPU");

    for (const auto value : {0U, 0xffU}) { // mode invalide / changement de T par MSR
        cpu.reset(base, false);
        cpu.set_register(0, value);
        memory.write32(processor, base, 0xe121f000);
        const auto initial = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "contrôle CPSR invalide accepté");
        check(cpu.state() == initial, "MSR refusé a modifié l'état");
    }
}

void software_exception_test(Processor processor, bool thumb, bool undefined) {
    Memory memory;
    memory.load_bios(processor, synthetic_bios(processor));
    Cpu cpu{processor};
    cpu.reset(base, thumb);
    cpu.set_register(0, 7);
    cpu.set_register(13, 0x02003000);
    cpu.set_register(14, 0x11110000);
    if (thumb) {
        memory.write16(processor, base, undefined ? 0xde00 : 0xdf42);
        memory.write16(processor, base + 2, 0x2109); // MOVS r1,#9 après retour
    } else {
        memory.write32(processor, base, undefined ? 0xe7f000f0 : 0xef123456);
        memory.write32(processor, base + 4, 0xe3a01009); // MOV r1,#9
    }
    const auto saved = cpu.state().cpsr();
    const auto next = base + (thumb ? 2U : 4U);
    cpu.step(memory);
    check(cpu.state().mode == (undefined ? CpuMode::undefined : CpuMode::supervisor) && !cpu.state().thumb &&
          cpu.state().irq_disabled && cpu.state().spsr() == saved && cpu.state().registers[14] == next &&
          cpu.state().registers[15] == vectors(processor) + (undefined ? 4U : 8U), "entrée SWI/UND ARM/Thumb");
    steps(cpu, memory, undefined ? 1U : 4U);
    check(cpu.state().mode == CpuMode::system && cpu.state().cpsr() == saved &&
          cpu.state().registers[13] == 0x02003000 && cpu.state().registers[14] == 0x11110000 &&
          cpu.state().registers[15] == next && cpu.state().registers[0] == (undefined ? 7U : 8U),
          "MOVS pc,lr doit restaurer CPSR et banques");
    cpu.step(memory);
    check(cpu.state().registers[1] == 9, "le programme ne reprend pas après l'exception");
}

void interrupts_test(Processor processor) {
    Memory memory;
    memory.load_bios(processor, synthetic_bios(processor));
    Cpu cpu{processor};
    cpu.reset(base, false);
    cpu.set_register(8, 0x55);
    memory.write32(processor, base, 0xe321f01f); // MSR cpsr_c,#0x1f : démasquer I/F
    memory.write32(processor, base + 4, 0xe3a00007);
    cpu.set_interrupt_lines(true, true);
    cpu.step(memory); // I/F initialement masqués : exécuter MSR
    check(cpu.state().mode == CpuMode::system && cpu.state().instructions == 1, "I/F masqués ignorés");
    cpu.step(memory); // FIQ prioritaire, sans instruction terminée
    check(cpu.state().mode == CpuMode::fiq && cpu.state().registers[15] == vectors(processor) + 0x1c &&
          cpu.state().registers[14] == base + 8 && cpu.state().instructions == 1 && cpu.state().registers[8] == 0,
          "priorité FIQ, LR ou banque r8");
    cpu.set_interrupt_lines(true, false);
    steps(cpu, memory, 3); // branche, calcul en banque FIQ, SUBS pc,lr,#4
    check(cpu.state().mode == CpuMode::system && cpu.state().registers[15] == base + 4 &&
          cpu.state().registers[8] == 0x55 && !cpu.state().irq_disabled && !cpu.state().fiq_disabled,
          "retour FIQ : état System et r8 préservés");
    cpu.step(memory); // IRQ restée en attente
    check(cpu.state().mode == CpuMode::irq && cpu.state().instructions == 4 && cpu.state().registers[14] == base + 8,
          "IRQ après FIQ ou compteur d'instructions");
    cpu.set_interrupt_lines(false);
    steps(cpu, memory, 4); // gestionnaire puis MOV du programme
    check(cpu.state().mode == CpuMode::system && cpu.state().registers[0] == 7 && cpu.state().instructions == 8,
          "reprise après IRQ");

    cpu.reset(base, false);
    cpu.set_register(1, base + 0x101);
    memory.write32(processor, base + 4, 0xe12fff11); // BX r1 -> Thumb
    memory.write16(processor, base + 0x100, 0x2009);
    steps(cpu, memory, 2);
    cpu.set_interrupt_lines(true);
    cpu.step(memory);
    check(cpu.state().registers[14] == base + 0x104 && (cpu.state().spsr() & 0x20U) != 0,
          "IRQ Thumb : LR ou T sauvegardé");
    cpu.set_interrupt_lines(false);
    steps(cpu, memory, 4);
    check(cpu.state().thumb && cpu.state().mode == CpuMode::system && cpu.state().registers[0] == 9 &&
          cpu.state().registers[15] == base + 0x102, "SUBS pc,lr,#4 doit reprendre le code Thumb");

    cpu.reset(base, false);
    cpu.step(memory); // MSR, IRQ démasquée
    cpu.set_interrupt_lines(true);
    cpu.step(memory);
    steps(cpu, memory, 3);
    check(cpu.state().mode == CpuMode::system, "retour IRQ sans acquittement");
    const auto count = cpu.state().instructions;
    cpu.step(memory);
    check(cpu.state().mode == CpuMode::irq && cpu.state().instructions == count,
          "une ligne IRQ active doit provoquer une nouvelle entrée après retour");
}

void nested_exception_test(Processor processor) {
    Memory memory;
    auto bios = synthetic_bios(processor);
    put32(bios, 0x40, 0xe321f053); // MSR cpsr_c,#0x53 : IRQ permise en SVC, FIQ masquée
    put32(bios, 0x44, 0xe2800001); // ADD r0,r0,#1
    put32(bios, 0x48, 0xe1b0f00e); // MOVS pc,lr
    memory.load_bios(processor, bios);
    Cpu cpu{processor};
    cpu.reset(base, false);
    memory.write32(processor, base, 0xef000000);
    steps(cpu, memory, 3); // SWI, branche du vecteur, MSR en SVC
    const auto saved = cpu.state().spsr();
    cpu.set_interrupt_lines(true);
    cpu.step(memory); // IRQ interrompt SVC, son SPSR conserve le CPSR SVC
    check(cpu.state().mode == CpuMode::irq && cpu.state().spsr() == 0x53, "SPSR IRQ doit décrire le mode SVC");
    cpu.set_interrupt_lines(false);
    steps(cpu, memory, 3);
    check(cpu.state().mode == CpuMode::supervisor && cpu.state().spsr() == saved &&
          cpu.state().registers[14] == base + 4, "IRQ a écrasé SPSR ou LR du SWI");
    steps(cpu, memory, 2);
    check(cpu.state().mode == CpuMode::system && cpu.state().registers[0] == 3 && cpu.state().irq_disabled &&
          cpu.state().registers[15] == base + 4, "retours imbriqués IRQ -> SVC -> System");
}

void ldm_return_test(Processor processor) {
    Memory memory;
    auto bios = synthetic_bios(processor);
    put32(bios, 0x40, 0xe8fd8001); // LDMIA sp!,{r0,pc}^ : retour d'exception
    memory.load_bios(processor, bios);
    Cpu cpu{processor};
    cpu.reset(base, false);
    cpu.set_register(13, 0x02003000);
    cpu.set_register(0, 0xd3);
    cpu.set_register(1, 0xd3);
    cpu.set_register(2, 0xdf);
    memory.write32(processor, base, 0xe121f000); // SVC
    memory.write32(processor, base + 4, 0xe121f002); // System
    memory.write32(processor, base + 8, 0xef000000); // SWI
    memory.write32(processor, base + 12, 0xe121f001); // SVC à nouveau, contrôler SP
    cpu.step(memory);
    cpu.set_register(13, 0x02004000);
    memory.write32(processor, 0x02004000, 42);
    memory.write32(processor, 0x02004004, base + 13); // bit 0 ignoré : T vient du SPSR
    steps(cpu, memory, 4); // System, SWI, branche vecteur, LDM^
    check(cpu.state().mode == CpuMode::system && !cpu.state().thumb && cpu.state().registers[0] == 42 &&
          cpu.state().registers[13] == 0x02003000 && cpu.state().registers[15] == base + 12,
          "LDM^ doit restaurer SPSR et banque System");
    cpu.step(memory);
    check(cpu.state().registers[13] == 0x02004008, "LDM^ doit écrire SP dans la banque SVC avant le retour");
    // Un SPSR au mode réservé est refusé au retour, sans changer la machine.
    memory.write32(processor, base + 16, 0xe161f003); // MSR spsr_c,r3 (r3=0)
    memory.write32(processor, base + 20, 0xe1b0f00e); // MOVS pc,lr
    cpu.step(memory);
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "retour avec mode SPSR réservé accepté");
    check(cpu.state() == before, "retour invalide a modifié les banques");
}

void bios_test(Processor processor) {
    Memory memory;
    const auto start = vectors(processor);
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(processor, start + 8)); }, "BIOS absent accepté");
    memory.load_bios(processor, synthetic_bios(processor));
    check(memory.read32(processor, start + 8) == 0xea00000c, "BIOS au mauvais vecteur");
    expect_failure<ravenemu::Error>([&] { memory.write32(processor, start + 8, 0); }, "BIOS modifiable par le bus");
    const std::array<std::uint8_t, 1> invalid{0};
    expect_failure<ravenemu::RomLoadError>([&] { memory.load_bios(processor, invalid); }, "taille BIOS invalide acceptée");
    memory.write32(processor, base, 1);
    memory.clear_ram();
    check(memory.read32(processor, base) == 0 && memory.read32(processor, start + 8) == 0xea00000c,
          "clear_ram doit conserver le BIOS");
    Cpu cpu{processor};
    Memory absent;
    cpu.reset(base, false);
    absent.write32(processor, base, 0xef000000);
    cpu.step(absent); // entrer dans SVC ne lit pas encore le vecteur
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(absent); }, "fetch BIOS manquant ignoré");
    check(cpu.state() == before && cpu.state().mode == CpuMode::supervisor,
          "fetch fautif doit préserver l'entrée d'exception déjà terminée");
}

void machine_test() {
    ravenemu::nds::Machine machine;
    machine.memory().load_bios(arm9, synthetic_bios(arm9));
    machine.memory().load_bios(arm7, synthetic_bios(arm7));
    std::vector<std::uint8_t> rom(0x20c);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, base); put32(rom, 0x28, base); put32(rom, 0x2c, 8);
    put32(rom, 0x30, 0x208); put32(rom, 0x34, 0x03800001); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 4);
    put32(rom, 0x200, 0xe321f01f); // démasquer les IRQ ARM9
    put32(rom, 0x204, 0xeafffffe);
    put32(rom, 0x208, 0xe7fedf00); // SWI Thumb ; B .
    machine.set_interrupt_lines(arm9, true);
    machine.load_cartridge(ravenemu::nds::Cartridge{rom}); // conserve BIOS, remet les lignes à zéro
    machine.step(arm9);
    machine.step(arm9); // la ligne d'avant le chargement doit avoir été effacée
    check(machine.cpu_state(arm9).mode == CpuMode::system, "chargement a conservé une ancienne ligne IRQ");
    machine.set_interrupt_lines(arm9, true);
    machine.run_steps(arm9, 1);
    check(machine.cpu_state(arm9).mode == CpuMode::irq && machine.cpu_state(arm9).instructions == 2 &&
          machine.cpu_state(arm7).instructions == 0, "budget Machine et isolation des IRQ des deux CPU");
    machine.set_interrupt_lines(arm9, false);
    machine.run_steps(arm9, 3);
    machine.run_steps(arm7, 5);
    check(machine.cpu_state(arm9).mode == CpuMode::system && machine.cpu_state(arm9).registers[0] == 2 &&
          machine.cpu_state(arm7).mode == CpuMode::system && machine.cpu_state(arm7).thumb &&
          machine.cpu_state(arm7).registers[0] == 1, "gestionnaires BIOS synthétiques après chargement ROM");
}

} // namespace

int main() {
    banks_test();
    for (const auto processor : {arm9, arm7}) {
        psr_test(processor);
        for (const auto thumb : {false, true}) {
            software_exception_test(processor, thumb, false);
            software_exception_test(processor, thumb, true);
        }
        interrupts_test(processor);
        nested_exception_test(processor);
        ldm_return_test(processor);
        bios_test(processor);
    }
    machine_test();
}
