#include <ravenemu/nds/cpu.hpp>
#include "check.hpp"

#include <array>

using ravenemu::nds::Cpu;
using ravenemu::nds::CpuExecutionError;
using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t base = 0x02000000;
constexpr std::uint32_t data = 0x02001000;

namespace {

void steps(Cpu& cpu, Memory& memory, unsigned count) {
    for (unsigned i = 0; i < count; ++i) cpu.step(memory);
}

void same_flags(const ravenemu::nds::CpuState& a, const ravenemu::nds::CpuState& b) {
    check(a.negative == b.negative && a.zero == b.zero && a.carry == b.carry && a.overflow == b.overflow,
          "instruction sans mise à jour des drapeaux a modifié NZCV");
}

void prime_flags(Cpu& cpu, Memory& memory, Processor processor) {
    cpu.set_register(9, 0x80000000);
    cpu.set_register(10, 0x80000000);
    memory.write32(processor, base, 0xe099800a); // ADDS r8,r9,r10 -> N=0 Z=1 C=1 V=1
    cpu.step(memory);
}

void signed_program_test(Processor processor, bool thumb) {
    Memory memory;
    Cpu cpu{processor};
    const std::array<std::uint16_t, 3> x{0xfffd, 7, 0xfffe}; // -3, 7, -2
    const std::array<std::uint16_t, 3> y{5, 0xfffc, 9};       // 5, -4, 9
    for (std::size_t i = 0; i < x.size(); ++i) {
        memory.write16(processor, data + static_cast<std::uint32_t>(i * 2), x[i]);
        memory.write16(processor, data + 0x100 + static_cast<std::uint32_t>(i * 2), y[i]);
    }
    if (!thumb) {
        const std::array<std::uint32_t, 10> program{
            0xe3a04000, // mov r4,#0 : index octet
            0xe3a05000, // mov r5,#0 : somme
            0xe19020f4, // loop: ldrsh r2,[r0,r4]
            0xe19130f4, // ldrsh r3,[r1,r4]
            0xe0255392, // mla r5,r2,r3,r5
            0xe2844002, // add r4,r4,#2
            0xe3540006, // cmp r4,#6
            0x1afffff9, // bne loop
            0xe5865000, // str r5,[r6]
            0xeafffffe, // b .
        };
        for (std::size_t i = 0; i < program.size(); ++i)
            memory.write32(processor, base + static_cast<std::uint32_t>(i * 4), program[i]);
    } else {
        const std::array<std::uint16_t, 11> program{
            0x2400, // movs r4,#0
            0x2500, // movs r5,#0
            0x5f02, // loop: ldrsh r2,[r0,r4]
            0x5f0b, // ldrsh r3,[r1,r4]
            0x435a, // muls r2,r3
            0x18ad, // adds r5,r5,r2
            0x3402, // adds r4,#2
            0x2c06, // cmp r4,#6
            0xd1f8, // bne loop
            0x6035, // str r5,[r6]
            0xe7fe, // b .
        };
        for (std::size_t i = 0; i < program.size(); ++i)
            memory.write16(processor, base + static_cast<std::uint32_t>(i * 2), program[i]);
    }
    cpu.reset(base, thumb);
    cpu.set_register(0, data);
    cpu.set_register(1, data + 0x100);
    cpu.set_register(6, data + 0x200);
    steps(cpu, memory, thumb ? 24U : 21U);
    check(cpu.state().registers[5] == 0xffffffc3 && memory.read32(processor, data + 0x200) == 0xffffffc3,
          "produit scalaire signé : -3*5 + 7*(-4) + (-2)*9 doit valoir -61");
    check(cpu.state().registers[4] == 6 && cpu.state().registers[15] == base + (thumb ? 20U : 36U),
          "programme de calcul signé : boucle ou PC final");
}

void multiply_test(Processor processor) {
    struct Case {
        std::uint32_t raw, x, y, lo, hi, result_lo, result_hi;
        bool wide, flags, negative, zero;
    };
    const std::array<Case, 11> cases{{
        {0xe0020190, 0xffffffff, 2, 0, 0, 0xfffffffe, 0, false, false, false, true}, // MUL
        {0xe0223190, 0x80000000, 2, 0, 7, 7, 0, false, false, false, true}, // MLA
        {0xe0120190, 0x80000000, 2, 0, 0, 0, 0, false, true, false, true}, // MULS
        {0xe0323190, 0x7fffffff, 2, 0, 1, 0xffffffff, 0, false, true, true, false}, // MLAS
        {0xe0832190, 0xffffffff, 0xffffffff, 0, 0, 1, 0xfffffffe, true, false, false, true}, // UMULL
        {0xe0d32190, 0xffffffff, 2, 0, 0, 0xfffffffe, 0xffffffff, true, true, true, false}, // SMULLS
        {0xe0c32190, 0x80000000, 0xffffffff, 0, 0, 0x80000000, 0, true, false, false, true}, // SMULL
        {0xe0a32190, 0xffffffff, 2, 0xfffffffe, 0xffffffff, 0xfffffffc, 1, true, false, false, true}, // UMLAL
        {0xe0f32190, 0xffffffff, 2, 1, 0, 0xffffffff, 0xffffffff, true, true, true, false}, // SMLALS
        {0xe0b32190, 1, 1, 0xffffffff, 0xffffffff, 0, 0, true, true, false, true}, // UMLALS
        {0xe0932190, 0x80000000, 1, 0, 0, 0x80000000, 0, true, true, false, false}, // UMULLS
    }};
    Memory memory;
    Cpu cpu{processor};
    for (const auto value : cases) {
        cpu.reset(base, false);
        prime_flags(cpu, memory, processor);
        cpu.set_register(0, value.x);
        cpu.set_register(1, value.y);
        cpu.set_register(2, value.lo);
        cpu.set_register(3, value.hi);
        const auto before = cpu.state();
        memory.write32(processor, base + 4, value.raw);
        cpu.step(memory);
        const auto& s = cpu.state();
        check(s.registers[2] == value.result_lo && (!value.wide || s.registers[3] == value.result_hi),
              "résultat de multiplication/accumulation ARM 32/64 bits");
        if (value.flags) check(s.negative == value.negative && s.zero == value.zero, "drapeaux NZ du produit");
        else same_flags(s, before);
        check(s.carry == before.carry && s.overflow == before.overflow, "modèle MUL : C/V doivent rester inchangés");
        check(s.registers[0] == value.x && s.registers[1] == value.y, "MUL a modifié une source indépendante");
    }
    // ARM9 permet le recouvrement des sources et destinations, ARM7 le refuse.
    for (const auto raw : {0xe0000190U, 0xe0810190U, 0xe0a10190U}) {
        cpu.reset(base, false);
        cpu.set_register(0, 3);
        cpu.set_register(1, 2);
        memory.write32(processor, base, raw);
        const auto before = cpu.state();
        if (processor == arm7) {
            expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "MUL ARM7 avec source/destination interdite");
            check(cpu.state() == before, "MUL refusé a modifié ARM7");
        } else {
            cpu.step(memory);
            check(cpu.state().registers[0] == (raw == 0xe0a10190 ? 9U : 6U), "MUL ARM9 lit une source trop tard");
            if (raw != 0xe0000190) check(cpu.state().registers[1] == (raw == 0xe0a10190 ? 2U : 0U),
                                       "produit long ARM9 avec recouvrement");
        }
    }
}

void register_shift_test(Processor processor) {
    struct Case { unsigned type, amount; std::uint32_t result; bool carry; };
    const std::array<Case, 12> cases{{
        {0, 0, 0x80000001, true}, {0, 1, 2, true}, {0, 31, 0x80000000, false},
        {0, 32, 0, true}, {0, 33, 0, false}, {1, 32, 0, true}, {1, 255, 0, false},
        {2, 1, 0xc0000000, true}, {2, 255, 0xffffffff, true},
        {3, 32, 0x80000001, true}, {3, 33, 0xc0000000, true}, {0, 256, 0x80000001, true},
    }};
    Memory memory;
    Cpu cpu{processor};
    for (const auto value : cases) {
        cpu.reset(base, false);
        prime_flags(cpu, memory, processor);
        cpu.set_register(0, 0x80000001);
        cpu.set_register(1, value.amount);
        memory.write32(processor, base + 4, 0xe1b02110U | (value.type << 5U)); // MOVS r2,r0,shift r1
        cpu.step(memory);
        check(cpu.state().registers[2] == value.result && cpu.state().carry == value.carry && cpu.state().overflow,
              "décalage ARM par registre : valeur, C ou préservation de V");
    }
    cpu.reset(base, false);
    prime_flags(cpu, memory, processor);
    cpu.set_register(0, 0);
    cpu.set_register(1, 1);
    cpu.set_register(3, 1);
    memory.write32(processor, base + 4, 0xe0b02311); // ADCS r2,r0,r1,LSL r3
    cpu.step(memory);
    check(cpu.state().registers[2] == 3 && !cpu.state().carry && !cpu.state().overflow,
          "ADC doit utiliser C d'entrée, pas la retenue du décalage");
    cpu.reset(base, false);
    prime_flags(cpu, memory, processor);
    cpu.set_register(0, 1);
    cpu.set_register(1, 1);
    const auto before = cpu.state();
    memory.write32(processor, base + 4, 0xe1a02110); // MOV r2,r0,LSL r1 (sans S)
    cpu.step(memory);
    check(cpu.state().registers[2] == 2, "décalage ARM sans S");
    same_flags(cpu.state(), before);
}

void halfword_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    cpu.reset(base, false);
    prime_flags(cpu, memory, processor);
    cpu.set_register(0, data);
    cpu.set_register(1, 0x1234abcd);
    cpu.set_register(6, 2);
    memory.write8(processor, data + 1, 0x80);
    memory.write16(processor, data + 2, 0x8001);
    const std::array<std::uint32_t, 6> program{
        0xe1e011b2, // STRH r1,[r0,#0x12]!
        0xe05021b2, // LDRH r2,[r0],#-0x12
        0xe1d030d1, // LDRSB r3,[r0,#1]
        0xe1d040f2, // LDRSH r4,[r0,#2]
        0xe19050b6, // LDRH r5,[r0,r6]
        0xe10010b6, // STRH r1,[r0,-r6]
    };
    for (std::size_t i = 0; i < program.size(); ++i)
        memory.write32(processor, base + 4 + static_cast<std::uint32_t>(i * 4), program[i]);
    const auto before = cpu.state();
    steps(cpu, memory, 6);
    const auto& s = cpu.state();
    check(s.registers[0] == data && s.registers[2] == 0xabcd && s.registers[3] == 0xffffff80 &&
          s.registers[4] == 0xffff8001 && s.registers[5] == 0x8001, "transferts ARM demi-mot / signés");
    check(memory.read16(processor, data + 0x12) == 0xabcd && memory.read16(processor, data - 2) == 0xabcd,
          "STRH tronque aux 16 bits bas, pré/post et offset registre négatif");
    same_flags(s, before);
    cpu.reset(base, false);
    memory.write32(processor, base, 0xe1df21f0); // LDRSH r2,[pc,#16]
    memory.write16(processor, base + 24, 0x8001);
    cpu.step(memory);
    check(cpu.state().registers[2] == 0xffff8001, "LDRSH relatif au PC ARM");
}

void thumb_transfer_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    const std::array<std::uint32_t, 8> results{
        0xaabbccdd, 0xccdd, 0xdd, 0xffffff81, 0x12348081, 0x8081, 0x81, 0xffff8081,
    };
    for (std::uint32_t type = 0; type < 8; ++type) {
        cpu.reset(base, true);
        cpu.set_register(0, data);
        cpu.set_register(1, 0xaabbccdd);
        cpu.set_register(2, 4);
        memory.write16(processor, base, 0x2b00); // CMP r3,#0
        memory.write16(processor, base + 2, static_cast<std::uint16_t>(0x5081U | (type << 9U)));
        memory.write32(processor, data + 4, 0x12348081);
        cpu.step(memory);
        const auto before = cpu.state();
        cpu.step(memory);
        if (type >= 3) check(cpu.state().registers[1] == results[type], "lecture Thumb indexée signée/non signée");
        else {
            const auto actual = type == 0 ? memory.read32(processor, data + 4) :
                                type == 1 ? memory.read16(processor, data + 4) : memory.read8(processor, data + 4);
            check(actual == results[type], "écriture Thumb indexée mot/demi-mot/octet");
        }
        same_flags(cpu.state(), before);
    }
    struct SignedCase { std::uint16_t value; std::uint32_t expected; bool half; };
    for (const auto value : std::array<SignedCase, 6>{{
             {0x7f, 0x7f, false}, {0x80, 0xffffff80, false}, {0xff, 0xffffffff, false},
             {0x7fff, 0x7fff, true}, {0x8000, 0xffff8000, true}, {0xffff, 0xffffffff, true}}}) {
        for (const auto thumb : {false, true}) {
            cpu.reset(base, thumb);
            cpu.set_register(0, data);
            cpu.set_register(2, 0);
            memory.write16(processor, data, value.value);
            if (thumb) memory.write16(processor, base, value.half ? 0x5e81 : 0x5681); // LDRSH/SB r1,[r0,r2]
            else memory.write32(processor, base, value.half ? 0xe1d010f0 : 0xe1d010d0);
            cpu.step(memory);
            check(cpu.state().registers[1] == value.expected, "frontière de signe 8/16 bits ARM/Thumb");
        }
    }
}

void scaled_transfer_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    cpu.reset(base, false);
    prime_flags(cpu, memory, processor);
    cpu.set_register(0, data);
    cpu.set_register(1, 3);
    cpu.set_register(2, 0xaabbccdd);
    memory.write32(processor, base + 4, 0xe7a02101); // STR r2,[r0,r1,LSL #2]!
    memory.write32(processor, base + 8, 0xe7103001); // LDR r3,[r0,-r1] : décalage 3 -> adresse impaire
    const auto before = cpu.state();
    cpu.step(memory);
    check(cpu.state().registers[0] == data + 12 && memory.read32(processor, data + 12) == 0xaabbccdd,
          "STR indexé avec LSL et writeback");
    same_flags(cpu.state(), before);
    cpu.set_register(1, 12);
    memory.write32(processor, data, 0x12345678);
    cpu.step(memory);
    check(cpu.state().registers[3] == 0x12345678, "LDR indexé négatif");
    cpu.reset(base, false);
    cpu.set_register(0, data);
    cpu.set_register(1, 8);
    memory.write32(processor, data + 4, 0x56789abc);
    memory.write32(processor, base, 0xe79020a1); // LDR r2,[r0,r1,LSR #1]
    cpu.step(memory);
    check(cpu.state().registers[2] == 0x56789abc, "offset registre avec LSR immédiat");
}

void rejected_variant_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    const std::array<std::uint32_t, 11> invalid{
        0xe1a02f10, // Rs=pc pour décalage par registre
        0xe1a0211f, // Rm=pc pour décalage par registre
        0xe08f2110, // Rn=pc pour décalage par registre
        0xe0020f90, // multiplication avec pc
        0xe0822190, // produit long : destinations identiques
        0xe1c020d0, // LDRD : pas un stockage signé, encore absent
        0xe1c020f0, // STRD encore absent
        0xe19021b1, // offset demi-mot registre : champ haut non nul
        0xe1d0f0b0, // LDRH pc
        0xe1f000b2, // LDRH r0,[r0,#2]! : base/destination identiques
        0xe7901012, // offset mot décalé par registre (encodage invalide)
    };
    for (const auto raw : invalid) {
        cpu.reset(base, false);
        cpu.set_register(0, data);
        cpu.set_register(1, 2);
        memory.write32(processor, base, raw);
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "variante réservée ou absente acceptée");
        check(cpu.state() == before, "variante refusée a modifié les registres");
    }
    for (const auto raw : {0xe1f020b1U, 0xe1e010b1U, 0xe1d020f1U}) { // LDRH!, STRH!, LDRSH impairs
        cpu.reset(base, false);
        cpu.set_register(0, data);
        cpu.set_register(1, 0xaabbccdd);
        memory.write32(processor, data, 0x12345678);
        memory.write32(processor, base, raw);
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "demi-mot non aligné accepté");
        check(cpu.state() == before && memory.read32(processor, data) == 0x12345678,
              "demi-mot refusé : registres, writeback ou RAM modifiés");
    }
}

} // namespace

int main() {
    for (const auto processor : {arm9, arm7}) {
        signed_program_test(processor, false);
        signed_program_test(processor, true);
        multiply_test(processor);
        register_shift_test(processor);
        halfword_test(processor);
        thumb_transfer_test(processor);
        scaled_transfer_test(processor);
        rejected_variant_test(processor);
    }
}
