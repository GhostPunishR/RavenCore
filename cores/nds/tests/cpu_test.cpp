#include <ravenemu/nds/cpu.hpp>
#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

using ravenemu::nds::Cpu;
using ravenemu::nds::CpuExecutionError;
using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t base = 0x02000000;

namespace {

void steps(Cpu& cpu, Memory& memory, unsigned count) {
    for (unsigned i = 0; i < count; ++i) cpu.step(memory);
}

void arm_program_test(Processor processor) {
    Memory memory;
    // Somme de 1 à 5, écriture en RAM, appel puis retour par BX LR.
    const std::array<std::uint32_t, 14> program{
        0xe3a00000, // mov r0, #0
        0xe3a01005, // mov r1, #5
        0xe0800001, // loop: add r0, r0, r1
        0xe2511001, // subs r1, r1, #1
        0x1afffffc, // bne loop
        0xe59f2010, // ldr r2, [pc, #16] -> littéral à +44
        0xe5820000, // str r0, [r2]
        0xeb000003, // bl function
        0xe5923000, // ldr r3, [r2]
        0xeafffffe, // b .
        0xe1a00000, // nop
        0x02001000, // adresse de résultat
        0xe2800001, // function: add r0, r0, #1
        0xe12fff1e, // bx lr
    };
    for (std::size_t i = 0; i < program.size(); ++i)
        memory.write32(processor, base + static_cast<std::uint32_t>(i * 4), program[i]);
    Cpu cpu{processor};
    cpu.reset(base, false);
    steps(cpu, memory, 23);
    const auto& s = cpu.state();
    check(s.registers[0] == 16 && s.registers[1] == 0 && s.registers[3] == 15, "programme ARM : calcul / RAM");
    check(memory.read32(processor, 0x02001000) == 15, "programme ARM : résultat mémoire");
    check(s.registers[14] == base + 0x20 && s.registers[15] == base + 0x24, "BL, LR, retour ARM");
    check(!s.thumb && s.instructions == 23, "état et compteur ARM");
    cpu.step(memory);
    check(s.registers[15] == base + 0x24 && s.instructions == 24, "branche ARM vers soi-même");
}

void thumb_program_test(Processor processor) {
    Memory memory;
    const std::array<std::uint16_t, 12> program{
        0x2000, // movs r0, #0
        0x2105, // movs r1, #5
        0x1840, // loop: adds r0, r0, r1
        0x3901, // subs r1, #1
        0xd1fc, // bne loop
        0x4a03, // ldr r2, [pc, #12] : PC + 4 aligné, littéral à +24
        0x6010, // str r0, [r2]
        0x6813, // ldr r3, [r2]
        0x7111, // strb r1, [r2, #4]
        0x7914, // ldrb r4, [r2, #4]
        0xe7fe, // b .
        0x46c0, // nop (mov r8, r8)
    };
    for (std::size_t i = 0; i < program.size(); ++i)
        memory.write16(processor, base + static_cast<std::uint32_t>(i * 2), program[i]);
    memory.write32(processor, base + 24, 0x02001000);
    memory.write8(processor, 0x02001004, 0xff);
    Cpu cpu{processor};
    cpu.reset(base, true);
    steps(cpu, memory, 22);
    const auto& s = cpu.state();
    check(s.registers[0] == 15 && s.registers[3] == 15 && s.registers[4] == 0, "programme Thumb : calcul / RAM");
    check(memory.read32(processor, 0x02001000) == 15 && memory.read8(processor, 0x02001004) == 0,
          "programme Thumb : écritures mot et octet");
    check(s.registers[15] == base + 20 && s.thumb && s.instructions == 22, "PC / compteur Thumb");
    cpu.step(memory);
    check(s.registers[15] == base + 20, "branche Thumb vers soi-même");
}

void interworking_test(Processor processor) {
    Memory memory;
    memory.write32(processor, base, 0xe59f0018);      // ldr r0, [pc, #24]
    memory.write32(processor, base + 4, 0xe12fff10);  // bx r0
    memory.write32(processor, base + 8, 0xe3a04042);  // mov r4, #0x42
    memory.write16(processor, base + 0x10, 0x2107);   // movs r1, #7
    memory.write16(processor, base + 0x12, 0x4a04);   // ldr r2, [pc, #16]
    memory.write16(processor, base + 0x14, 0x4710);   // bx r2
    memory.write32(processor, base + 0x20, base + 0x11);
    memory.write32(processor, base + 0x24, base + 8);
    Cpu cpu{processor};
    cpu.reset(base, false);
    steps(cpu, memory, 2);
    check(cpu.state().thumb && cpu.state().registers[15] == base + 0x10, "BX ARM -> Thumb");
    steps(cpu, memory, 3);
    check(!cpu.state().thumb && cpu.state().registers[15] == base + 8, "BX Thumb -> ARM");
    cpu.step(memory);
    check(cpu.state().registers[1] == 7 && cpu.state().registers[4] == 0x42, "exécution après BX");

    cpu.reset(base, false);
    memory.write32(processor, base, 0xe1a0400f); // mov r4, pc
    cpu.step(memory);
    check(cpu.state().registers[4] == base + 8, "PC architectural ARM");
    cpu.reset(base, true);
    memory.write16(processor, base, 0x467c); // mov r4, pc
    memory.write16(processor, base + 2, 0x46a0); // mov r8, r4
    memory.write16(processor, base + 4, 0x4444); // add r4, r8
    steps(cpu, memory, 3);
    check(cpu.state().registers[8] == base + 4 && cpu.state().registers[4] == 2 * (base + 4),
          "PC architectural Thumb et registres hauts");
}

void arithmetic_flags_test(Processor processor) {
    struct Case {
        std::uint32_t instruction, x, y, result;
        bool carry_in, n, z, c, v;
    };
    const std::array<Case, 10> cases{{
        {0xe0902001, 0xffffffff, 1, 0, false, false, true, true, false}, // ADDS
        {0xe0902001, 0x7fffffff, 1, 0x80000000, false, true, false, false, true},
        {0xe0902001, 0x80000000, 0x80000000, 0, false, false, true, true, true},
        {0xe0502001, 0, 1, 0xffffffff, false, true, false, false, false}, // SUBS
        {0xe0502001, 0x80000000, 1, 0x7fffffff, false, false, false, true, true},
        {0xe0502001, 1, 1, 0, false, false, true, true, false},
        {0xe0b02001, 0xffffffff, 0, 0, true, false, true, true, false}, // ADCS
        {0xe0b02001, 0x7fffffff, 0, 0x80000000, true, true, false, false, true},
        {0xe0d02001, 0, 0, 0xffffffff, false, true, false, false, false}, // SBCS
        {0xe0d02001, 0x80000000, 0x7fffffff, 0, false, false, true, true, true},
    }};
    Memory memory;
    Cpu cpu{processor};
    for (const auto& value : cases) {
        cpu.reset(base, false);
        cpu.set_register(0, value.x);
        cpu.set_register(1, value.y);
        memory.write32(processor, base, value.carry_in ? 0xe3530000 : 0xe3530001); // CMP r3,#0/#1
        memory.write32(processor, base + 4, value.instruction);
        steps(cpu, memory, 2);
        const auto& s = cpu.state();
        check(s.registers[2] == value.result, "résultat arithmétique ARM");
        check(s.negative == value.n && s.zero == value.z && s.carry == value.c && s.overflow == value.v,
              "drapeaux NZCV arithmétiques ARM");
    }
    cpu.reset(base, true);
    cpu.set_register(0, 0x7fffffff);
    memory.write16(processor, base, 0x3001); // adds r0, #1
    memory.write16(processor, base + 2, 0x2100); // movs r1, #0 : préserve C/V
    steps(cpu, memory, 2);
    check(cpu.state().registers[0] == 0x80000000 && cpu.state().zero && !cpu.state().negative &&
          !cpu.state().carry && cpu.state().overflow, "MOVS Thumb doit préserver C/V");
}

void shift_test(Processor processor) {
    struct Case { std::uint32_t instruction, x, result; bool carry; };
    // MOVS r2,r0,shift : limites ARM immédiates et RRX, C initial à 1.
    const std::array<Case, 8> cases{{
        {0xe1b02000, 0, 0, true},                     // LSL #0 préserve C
        {0xe1b02080, 0x80000001, 2, true},            // LSL #1
        {0xe1b02020, 0x80000001, 0, true},            // LSR #0 signifie #32
        {0xe1b02040, 0x80000001, 0xffffffff, true},   // ASR #0 signifie #32
        {0xe1b020c0, 0x80000000, 0xc0000000, false},  // ASR #1
        {0xe1b02060, 2, 0x80000001, false},          // RRX
        {0xe1b02260, 0x12345678, 0x81234567, true},  // ROR #4
        {0xe3b02102, 0, 0x80000000, true},           // MOVS #2 ROR #2
    }};
    Memory memory;
    Cpu cpu{processor};
    for (const auto& value : cases) {
        cpu.reset(base, false);
        cpu.set_register(0, value.x);
        memory.write32(processor, base, 0xe3530000); // cmp r3,#0 : C=1
        memory.write32(processor, base + 4, value.instruction);
        steps(cpu, memory, 2);
        check(cpu.state().registers[2] == value.result && cpu.state().carry == value.carry,
              "décalage immédiat ARM / retenue");
    }
    struct RegisterShift { std::uint16_t instruction; std::uint32_t amount, result; bool carry; };
    const std::array<RegisterShift, 10> register_cases{{
        {0x4088, 0, 0x80000001, true},       // lsls r0,r1 : pas de décalage
        {0x4088, 32, 0, true},
        {0x4088, 33, 0, false},
        {0x40c8, 32, 0, true},              // lsrs r0,r1
        {0x40c8, 255, 0, false},
        {0x4108, 32, 0xffffffff, true},      // asrs r0,r1
        {0x4108, 255, 0xffffffff, true},
        {0x41c8, 32, 0x80000001, true},      // rors r0,r1
        {0x41c8, 33, 0xc0000000, true},
        {0x4088, 256, 0x80000001, true},     // seuls les 8 bits bas comptent
    }};
    for (const auto& value : register_cases) {
        cpu.reset(base, true);
        cpu.set_register(0, 0x80000001);
        cpu.set_register(1, value.amount);
        memory.write16(processor, base, 0x2a00); // cmp r2,#0 : C=1
        memory.write16(processor, base + 2, value.instruction);
        steps(cpu, memory, 2);
        check(cpu.state().registers[0] == value.result && cpu.state().carry == value.carry,
              "décalage Thumb par registre / retenue");
    }
}

void alu_table_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    // Un jeu d'opérandes fixe pour chaque opération ARM, dans l'ordre des
    // encodages AND..MVN. Les opérations sans S doivent conserver NZCV.
    const std::array<std::uint32_t, 16> arm_results{
        0x30, 0xc3, 0xbd, 0xffffff43, 0x123, 0x124, 0xbd, 0xffffff43,
        0, 0, 0, 0, 0xf3, 0x33, 0xc0, 0xffffffcc,
    };
    for (std::uint32_t opcode = 0; opcode < 16; ++opcode) {
        const bool test = opcode >= 8 && opcode <= 11;
        cpu.reset(base, false);
        cpu.set_register(0, 0xf0);
        cpu.set_register(1, 0x33);
        cpu.set_register(2, 0xdeadbeef);
        memory.write32(processor, base, 0xe3530000); // CMP r3,#0 -> NZCV=0110
        memory.write32(processor, base + 4, (test ? 0xe0100001U : 0xe0002001U) | (opcode << 21U));
        cpu.step(memory);
        const auto flags = cpu.state();
        cpu.step(memory);
        const auto& s = cpu.state();
        if (test) {
            check(s.registers[0] == 0xf0 && s.registers[2] == 0xdeadbeef, "TST/TEQ/CMP/CMN a écrit un registre");
            check(!s.negative && !s.zero && s.carry == (opcode != 11) && !s.overflow,
                  "drapeaux TST/TEQ/CMP/CMN");
        } else {
            check(s.registers[2] == arm_results[opcode], "table des opérations ALU ARM");
            check(s.negative == flags.negative && s.zero == flags.zero && s.carry == flags.carry &&
                  s.overflow == flags.overflow, "opération sans S a modifié NZCV");
        }
    }
    // Les 16 opérations Thumb sur registres bas, x=0xf0, y=3, C initial=1.
    const std::array<std::uint32_t, 16> thumb_results{
        0, 0xf3, 0x780, 0x1e, 0x1e, 0xf4, 0xed, 0x1e,
        0, 0xfffffffd, 0xed, 0xf3, 0xf3, 0x2d0, 0xf0, 0xfffffffc,
    };
    const std::array<bool, 16> carry{
        true, true, false, false, false, false, true, false,
        true, false, true, false, true, true, true, true,
    };
    for (std::uint32_t opcode = 0; opcode < 16; ++opcode) {
        cpu.reset(base, true);
        cpu.set_register(0, 0xf0);
        cpu.set_register(1, 3);
        memory.write16(processor, base, 0x2a00); // CMP r2,#0 : C=1
        memory.write16(processor, base + 2, static_cast<std::uint16_t>(0x4008U | (opcode << 6U)));
        steps(cpu, memory, 2);
        const auto& s = cpu.state();
        const bool test = opcode == 8 || opcode == 10 || opcode == 11;
        check(s.registers[0] == (test ? 0xf0U : thumb_results[opcode]), "table des opérations ALU Thumb");
        check(s.negative == ((thumb_results[opcode] >> 31U) != 0) &&
              s.zero == (thumb_results[opcode] == 0) && s.carry == carry[opcode] && !s.overflow,
              "drapeaux ALU Thumb");
    }
}

void condition_test(Processor processor) {
    // CMP x,y suivi de MOV{condition} r2,#1. Les tables suivent ARM ARM,
    // Condition codes : EQ NE CS CC MI PL VS VC HI LS GE LT GT LE AL.
    struct Case { std::uint32_t x, y; std::array<bool, 15> expected; };
    const std::array<Case, 4> cases{{
        {0, 0, {true,false,true,false,false,true,false,true,false,true,true,false,false,true,true}},
        {0, 1, {false,true,false,true,true,false,false,true,false,true,false,true,false,true,true}},
        {2, 1, {false,true,true,false,false,true,false,true,true,false,true,false,true,false,true}},
        {0x80000000, 1, {false,true,true,false,false,true,true,false,true,false,false,true,false,true,true}},
    }};
    Memory memory;
    Cpu cpu{processor};
    for (const auto& value : cases) {
        for (std::uint32_t cond = 0; cond < 15; ++cond) {
            cpu.reset(base, false);
            cpu.set_register(0, value.x);
            cpu.set_register(1, value.y);
            memory.write32(processor, base, 0xe1500001); // cmp r0,r1
            memory.write32(processor, base + 4, (cond << 28U) | 0x03a02001U);
            steps(cpu, memory, 2);
            check(cpu.state().registers[2] == (value.expected[cond] ? 1U : 0U), "condition ARM");
            check(cpu.state().registers[15] == base + 8 && cpu.state().instructions == 2,
                  "condition fausse doit aussi avancer le CPU");
        }
    }
    // Un SWI dont la condition est fausse ne doit pas lever d'erreur.
    cpu.reset(base, false);
    memory.write32(processor, base, 0x0f000000); // swieq #0 ; Z initial à 0
    cpu.step(memory);
    check(cpu.state().registers[15] == base + 4, "instruction ignorée par condition");
}

void transfer_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    cpu.reset(base, false);
    cpu.set_register(0, 0x02001000);
    cpu.set_register(1, 0x1234abcd);
    const std::array<std::uint32_t, 5> arm{
        0xe5a01004, // str r1,[r0,#4]!
        0xe4102004, // ldr r2,[r0],#-4
        0xe5c01001, // strb r1,[r0,#1]
        0xe5d03001, // ldrb r3,[r0,#1]
        0xe5904004, // ldr r4,[r0,#4]
    };
    for (std::size_t i = 0; i < arm.size(); ++i)
        memory.write32(processor, base + static_cast<std::uint32_t>(i * 4), arm[i]);
    steps(cpu, memory, 5);
    check(cpu.state().registers[0] == 0x02001000 && cpu.state().registers[2] == 0x1234abcd &&
          cpu.state().registers[3] == 0xcd && cpu.state().registers[4] == 0x1234abcd,
          "transferts ARM pré/post, soustraction, octet et mot");

    cpu.reset(base, true);
    cpu.set_register(0, 0x02001000);
    cpu.set_register(1, 0x1234abcd);
    cpu.set_register(13, 0x02002000);
    const std::array<std::uint16_t, 4> thumb{
        0x8041, // strh r1,[r0,#2]
        0x8842, // ldrh r2,[r0,#2]
        0x9101, // str r1,[sp,#4]
        0x9b01, // ldr r3,[sp,#4]
    };
    for (std::size_t i = 0; i < thumb.size(); ++i)
        memory.write16(processor, base + static_cast<std::uint32_t>(i * 2), thumb[i]);
    steps(cpu, memory, 4);
    check(cpu.state().registers[2] == 0xabcd && cpu.state().registers[3] == 0x1234abcd &&
          memory.read32(processor, 0x02002004) == 0x1234abcd, "demi-mots Thumb / transferts SP");
}

void fault_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    const std::array<std::uint32_t, 11> unsupported{
        0xe1200070, // BKPT : débogueur matériel absent
        0xee070f15, // CP15
        0xe8fd8000, // LDM avec restauration SPSR en mode System sans SPSR
        0xe10ff000, // MRS pc : encodage imprévisible
        0xe12fff3f, // BLX pc : variante imprévisible
        0xf0000000, // espace inconditionnel hors BLX
        0xe10f1090, // SWP : échange atomique encore absent
        0xe1b0f000, // MOVS pc en mode System sans SPSR
        0xe7901012, // LDR offset décalé par registre : encodage invalide
        0xe5d0f000, // LDRB pc : variante imprévisible
        0xe5a00004, // STR r0,[r0,#4]! : variante à writeback ambiguë
    };
    for (const auto raw : unsupported) {
        cpu.reset(base, false);
        cpu.set_register(0, 0x02001000);
        memory.write32(processor, base, raw);
        const auto before = cpu.state();
        try {
            cpu.step(memory);
            check(false, "instruction ARM non prise en charge ignorée");
        } catch (const CpuExecutionError& error) {
            check(error.processor() == processor && error.address() == base && !error.thumb() &&
                  error.instruction() == raw, "contexte d'erreur ARM");
            check(std::string{error.what()}.find("PC=0x02000000") != std::string::npos,
                  "diagnostic lisible sans contexte externe");
        }
        check(cpu.state() == before, "instruction refusée a modifié le CPU");
    }
    for (const auto raw : std::array<std::uint16_t, 6>{0xbe00, 0xb400, 0xbc00, 0xe801, 0x47f8, 0xb600}) {
        cpu.reset(base, true);
        memory.write16(processor, base, raw);
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "instruction Thumb absente ignorée");
        check(cpu.state() == before, "instruction Thumb refusée a modifié le CPU");
    }
    cpu.reset(0x04000060, false);
    try {
        cpu.step(memory);
        check(false, "fetch hors RAM accepté");
    } catch (const CpuExecutionError& error) {
        check(!error.instruction() && error.address() == 0x04000060, "erreur fetch : opcode doit être absent");
    }
    memory.set_wram_control(3);
    cpu.reset(base, false);
    cpu.set_register(0, 0x0400005c);
    cpu.set_register(1, 0x12345678);
    memory.write32(processor, base, 0xe5a01004); // STR r1,[r0,#4]! vers MMIO
    {
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "store non mappé accepté");
        check(cpu.state() == before && memory.read32(processor, 0x02fffffc) == 0,
              "store en erreur : writeback ou mémoire modifiés");
    }
    cpu.reset(base, false);
    cpu.set_register(0, 0x02001001);
    memory.write32(processor, base, 0xe5901000); // LDR non aligné, pas de rotation implémentée
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "load non aligné accepté");
    check(cpu.state() == before, "load en erreur a modifié le CPU");
    expect_failure<std::invalid_argument>([&] { cpu.reset(base + 2, false); }, "reset non aligné accepté");
    expect_failure<std::out_of_range>([&] { cpu.set_register(16, 0); }, "registre inexistant accepté");
    check(cpu.state() == before, "API invalide a modifié le CPU");
}

void put32(std::vector<std::uint8_t>& rom, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) rom[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

void machine_test() {
    std::vector<std::uint8_t> rom(0x230);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, base); put32(rom, 0x28, base); put32(rom, 0x2c, 0x1c);
    put32(rom, 0x30, 0x220); put32(rom, 0x34, 0x03800001); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 0x10);
    put32(rom, 0x200, 0xe3a00007); // ARM9 : mov r0,#7
    put32(rom, 0x204, 0xe59f100c); // ldr r1,[pc,#12]
    put32(rom, 0x208, 0xe5810000); // str r0,[r1]
    put32(rom, 0x20c, 0xeafffffe); // b .
    put32(rom, 0x218, 0x02001000);
    put32(rom, 0x220, 0x68084902); // ARM7 Thumb : ldr r1,[pc,#8] ; ldr r0,[r1]
    put32(rom, 0x224, 0x60483001); // adds r0,#1 ; str r0,[r1,#4]
    put32(rom, 0x228, 0x46c0e7fe); // b . ; nop
    put32(rom, 0x22c, 0x02001000);
    ravenemu::nds::Machine machine;
    expect_failure<ravenemu::Error>([&] { machine.step(arm9); }, "machine vide exécutée");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    machine.run_steps(arm9, 3);
    check(machine.cpu_state(arm7).instructions == 0, "le budget ARM9 a avancé ARM7");
    machine.run_steps(arm7, 4);
    check(machine.memory().read32(arm9, 0x02001004) == 8, "échange ARM9/ARM7 par RAM");
    check(machine.cpu_state(arm9).registers[0] == 7 && machine.cpu_state(arm7).registers[0] == 8,
          "registres ARM9/ARM7 non indépendants");
    auto copy = machine;
    copy.memory().write32(arm9, base + 12, 0xe3a00063); // mov r0,#99 dans la copie
    copy.step(arm9);
    check(copy.cpu_state(arm9).registers[0] == 99 && machine.cpu_state(arm9).registers[0] == 7,
          "CPU d'une copie attaché à l'ancienne RAM");
    const auto arm9_before = machine.cpu_state(arm9);
    const auto arm7_before = machine.cpu_state(arm7);
    auto invalid = rom;
    put32(invalid, 0x34, 0x04000001); put32(invalid, 0x38, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(ravenemu::nds::Cartridge{invalid}); },
                                         "ROM invalide acceptée");
    check(machine.cpu_state(arm9) == arm9_before && machine.cpu_state(arm7) == arm7_before,
          "chargement refusé a effacé les CPU");
    machine.run_steps(arm9, 100);
    check(machine.cpu_state(arm9).instructions == 103 && machine.cpu_state(arm9).registers[15] == base + 12,
          "budget borné sur boucle infinie");
    machine.run_steps(arm7, 0);
    check(machine.cpu_state(arm7) == arm7_before, "budget nul a avancé le CPU");
    machine.memory().write32(arm9, base + 12, 0xe3a00009); // une instruction valide
    machine.memory().write32(arm9, base + 16, 0xee070f15); // puis accès CP15 non implémenté
    expect_failure<CpuExecutionError>([&] { machine.run_steps(arm9, 2); }, "budget ignore une erreur");
    check(machine.cpu_state(arm9).registers[0] == 9 && machine.cpu_state(arm9).registers[15] == base + 16 &&
          machine.cpu_state(arm9).instructions == 104, "erreur doit conserver le progrès avant la faute");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    check(machine.cpu_state(arm9).instructions == 0 && machine.cpu_state(arm7).instructions == 0 &&
          machine.cpu_state(arm9).registers[0] == 0 && machine.cpu_state(arm7).thumb,
          "rechargement doit réinitialiser les CPU");
}

} // namespace

int main() {
    for (const auto processor : {arm9, arm7}) {
        arm_program_test(processor);
        thumb_program_test(processor);
        interworking_test(processor);
        arithmetic_flags_test(processor);
        shift_test(processor);
        alu_table_test(processor);
        condition_test(processor);
        transfer_test(processor);
        fault_test(processor);
    }
    machine_test();
}
