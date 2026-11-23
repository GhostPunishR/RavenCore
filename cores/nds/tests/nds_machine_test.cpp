#include "machine/machine.hpp"

#include "check.hpp"

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

/**
 * Machine DS complète, démarrée directement sur une ROM écrite pour ce test
 * (programmes assemblés par llvm-mc, source en commentaire) :
 * - l'ARM9 envoie 41 à l'ARM7 par la FIFO d'IPC, et l'ARM7 renvoie 42 ;
 * - l'ARM9 divise 100 par 7 avec le diviseur matériel ;
 * - l'ARM7 appelle Div du BIOS (SWI 09h) : -100 / 7 ;
 * - l'ARM9 installe son gestionnaire d'IRQ à DTCM + 0x3FFC, permet l'IRQ
 *   de V-Blank, puis compte les images par VBlankIntrWait (SWI 05h).
 * Résultats en RAM principale à 0x02200000.
 */
namespace {

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

constexpr std::uint32_t arm9_address = 0x0200'0000U;
constexpr std::uint32_t arm7_address = 0x037f'8000U;
constexpr std::uint32_t results = 0x0220'0000U;

const std::initializer_list<std::uint32_t> arm9_program{
    0xe3a02301U,  // mov r2, #0x04000000
    0xe3a03902U,  // mov r3, #0x8000
    0xe3833008U,  // orr r3, r3, #0x8
    0xe5823184U,  // str r3, [r2, #0x184]       ; FIFO activée et vidée
    0xe3a03029U,  // mov r3, #41
    0xe5823188U,  // str r3, [r2, #0x188]
    // .Lwait_reply:
    0xe5923184U,  // ldr r3, [r2, #0x184]
    0xe3130c01U,  // tst r3, #0x100             ; réception vide ?
    0x1afffffcU,  // bne .Lwait_reply
    0xe3a08641U,  // mov r8, #0x04100000
    0xe5989000U,  // ldr r9, [r8]
    0xe3a03000U,  // mov r3, #0
    0xe5823280U,  // str r3, [r2, #0x280]       ; DIVCNT : 32/32
    0xe3a03064U,  // mov r3, #100
    0xe5823290U,  // str r3, [r2, #0x290]
    0xe3a03007U,  // mov r3, #7
    0xe5823298U,  // str r3, [r2, #0x298]
    // .Lwait_div:
    0xe5923280U,  // ldr r3, [r2, #0x280]
    0xe3130902U,  // tst r3, #0x8000            ; occupé ?
    0x1afffffcU,  // bne .Lwait_div
    0xe592a2a0U,  // ldr r10, [r2, #0x2a0]
    0xe592b2a8U,  // ldr r11, [r2, #0x2a8]
    0xe3a07622U,  // mov r7, #0x02200000
    0xe8870e00U,  // stmia r7, {r9, r10, r11}
    0xe287700cU,  // add r7, r7, #12
    0xe3a06000U,  // mov r6, #0
    0xe3a0079fU,  // mov r0, #0x027c0000
    0xe3800c3fU,  // orr r0, r0, #0x3f00
    0xe38000fcU,  // orr r0, r0, #0xfc
    0xe28f1024U,  // adr r1, .Lirq
    0xe5801000U,  // str r1, [r0]               ; [DTCM + 0x3FFC]
    0xe3a03008U,  // mov r3, #8
    0xe1c230b4U,  // strh r3, [r2, #4]          ; DISPSTAT : IRQ de V-Blank
    0xe3a03001U,  // mov r3, #1
    0xe5823210U,  // str r3, [r2, #0x210]       ; IE
    0xe5823208U,  // str r3, [r2, #0x208]       ; IME
    // .Lloop:
    0xef050000U,  // swi #0x050000              ; VBlankIntrWait
    0xe2866001U,  // add r6, r6, #1
    0xe5876000U,  // str r6, [r7]
    0xeafffffbU,  // b .Lloop
    // .Lirq:
    0xe3a00301U,  // mov r0, #0x04000000
    0xe5901214U,  // ldr r1, [r0, #0x214]
    0xe5801214U,  // str r1, [r0, #0x214]       ; acquitte IF
    0xe3a0279fU,  // mov r2, #0x027c0000
    0xe3822c3fU,  // orr r2, r2, #0x3f00
    0xe38220f8U,  // orr r2, r2, #0xf8
    0xe5923000U,  // ldr r3, [r2]
    0xe1833001U,  // orr r3, r3, r1
    0xe5823000U,  // str r3, [r2]               ; drapeaux du BIOS à DTCM + 0x3FF8
    0xe12fff1eU,  // bx lr
};

const std::initializer_list<std::uint32_t> arm7_program{
    0xe3a02301U,  // mov r2, #0x04000000
    0xe3a03902U,  // mov r3, #0x8000
    0xe3833008U,  // orr r3, r3, #0x8
    0xe5823184U,  // str r3, [r2, #0x184]
    // .Lwait:
    0xe5923184U,  // ldr r3, [r2, #0x184]
    0xe3130c01U,  // tst r3, #0x100
    0x1afffffcU,  // bne .Lwait
    0xe3a04641U,  // mov r4, #0x04100000
    0xe5945000U,  // ldr r5, [r4]
    0xe2855001U,  // add r5, r5, #1
    0xe5825188U,  // str r5, [r2, #0x188]
    0xe3e00063U,  // mvn r0, #99
    0xe3a01007U,  // mov r1, #7
    0xef090000U,  // swi #0x090000              ; Div
    0xe3a06622U,  // mov r6, #0x02200000
    0xe2866010U,  // add r6, r6, #0x10
    0xe886000bU,  // stmia r6, {r0, r1, r3}
    0xeafffffeU,  // b .
};

void put32(std::vector<std::uint8_t>& rom, std::size_t at, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) rom[at + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
}

std::vector<std::uint8_t> test_rom(std::string_view game_code = "####",
                                   std::initializer_list<std::uint32_t> arm9 = arm9_program,
                                   std::initializer_list<std::uint32_t> arm7 = arm7_program) {
    std::vector<std::uint8_t> rom(0x800);
    const std::string_view title{"RAVENTEST"};
    for (std::size_t index = 0; index < title.size(); ++index) rom[index] = static_cast<std::uint8_t>(title[index]);
    for (std::size_t index = 0; index < game_code.size(); ++index)
        rom[0x0c + index] = static_cast<std::uint8_t>(game_code[index]);
    const auto place = [&](std::size_t header, std::size_t offset, std::uint32_t address,
                           std::initializer_list<std::uint32_t> code) {
        put32(rom, header, static_cast<std::uint32_t>(offset));
        put32(rom, header + 4, address);
        put32(rom, header + 8, address);
        put32(rom, header + 12, static_cast<std::uint32_t>(code.size() * 4));
        for (const auto word : code) {
            put32(rom, offset, word);
            offset += 4;
        }
    };
    place(0x20, 0x200, arm9_address, arm9);
    place(0x30, 0x400, arm7_address, arm7);
    const auto crc = crc16(std::span<const std::uint8_t>{rom}.first(0x15e));
    rom[0x15e] = static_cast<std::uint8_t>(crc);
    rom[0x15f] = static_cast<std::uint8_t>(crc >> 8U);
    return rom;
}

std::uint32_t main_word(Machine& machine, std::uint32_t address) { return load(machine.hw.memory.main(address), 4); }

void direct_boot_test() {
    Machine machine{test_rom()};
    auto& memory = machine.hw.memory;
    check(load(memory.main(arm9_address), 4) == 0xe3a0'2301U, "binaire ARM9 en RAM principale");
    check(load(memory.wram7(arm7_address), 4) == 0xe3a0'2301U && memory.wram_control == 3,
          "binaire ARM7 en WRAM, les 32 Kio partagés à l'ARM7");
    check(main_word(machine, 0x027f'fe00U) == 0x4556'4152U && main_word(machine, 0x027f'fe28U) == arm9_address,
          "en-tête recopié à 0x027FFE00");
    check(main_word(machine, 0x027f'f800U) == 0xc2U && (main_word(machine, 0x027f'fc40U) & 0xffffU) == 1,
          "identifiant de puce et indicateur de démarrage");
    check(machine.arm9.pc() == arm9_address && machine.arm7.pc() == arm7_address, "points d'entrée");
    check(machine.arm9.vector_base == 0xffff'0000U && machine.hw.cp15.dtcm_base() == 0x027c'0000U,
          "CP15 : vecteurs hauts, DTCM à 0x027C0000");
    check((main_word(machine, 0x027f'fc80U) & 0xffffU) == 5 && (main_word(machine, 0x027f'fc86U) & 0xffffU) == u'R' &&
              (main_word(machine, 0x027f'fc9aU) & 0xffffU) == 8,
          "réglages utilisateur générés : version 5, pseudo de 8 lettres");
}

// L'ARM9 donne la banque A au LCDC, la remplit de rouge et l'affiche (mode 2).
void video_test() {
    const std::initializer_list<std::uint32_t> program{
        0xe3a00301U,  // mov r0, #0x04000000
        0xe3a01080U,  // mov r1, #0x80
        0xe5c01240U,  // strb r1, [r0, #0x240]     ; VRAMCNT_A : LCDC
        0xe3a01802U,  // mov r1, #0x00020000
        0xe5801000U,  // str r1, [r0]              ; DISPCNT : affichage de la VRAM A
        0xe3a0251aU,  // mov r2, #0x06800000
        0xe3a0301fU,  // mov r3, #0x1f
        0xe1833803U,  // orr r3, r3, r3, lsl #16
        0xe3a04906U,  // mov r4, #0x18000
        // .Lfill:
        0xe4823004U,  // str r3, [r2], #4
        0xe2544004U,  // subs r4, r4, #4
        0x1afffffcU,  // bne .Lfill
        0xeafffffeU,  // b .
    };
    Machine machine{test_rom("####", program, {0xeafffffeU})};
    machine.run_frame();
    machine.run_frame();
    const auto& frame = machine.frame();
    check(frame.size() == 2 * Hardware::screen_pixels, "deux écrans de 256 × 192");
    check(frame[Hardware::screen_pixels] == 0xffff'0000U && frame.back() == 0xffff'0000U,
          "moteur A sur l'écran du bas : la VRAM rouge");
    check(frame[0] == 0xffff'ffffU, "moteur B éteint : écran du haut blanc");
}

void two_processors_test() {
    Machine machine{test_rom()};
    for (int frame = 0; frame < 4; ++frame) machine.run_frame();
    check(main_word(machine, results) == 42, "IPC : l'ARM7 renvoie la valeur de l'ARM9 augmentée de 1");
    check(main_word(machine, results + 4) == 14 && main_word(machine, results + 8) == 2, "diviseur matériel : 100 / 7");
    check(main_word(machine, results + 0x10) == static_cast<std::uint32_t>(-14) &&
              main_word(machine, results + 0x14) == static_cast<std::uint32_t>(-2) &&
              main_word(machine, results + 0x18) == 14,
          "Div du BIOS de l'ARM7 : quotient, reste, valeur absolue");
    const auto frames = main_word(machine, results + 0x0c);
    check(frames >= 2 && frames <= 4, "VBlankIntrWait : une image comptée par V-Blank");
    machine.run_frame();
    check(main_word(machine, results + 0x0c) == frames + 1, "une image de plus après un V-Blank de plus");
    check(machine.bios9.unsupported == 0 && machine.bios7.unsupported == 0, "aucune SWI inconnue");
    check(machine.hw.now >= 4 * LcdTiming::frame_cycles, "temps écoulé");
}

void refused_rom_test() {
    auto rom = test_rom("ABCE");
    rom.resize(0x8000);
    put32(rom, 0x20, 0x4000);  // ARM9 en zone sécurisée, au contenu chiffré
    rom[0x4000] = 0x12;
    const auto crc = crc16(std::span<const std::uint8_t>{rom}.first(0x15e));
    rom[0x15e] = static_cast<std::uint8_t>(crc);
    rom[0x15f] = static_cast<std::uint8_t>(crc >> 8U);
    expect_failure<CartridgeError>([&] { Machine machine{rom}; }, "zone sécurisée chiffrée acceptée");
}

} // namespace

int main() {
    direct_boot_test();
    video_test();
    two_processors_test();
    refused_rom_test();
    return 0;
}
