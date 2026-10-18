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
constexpr std::uint32_t stack_top = 0x02003000;

namespace {

void steps(Cpu& cpu, Memory& memory, unsigned count) {
    for (unsigned i = 0; i < count; ++i) cpu.step(memory);
}

void nested_thumb_calls_test(Processor processor) {
    Memory memory;
    // main -> outer -> inner. LR doit survivre à l'appel imbriqué sur la pile.
    const std::array<std::uint16_t, 22> program{
        0x2001, // +00 movs r0,#1
        0xf000, // +02 bl outer : préfixe
        0xf805, // +04 suffixe -> +0x10 ; LR = +7
        0x2155, // +06 movs r1,#0x55 (retour dans main)
        0xe7fe, // +08 b .
        0x46c0, 0x46c0, 0x46c0,
        0xb510, // +10 outer: push {r4,lr}
        0x2409, // +12 movs r4,#9
        0x3002, // +14 adds r0,#2
        0xf000, // +16 bl inner
        0xf803, // +18 -> +0x20 ; LR = +0x1b
        0x1900, // +1a adds r0,r0,r4
        0xbd10, // +1c pop {r4,pc} -> main
        0x46c0,
        0xb510, // +20 inner: push {r4,lr}
        0x2463, // +22 movs r4,#99
        0xb081, // +24 sub sp,#4
        0x3004, // +26 adds r0,#4
        0xb001, // +28 add sp,#4
        0xbd10, // +2a pop {r4,pc} -> outer
    };
    for (std::size_t i = 0; i < program.size(); ++i)
        memory.write16(processor, base + static_cast<std::uint32_t>(i * 2), program[i]);
    Cpu cpu{processor};
    cpu.reset(base, true);
    cpu.set_register(13, stack_top);
    cpu.set_register(4, 0x12345678);
    steps(cpu, memory, 2);
    check(cpu.state().registers[14] == base + 6 && cpu.state().registers[15] == base + 4,
          "préfixe BL Thumb doit être un pas distinct");
    steps(cpu, memory, 16);
    const auto& s = cpu.state();
    check(s.registers[0] == 16 && s.registers[1] == 0x55 && s.registers[4] == 0x12345678,
          "appels Thumb imbriqués : résultat ou registre préservé");
    check(s.registers[13] == stack_top && s.registers[15] == base + 8 && s.thumb,
          "appels Thumb imbriqués : retour ou SP");
    check(memory.read32(processor, stack_top - 8) == 0x12345678 &&
          memory.read32(processor, stack_top - 4) == base + 7, "ordre PUSH r4,lr");
    check(memory.read32(processor, stack_top - 16) == 9 &&
          memory.read32(processor, stack_top - 12) == base + 0x1b, "pile du second appel");
}

void nested_arm_calls_test(Processor processor) {
    Memory memory;
    const std::array<std::uint32_t, 18> program{
        0xe3a00001, // +00 mov r0,#1
        0xeb000003, // +04 bl outer (+0x18)
        0xe3a01055, // +08 mov r1,#0x55
        0xeafffffe, // +0c b .
        0xe1a00000, 0xe1a00000,
        0xe92d4010, // +18 outer: stmdb sp!,{r4,lr}
        0xe3a04009, // +1c mov r4,#9
        0xe2800002, // +20 add r0,r0,#2
        0xeb000001, // +24 bl inner (+0x30)
        0xe0800004, // +28 add r0,r0,r4
        0xe8bd8010, // +2c ldmia sp!,{r4,pc}
        0xe92d4010, // +30 inner: stmdb sp!,{r4,lr}
        0xe3a04063, // +34 mov r4,#99
        0xe24dd004, // +38 sub sp,sp,#4
        0xe2800004, // +3c add r0,r0,#4
        0xe28dd004, // +40 add sp,sp,#4
        0xe8bd8010, // +44 ldmia sp!,{r4,pc}
    };
    for (std::size_t i = 0; i < program.size(); ++i)
        memory.write32(processor, base + static_cast<std::uint32_t>(i * 4), program[i]);
    Cpu cpu{processor};
    cpu.reset(base, false);
    cpu.set_register(13, stack_top);
    cpu.set_register(4, 0x12345678);
    steps(cpu, memory, 16);
    const auto& s = cpu.state();
    check(s.registers[0] == 16 && s.registers[1] == 0x55 && s.registers[4] == 0x12345678,
          "appels ARM imbriqués : résultat ou registre préservé");
    check(s.registers[13] == stack_top && s.registers[15] == base + 12 && !s.thumb,
          "appels ARM imbriqués : retour ou SP");
}

void arm_addressing_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    // Transfert {r1,r3,r7}, toujours dans l'ordre croissant des registres.
    struct Mode { std::uint32_t bits, start, final_base; };
    const std::array<Mode, 4> modes{{
        {0x00800000, 0x02001020, 0x0200102c}, // IA
        {0x01800000, 0x02001024, 0x0200102c}, // IB
        {0, 0x02001018, 0x02001014},          // DA
        {0x01000000, 0x02001014, 0x02001014}, // DB
    }};
    for (const auto mode : modes) {
        cpu.reset(base, false);
        cpu.set_register(0, 0x02001020);
        cpu.set_register(1, 0x11111111);
        cpu.set_register(3, 0x33333333);
        cpu.set_register(7, 0x77777777);
        memory.write32(processor, base, 0xe820008aU | mode.bits); // STM{mode} r0!,{r1,r3,r7}
        cpu.step(memory);
        check(cpu.state().registers[0] == mode.final_base, "writeback STM IA/IB/DA/DB");
        check(memory.read32(processor, mode.start) == 0x11111111 &&
              memory.read32(processor, mode.start + 4) == 0x33333333 &&
              memory.read32(processor, mode.start + 8) == 0x77777777, "ordre STM IA/IB/DA/DB");
        cpu.reset(base, false);
        cpu.set_register(0, 0x02001020);
        memory.write32(processor, base, 0xe830008aU | mode.bits); // LDM{mode} r0!,{r1,r3,r7}
        cpu.step(memory);
        check(cpu.state().registers[0] == mode.final_base && cpu.state().registers[1] == 0x11111111 &&
              cpu.state().registers[3] == 0x33333333 && cpu.state().registers[7] == 0x77777777,
              "LDM IA/IB/DA/DB");
    }
    // Sans writeback, la base peut être stockée ou chargée comme tout registre.
    cpu.reset(base, false);
    cpu.set_register(0, 0x02001000);
    memory.write32(processor, base, 0xe8800001); // STMIA r0,{r0}
    cpu.step(memory);
    check(memory.read32(processor, 0x02001000) == 0x02001000, "STM sans writeback et base dans liste");
    memory.write32(processor, 0x02001000, 0xaabbccdd);
    memory.write32(processor, base + 4, 0xe8900001); // LDMIA r0,{r0}
    cpu.step(memory);
    check(cpu.state().registers[0] == 0xaabbccdd, "LDM sans writeback et base dans liste");
}

void thumb_multiple_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    cpu.reset(base, true);
    cpu.set_register(0, 0x02001000);
    cpu.set_register(1, 0x11111111);
    cpu.set_register(3, 0x33333333);
    memory.write16(processor, base, 0xc00a); // STMIA r0!,{r1,r3}
    memory.write16(processor, base + 2, 0xc80a); // LDMIA r0!,{r1,r3}
    cpu.step(memory);
    check(cpu.state().registers[0] == 0x02001008 && memory.read32(processor, 0x02001004) == 0x33333333,
          "STMIA Thumb : ordre ou writeback");
    cpu.set_register(0, 0x02001000);
    cpu.set_register(1, 0);
    cpu.set_register(3, 0);
    cpu.step(memory);
    check(cpu.state().registers[0] == 0x02001008 && cpu.state().registers[1] == 0x11111111 &&
          cpu.state().registers[3] == 0x33333333, "LDMIA Thumb : lecture ou writeback");
    memory.write32(processor, 0x02001000, 0xabcdef00);
    cpu.reset(base, true);
    cpu.set_register(0, 0x02001000);
    memory.write16(processor, base, 0xc809); // LDMIA r0!,{r0,r3} : pas de writeback
    cpu.step(memory);
    check(cpu.state().registers[0] == 0xabcdef00 && cpu.state().registers[3] == 0x33333333,
          "LDMIA Thumb doit préserver la base chargée");
    cpu.reset(base + 2, true);
    cpu.set_register(13, stack_top);
    memory.write16(processor, base + 2, 0xa101); // ADD r1,pc,#4 : PC+4 aligné
    memory.write16(processor, base + 4, 0xaa02); // ADD r2,sp,#8
    steps(cpu, memory, 2);
    check(cpu.state().registers[1] == base + 8 && cpu.state().registers[2] == stack_top + 8,
          "adressage Thumb PC/SP");
}

void load_pc_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    for (const auto raw : {0xe590f000U, 0xe8b08000U}) { // LDR pc,[r0] ; LDMIA r0!,{pc}
        for (const auto target : {base + 0x43, base + 0x42}) {
            cpu.reset(base, false);
            cpu.set_register(0, 0x02001000);
            memory.write32(processor, base, raw);
            memory.write32(processor, 0x02001000, target);
            cpu.step(memory);
            const bool thumb = processor == arm9 && (target & 1U) != 0;
            check(cpu.state().thumb == thumb && cpu.state().registers[15] == (target & (thumb ? ~1U : ~3U)),
                  "LDR/LDM pc : différence ARM7/ARM9 et alignement");
        }
    }
    for (const auto target : {base + 0x43, base + 0x42}) {
        cpu.reset(base, true);
        cpu.set_register(13, stack_top - 4);
        memory.write16(processor, base, 0xbd00); // POP {pc}
        memory.write32(processor, stack_top - 4, target);
        cpu.step(memory);
        const bool thumb = processor == arm7 || (target & 1U) != 0;
        check(cpu.state().thumb == thumb && cpu.state().registers[15] == (target & (thumb ? ~1U : ~3U)) &&
              cpu.state().registers[13] == stack_top, "POP pc : différence ARM7/ARM9");
    }
}

void blx_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    for (const auto thumb : {false, true}) {
        cpu.reset(base, thumb);
        cpu.set_register(14, base + 0x41);
        if (thumb) memory.write16(processor, base, 0x47f0); // BLX lr
        else memory.write32(processor, base, 0xe12fff3e); // BLX lr
        const auto before = cpu.state();
        if (processor == arm7) {
            expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "BLX registre accepté sur ARM7");
            check(cpu.state() == before, "BLX refusé a modifié ARM7");
        } else {
            cpu.step(memory);
            check(cpu.state().thumb && cpu.state().registers[15] == base + 0x40 &&
                  cpu.state().registers[14] == (thumb ? base + 3 : base + 4),
                  "BLX lr : lire la cible avant de remplacer LR");
        }
    }
    for (const auto raw : {0xfa000000U, 0xfb000000U, 0xfafffffcU}) {
        cpu.reset(base, false);
        memory.write32(processor, base, raw);
        const auto before = cpu.state();
        if (processor == arm7) {
            expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "BLX immédiat accepté sur ARM7");
            check(cpu.state() == before, "BLX immédiat refusé a modifié ARM7");
        } else {
            cpu.step(memory);
            const auto target = raw == 0xfa000000 ? base + 8 : raw == 0xfb000000 ? base + 10 : base - 8;
            check(cpu.state().thumb && cpu.state().registers[15] == target && cpu.state().registers[14] == base + 4,
                  "BLX ARM immédiat : bit H ou offset signé");
        }
    }
    cpu.reset(base, true);
    memory.write16(processor, base, 0xf000); // préfixe partagé BL/BLX
    memory.write16(processor, base + 2, 0xe802); // BLX +4, cible +8 alignée ARM
    cpu.step(memory);
    const auto prefix = cpu.state();
    if (processor == arm7) {
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "suffixe BLX accepté sur ARM7");
        check(cpu.state() == prefix, "échec suffixe BLX a annulé le préfixe terminé");
    } else {
        cpu.step(memory);
        check(!cpu.state().thumb && cpu.state().registers[15] == base + 8 && cpu.state().registers[14] == base + 5,
              "BLX Thumb immédiat : mode ARM ou LR");
    }
    // Appel BL Thumb à offset négatif : préfixe -4096, suffixe +4092.
    cpu.reset(base + 0x100, true);
    memory.write16(processor, base + 0x100, 0xf7ff);
    memory.write16(processor, base + 0x102, 0xfffe);
    steps(cpu, memory, 2);
    check(cpu.state().registers[15] == base + 0x100 && cpu.state().registers[14] == base + 0x105,
          "offset signé BL Thumb");
}

void interworking_calls_test() {
    Memory memory;
    Cpu cpu{arm9};
    // ARM -> fonction Thumb -> retour ARM par POP pc.
    memory.write32(arm9, base, 0xe59f0018); // ldr r0,[pc,#24]
    memory.write32(arm9, base + 4, 0xe12fff30); // blx r0
    memory.write32(arm9, base + 8, 0xe3a02055); // mov r2,#0x55
    memory.write32(arm9, base + 0x20, base + 0x31);
    memory.write16(arm9, base + 0x30, 0xb500); // push {lr}
    memory.write16(arm9, base + 0x32, 0x2107); // movs r1,#7
    memory.write16(arm9, base + 0x34, 0xbd00); // pop {pc}
    cpu.reset(base, false);
    cpu.set_register(13, stack_top);
    steps(cpu, memory, 6);
    check(!cpu.state().thumb && cpu.state().registers[15] == base + 12 &&
          cpu.state().registers[1] == 7 && cpu.state().registers[2] == 0x55 &&
          cpu.state().registers[13] == stack_top, "appel ARM -> Thumb -> ARM par POP pc");

    // Thumb -> fonction ARM -> retour Thumb par LDM pc.
    memory.write16(arm9, base + 0x100, 0x4803); // ldr r0,[pc,#12]
    memory.write16(arm9, base + 0x102, 0x4780); // blx r0
    memory.write16(arm9, base + 0x104, 0x2266); // movs r2,#0x66
    memory.write32(arm9, base + 0x110, base + 0x140);
    memory.write32(arm9, base + 0x140, 0xe92d4000); // stmdb sp!,{lr}
    memory.write32(arm9, base + 0x144, 0xe3a01009); // mov r1,#9
    memory.write32(arm9, base + 0x148, 0xe8bd8000); // ldmia sp!,{pc}
    cpu.reset(base + 0x100, true);
    cpu.set_register(13, stack_top);
    steps(cpu, memory, 6);
    check(cpu.state().thumb && cpu.state().registers[15] == base + 0x106 &&
          cpu.state().registers[1] == 9 && cpu.state().registers[2] == 0x66 &&
          cpu.state().registers[13] == stack_top, "appel Thumb -> ARM -> Thumb par LDM pc");
}

void transfer_fault_test(Processor processor) {
    Memory memory;
    memory.set_wram_control(3);
    // La fin de la fenêtre WRAM ARM7 précède les MMIO non implémentées.
    // Pour ARM9, c'est la fin de la RAM principale, avant une WRAM non allouée.
    const auto last = processor == arm9 ? 0x02fffffcU : 0x03fffffcU;
    memory.write32(processor, last, 0x11223344);
    Cpu cpu{processor};
    for (const auto load : {false, true}) {
        cpu.reset(base, false);
        cpu.set_register(0, last);
        cpu.set_register(1, 0xaaaaaaaa);
        cpu.set_register(2, 0xbbbbbbbb);
        memory.write32(processor, base, load ? 0xe8b00006 : 0xe8a00006); // LDM/STMIA r0!,{r1,r2}
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "transfert partiellement hors RAM accepté");
        check(cpu.state() == before && memory.read32(processor, last) == 0x11223344,
              "transfert refusé a modifié registres ou premier mot");
    }
    cpu.reset(base, true);
    cpu.set_register(13, last + 4);
    cpu.set_register(0, 0xaaaaaaaa);
    memory.write16(processor, base, 0xbd01); // POP {r0,pc} : deuxième lecture invalide
    cpu.set_register(13, last);
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "POP partiellement hors RAM accepté");
    check(cpu.state() == before, "POP fautif a modifié registre ou SP");
    cpu.reset(base, true);
    cpu.set_register(13, 0x02000004);
    memory.write16(processor, base, 0xb503); // PUSH {r0,r1,lr}, déborde sous RAM principale
    const auto before_push = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "PUSH sous la RAM accepté");
    check(cpu.state() == before_push, "PUSH fautif a modifié le CPU");
    for (const auto raw : {0xe8a00000U, 0xe8b00001U, 0xe8a08000U, 0xe8f00006U}) {
        cpu.reset(base, false);
        cpu.set_register(0, 0x02001000);
        memory.write32(processor, base, raw);
        const auto initial = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "variante multiple non implémentée acceptée");
        check(cpu.state() == initial, "variante multiple refusée a modifié le CPU");
    }
    // Alignement et débordement de l'API RAM groupée, sans écriture partielle.
    const std::array<std::uint32_t, 2> words{0xaabbccdd, 0x12345678};
    expect_failure<ravenemu::Error>([&] { memory.write_words(processor, last, words); }, "plage mixte acceptée");
    expect_failure<ravenemu::Error>([&] { memory.write_words(processor, 0xfffffffc, words); }, "plage 32 bits débordante");
    expect_failure<ravenemu::Error>([&] { memory.write_words(processor, base + 1, words); }, "mots non alignés acceptés");
    check(memory.read32(processor, last) == 0x11223344, "écriture groupée refusée a modifié le premier mot");
}

} // namespace

int main() {
    for (const auto processor : {arm9, arm7}) {
        nested_thumb_calls_test(processor);
        nested_arm_calls_test(processor);
        arm_addressing_test(processor);
        thumb_multiple_test(processor);
        load_pc_test(processor);
        blx_test(processor);
        transfer_fault_test(processor);
    }
    interworking_calls_test();
}
