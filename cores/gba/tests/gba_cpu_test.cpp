#include "machine/machine.hpp"

#include "check.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <vector>

/**
 * Comportements de l'ARM7TDMI et du bus que les suites de test publiques
 * (jsmolka/gba-tests, FuzzARM) ont révélés absents. Chaque programme est
 * écrit à la main en IWRAM (0x03000100, données en 0x03000000), puis exécuté
 * instruction par instruction jusqu'à sa boucle finale `b .`.
 */
namespace ravenemu::gba::testing {

using ravenemu::testing::check;

constexpr std::uint32_t code_address = 0x0300'0100U;
constexpr std::uint32_t data_address = 0x0300'0000U;

class Fixture {
public:
    explicit Fixture(std::string_view rom_marker = {}) : machine_(make_rom(rom_marker), std::nullopt, std::nullopt,
                                                                 [] { return std::int64_t{0}; }) {}

    void arm(std::initializer_list<std::uint32_t> code) {
        auto at = code_address;
        for (const auto word : code) { store32(at, word); at += 4; }
        machine_.cpu.reset(static_cast<std::int32_t>(code_address));
    }
    void thumb(std::initializer_list<std::uint16_t> code) {
        auto at = code_address;
        for (const auto half : code) { store16(at, half); at += 2; }
        machine_.cpu.reset(static_cast<std::int32_t>(code_address));
        machine_.cpu.state.thumb = true;
    }
    // Exécute jusqu'à `b .` : PC ne bouge plus après une instruction.
    void run() {
        for (int step = 0; step < 1000; ++step) {
            const auto pc = machine_.cpu.state.regs[15];
            machine_.cpu.step();
            if (machine_.cpu.state.regs[15] == pc) return;
        }
        throw std::runtime_error("programme de test sans boucle finale");
    }
    [[nodiscard]] std::uint32_t reg(int index) const {
        return static_cast<std::uint32_t>(machine_.cpu.state.regs[static_cast<std::size_t>(index)]);
    }
    [[nodiscard]] std::uint32_t word(std::uint32_t address) const {
        const auto at = address & 0x7fffU;
        const auto& ram = machine_.bus.iwram;
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) value |= std::uint32_t{ram[(at + index) & 0x7fffU]} << (8U * index);
        return value;
    }
    void store32(std::uint32_t address, std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index)
            machine_.bus.iwram[(address + index) & 0x7fffU] = static_cast<std::uint8_t>(value >> (8U * index));
    }
    void store16(std::uint32_t address, std::uint16_t value) {
        machine_.bus.iwram[address & 0x7fffU] = static_cast<std::uint8_t>(value);
        machine_.bus.iwram[(address + 1) & 0x7fffU] = static_cast<std::uint8_t>(value >> 8U);
    }
    [[nodiscard]] Bus& bus() noexcept { return machine_.bus; }
    [[nodiscard]] Cpu& cpu() noexcept { return machine_.cpu; }

private:
    static RomImage make_rom(std::string_view marker) {
        std::vector<std::uint8_t> rom(0x200);
        rom[0xb2] = 0x96;
        std::copy(marker.begin(), marker.end(), rom.begin() + 0x100);
        return std::make_shared<const std::vector<std::uint8_t>>(std::move(rom));
    }
    Machine machine_;
};

// CMP/CMN/TST/TEQ avec S et Rd = PC recopient SPSR dans CPSR.
void test_with_pc_destination_restores_spsr() {
    Fixture fixture;
    fixture.arm({
        0xe3a0'8020U,  // mov r8, #32            (r8 du mode système)
        0xe321'f0d1U,  // msr cpsr_c, #0xd1      (FIQ)
        0xe3a0'8040U,  // mov r8, #64            (r8 du mode FIQ)
        0xe361'f01fU,  // msr spsr_c, #0x1f      (SPSR = système)
        0xe15f'f000U,  // cmp pc, r0 avec Rd = PC
        0xe1a0'0008U,  // mov r0, r8
        0xe10f'1000U,  // mrs r1, cpsr
        0xeaff'fffeU,  // b .
    });
    fixture.run();
    check((fixture.reg(1) & 0x1fU) == 0x1fU, "CMP Rd=PC n'a pas restauré le mode depuis SPSR");
    check(fixture.reg(0) == 32, "CMP Rd=PC : r8 lu dans la mauvaise banque");
}

// STR/STM de PC stockent l'adresse de l'instruction + 12 en ARM.
void stored_pc_is_instruction_plus_twelve() {
    Fixture fixture;
    fixture.arm({
        0xe3a0'1403U,  // mov r1, #0x03000000
        0xe581'f000U,  // str pc, [r1]           (en 0x104)
        0xe591'0000U,  // ldr r0, [r1]
        0xeaff'fffeU,  // b .
    });
    fixture.run();
    check(fixture.reg(0) == code_address + 4 + 12, "STR PC ne stocke pas l'adresse + 12");
}

// Liste vide : PC seul est transféré et la base avance de 0x40.
void arm_empty_register_list() {
    Fixture store;
    store.arm({
        0xe3a0'1403U,  // mov r1, #0x03000000
        0xe8a1'0000U,  // stmia r1!, {}         (en 0x104)
        0xeaff'fffeU,  // b .
    });
    store.run();
    check(store.word(data_address) == code_address + 4 + 12, "STM vide : PC non stocké");
    check(store.reg(1) == data_address + 0x40, "STM vide : base non avancée de 0x40");

    Fixture load;
    load.store32(data_address, code_address + 0x20);
    load.arm({
        0xe3a0'1403U,  // mov r1, #0x03000000
        0xe8b1'0000U,  // ldmia r1!, {}         (charge PC)
        0xe3a0'5001U,  // mov r5, #1            (sauté)
        0xeaff'fffeU,  // b .
        0xeaff'fffeU, 0xeaff'fffeU, 0xeaff'fffeU, 0xeaff'fffeU,
        0xe3a0'6002U,  // mov r6, #2            (en 0x120)
        0xeaff'fffeU,  // b .
    });
    load.run();
    check(load.reg(6) == 2 && load.reg(5) == 0, "LDM vide : PC non chargé");
    check(load.reg(1) == data_address + 0x40, "LDM vide : base non avancée de 0x40");
}

// STM avec écriture différée : la base, si elle n'est pas le premier registre
// de la liste, est stockée avec sa nouvelle valeur.
void arm_store_multiple_with_base_in_list() {
    Fixture fixture;
    fixture.arm({
        0xe3a0'1403U,  // mov r1, #0x03000000
        0xe3a0'0005U,  // mov r0, #5
        0xe8a1'0003U,  // stmia r1!, {r0, r1}
        0xeaff'fffeU,  // b .
    });
    fixture.run();
    check(fixture.word(data_address) == 5, "STM : premier registre mal stocké");
    check(fixture.word(data_address + 4) == data_address + 8, "STM : base non première stockée sans sa nouvelle valeur");

    Fixture first;
    first.arm({
        0xe3a0'1403U,  // mov r1, #0x03000000
        0xe8a1'0006U,  // stmia r1!, {r1, r2}
        0xeaff'fffeU,  // b .
    });
    first.run();
    check(first.word(data_address) == data_address, "STM : base première stockée avec sa nouvelle valeur");
}

// Bit S sans PC : LDM charge les registres du mode utilisateur.
void arm_user_bank_transfer() {
    Fixture fixture;
    fixture.store32(data_address, 7);
    fixture.arm({
        0xe3a0'0403U,  // mov r0, #0x03000000
        0xe321'f0d1U,  // msr cpsr_c, #0xd1     (FIQ)
        0xe3a0'8009U,  // mov r8, #9            (r8 FIQ)
        0xe8d0'0100U,  // ldmia r0, {r8}^       (r8 utilisateur)
        0xe1a0'3008U,  // mov r3, r8
        0xe321'f0dfU,  // msr cpsr_c, #0xdf     (système)
        0xe1a0'4008U,  // mov r4, r8
        0xeaff'fffeU,  // b .
    });
    fixture.run();
    check(fixture.reg(3) == 9, "LDM^ a écrasé le r8 du mode FIQ");
    check(fixture.reg(4) == 7, "LDM^ n'a pas chargé le r8 utilisateur");
}

// Demi-mot non aligné : LDRH tourne de 8 bits, LDRSH charge l'octet signé.
void misaligned_halfword_loads() {
    Fixture arm;
    arm.store16(data_address, 0x8234);
    arm.arm({
        0xe3a0'1403U,  // mov r1, #0x03000000
        0xe1d1'20b1U,  // ldrh r2, [r1, #1]
        0xe1d1'30f1U,  // ldrsh r3, [r1, #1]
        0xeaff'fffeU,  // b .
    });
    arm.run();
    check(arm.reg(2) == 0x3400'0082U, "LDRH non aligné non tourné");
    check(arm.reg(3) == 0xffff'ff82U, "LDRSH non aligné : octet signé attendu");

    Fixture thumb;
    thumb.store16(data_address, 0x8234);
    thumb.thumb({
        0x2103,  // mov r1, #3
        0x0609,  // lsl r1, r1, #24
        0x2001,  // mov r0, #1
        0x5a0a,  // ldrh r2, [r1, r0]
        0x5e0b,  // ldsh r3, [r1, r0]
        0xe7fe,  // b .
    });
    thumb.run();
    check(thumb.reg(2) == 0x3400'0082U, "LDRH Thumb non aligné non tourné");
    check(thumb.reg(3) == 0xffff'ff82U, "LDSH Thumb non aligné : octet signé attendu");
}

// Thumb : liste vide (PC stocké à l'adresse + 6) et base dans la liste.
void thumb_block_transfers() {
    Fixture store;
    store.thumb({
        0x2103,  // mov r1, #3
        0x0609,  // lsl r1, r1, #24
        0xc100,  // stmia r1!, {}           (en 0x104)
        0xe7fe,  // b .
    });
    store.run();
    check(store.word(data_address) == code_address + 4 + 6, "STMIA Thumb vide : PC non stocké");
    check(store.reg(1) == data_address + 0x40, "STMIA Thumb vide : base non avancée de 0x40");

    Fixture load;
    load.store32(data_address, code_address + 0x10);
    load.thumb({
        0x2103,  // mov r1, #3
        0x0609,  // lsl r1, r1, #24
        0xc900,  // ldmia r1!, {}           (charge PC)
        0x2501,  // mov r5, #1              (sauté)
        0xe7fe, 0xe7fe, 0xe7fe, 0xe7fe,
        0x2602,  // mov r6, #2              (en 0x110)
        0xe7fe,  // b .
    });
    load.run();
    check(load.reg(6) == 2 && load.reg(5) == 0, "LDMIA Thumb vide : PC non chargé");

    Fixture base;
    base.thumb({
        0x2103,  // mov r1, #3
        0x0609,  // lsl r1, r1, #24
        0x2005,  // mov r0, #5
        0xc103,  // stmia r1!, {r0, r1}
        0xe7fe,  // b .
    });
    base.run();
    check(base.word(data_address + 4) == data_address + 8, "STMIA Thumb : base non première mal stockée");
}

// Pipeline : les deux instructions suivantes sont déjà lues, une écriture
// sur leur adresse ne change pas ce qui s'exécute.
void pipeline_ignores_overwritten_prefetch() {
    Fixture arm;
    arm.arm({
        0xe3a0'1000U,  // mov r1, #0
        0xe28f'0004U,  // add r0, pc, #4        (r0 = 0x110)
        0xe580'1000U,  // str r1, [r0]          (efface l'instruction en 0x110)
        0xe1a0'0000U,  // nop
        0xe3a0'3001U,  // mov r3, #1            (déjà préchargée)
        0xeaff'fffeU,  // b .
    });
    arm.run();
    check(arm.word(code_address + 0x10) == 0, "le programme n'a pas effacé l'instruction");
    check(arm.reg(3) == 1, "ARM : instruction préchargée relue après modification");

    Fixture thumb;
    thumb.thumb({
        0x2100,  // mov r1, #0
        0xa001,  // add r0, pc, #4          (r0 = 0x108)
        0x8001,  // strh r1, [r0]           (efface l'instruction en 0x108)
        0x46c0,  // nop
        0x2301,  // mov r3, #1              (déjà préchargée)
        0xe7fe,  // b .
    });
    thumb.run();
    check(thumb.reg(3) == 1, "Thumb : instruction préchargée relue après modification");
}

// Après un SWI, le pipeline est rechargé comme au retour du vrai BIOS : le
// code que CpuSet vient d'écrire juste après le SWI s'exécute.
void pipeline_reloads_after_swi() {
    Fixture fixture;
    fixture.store32(data_address, 0xe3a0'3007U);  // mov r3, #7
    fixture.arm({
        0xe3a0'0403U,  // mov r0, #0x03000000   (source)
        0xe28f'1008U,  // add r1, pc, #8        (destination : 0x114)
        0xe3a0'2301U,  // mov r2, #0x04000000   (mots de 32 bits)
        0xe382'2001U,  // orr r2, r2, #1        (un mot)
        0xef0b'0000U,  // swi 0x0b (CpuSet)
        0xe3a0'3001U,  // mov r3, #1            (remplacée par CpuSet)
        0xeaff'fffeU,  // b .
    });
    fixture.run();
    check(fixture.reg(3) == 7, "le code écrit par un SWI n'a pas été rechargé");
}

// Hors du BIOS, le lire rend le dernier mot que le BIOS a préchargé.
void bios_reads_return_last_prefetched_word() {
    Fixture fixture;
    fixture.arm({
        0xe3a0'1000U,  // mov r1, #0
        0xe591'4000U,  // ldr r4, [r1]
        0xef08'0000U,  // swi 0x08 (Sqrt, écrit r0)
        0xe591'2000U,  // ldr r2, [r1]
        0xeaff'fffeU,  // b .
    });
    fixture.run();
    check(fixture.reg(4) == 0xe129'f000U, "lecture du BIOS après démarrage");
    check(fixture.reg(2) == 0xe3a0'2004U, "lecture du BIOS après un SWI");
}

// SRAM : bus de 8 bits, mémoire vierge à 0xFF.
void sram_eight_bit_bus() {
    Fixture fixture("SRAM_V");
    auto& bus = fixture.bus();
    check(bus.read16(i32(0x0e00'0000U)) == 0xffff, "SRAM vierge : 0xFF attendu");
    bus.write8(i32(0x0e00'0000U), 1);
    check(bus.read16(i32(0x0e00'0000U)) == 0x0101, "SRAM : lecture 16 bits non répétée");
    check(u32(bus.read32(i32(0x0e00'0000U))) == 0x0101'0101U, "SRAM : lecture 32 bits non répétée");
    bus.write16(i32(0x0e00'0011U), 0xaabb);
    check(bus.read8(i32(0x0e00'0011U)) == 0xaa && bus.read8(i32(0x0e00'0010U)) == 0xff,
          "SRAM : écriture 16 bits mal placée");
    bus.write32(i32(0x0e00'0023U), i32(0xaabb'ccddU));
    check(bus.read8(i32(0x0e00'0023U)) == 0xaa && bus.read8(i32(0x0e00'0020U)) == 0xff,
          "SRAM : écriture 32 bits mal placée");

    Fixture none;
    check(none.bus().read8(i32(0x0e00'0000U)) == 0xff, "sans sauvegarde : 0xFF attendu");
}

// Au-delà de la ROM, le bus de la cartouche rend l'adresse du demi-mot.
void rom_reads_past_end_return_address() {
    Fixture fixture;
    auto& bus = fixture.bus();
    check(bus.read16(i32(0x0800'1000U)) == 0x0800, "lecture 16 bits hors ROM");
    check(bus.read8(i32(0x0800'1001U)) == 0x08, "lecture 8 bits hors ROM");
}

} // namespace ravenemu::gba::testing

int main() {
    using namespace ravenemu::gba::testing;
    test_with_pc_destination_restores_spsr();
    stored_pc_is_instruction_plus_twelve();
    arm_empty_register_list();
    arm_store_multiple_with_base_in_list();
    arm_user_bank_transfer();
    misaligned_halfword_loads();
    thumb_block_transfers();
    pipeline_ignores_overwritten_prefetch();
    pipeline_reloads_after_swi();
    bios_reads_return_last_prefetched_word();
    sram_eight_bit_bus();
    rom_reads_past_end_return_address();
    return 0;
}
