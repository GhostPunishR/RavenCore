#include "machine/machine.hpp"

#include "check.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

/**
 * Machine DS complète, démarrée directement sur une ROM écrite pour ce test
 * (programmes assemblés par llvm-mc, source en commentaire) :
 * - l'ARM9 envoie 41 à l'ARM7 par la FIFO d'IPC, et l'ARM7 renvoie 42 ;
 * - l'ARM9 divise 100 par 7 avec le diviseur matériel ;
 * - l'ARM7 appelle Div du BIOS (SWI 09h) : -100 / 7 ;
 * - l'ARM9 installe son gestionnaire d'IRQ à DTCM + 0x3FFC, permet l'IRQ
 *   de V-Blank, puis compte les images par VBlankIntrWait (SWI 05h) ;
 * - décompressions par fonctions de rappel (callback_decompression_test).
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

// DMA de l'ARM9, pilotée par ses registres depuis le bus : copie immédiate,
// remplissage par DMA0FILL, départ au V-Blank avec IRQ, TCM contournées.
void dma_test() {
    Machine machine{test_rom("####", {0xeafffffeU}, {0xeafffffeU})};
    auto& bus = machine.bus9;
    auto& memory = machine.hw.memory;
    for (std::uint32_t index = 0; index < 16; ++index) store(memory.main(0x0210'0000U + index * 4), index * 3, 4);
    bus.write32(0x0400'00d4U, 0x0210'0000U);
    bus.write32(0x0400'00d8U, 0x0211'0000U);
    bus.write32(0x0400'00dcU, 0x8400'0010U);  // activée, 32 bits, immédiate, 16 mots
    check(main_word(machine, 0x0211'003cU) == 45 && (bus.read32(0x0400'00dcU) & 0x8000'0000U) == 0,
          "DMA immédiate : copie, puis canal désactivé");
    bus.write32(0x0400'00e0U, 0xcafe'babeU);
    bus.write32(0x0400'00b0U, 0x0400'00e0U);
    bus.write32(0x0400'00b4U, 0x0212'0000U);
    bus.write32(0x0400'00b8U, 0x8500'0008U);  // source fixe
    check(main_word(machine, 0x0212'0000U) == 0xcafe'babeU && main_word(machine, 0x0212'001cU) == 0xcafe'babeU,
          "remplissage par DMA0FILL");
    bus.write32(0x0400'00bcU, 0x0210'0000U);
    bus.write32(0x0400'00c0U, 0x0213'0000U);
    bus.write32(0x0400'00c4U, 0xcc00'0004U);  // V-Blank, IRQ
    check(main_word(machine, 0x0213'0004U) == 0, "DMA de V-Blank : en attente");
    machine.run_frame();
    check(main_word(machine, 0x0213'0004U) == 3 && (machine.hw.irq9.flags() & (1U << (irq::dma0 + 1))) != 0,
          "DMA de V-Blank : copie et IRQ");
    bus.write32(0x0400'00d4U, 0x0210'0004U);  // le mot 3
    bus.write32(0x0400'00d8U, 0x027c'0000U);  // sous la DTCM
    bus.write32(0x0400'00dcU, 0x8400'0001U);
    check(load(memory.main(0x027c'0000U), 4) == 3 && load(machine.hw.cp15.dtcm(0x027c'0000U), 4) == 0 &&
              bus.peek(0x027c'0000U, 4) == 0,
          "la DMA écrit en RAM principale, pas dans la DTCM");
    auto& bus7 = machine.bus7;
    bus7.write32(0x0400'00d4U, 0x0210'0000U);
    bus7.write32(0x0400'00d8U, 0x0214'0000U);
    bus7.write32(0x0400'00dcU, 0x8400'0004U);
    check(main_word(machine, 0x0214'000cU) == 9, "DMA immédiate de l'ARM7");
}

// Programmes du test des décompressions par fonctions de rappel : ils
// remplissent la structure des fonctions, puis appellent les SWI.
const std::initializer_list<std::uint32_t> callback_arm9_program{
    0xe3a07622U,  // mov r7, #0x02200000
    0xe28f40d4U,  // adr r4, .Lcallbacks
    0xe28f00acU,  // adr r0, .Lopen
    0xe5840000U,  // str r0, [r4]
    0xe28f00c4U,  // adr r0, .Lget8
    0xe2800001U,  // add r0, r0, #1           ; Get_8bit en Thumb
    0xe5840008U,  // str r0, [r4, #8]
    0xe28f00a0U,  // adr r0, .Lget32
    0xe5840010U,  // str r0, [r4, #16]
    0xe3a00621U,  // mov r0, #0x02100000
    0xe3a01623U,  // mov r1, #0x02300000
    0xe1a03004U,  // mov r3, r4
    0xef120000U,  // swi #0x120000          ; LZ77 par fonctions de rappel
    0xe5870000U,  // str r0, [r7]
    0xe3a00621U,  // mov r0, #0x02100000
    0xe3800a01U,  // orr r0, r0, #0x1000
    0xe3a01623U,  // mov r1, #0x02300000
    0xe3811a01U,  // orr r1, r1, #0x1000
    0xe1a03004U,  // mov r3, r4
    0xef150000U,  // swi #0x150000          ; RL
    0xe5870004U,  // str r0, [r7, #4]
    0xe3a00621U,  // mov r0, #0x02100000
    0xe3800a02U,  // orr r0, r0, #0x2000
    0xe3a01623U,  // mov r1, #0x02300000
    0xe3811a02U,  // orr r1, r1, #0x2000
    0xe3a02785U,  // mov r2, #0x02140000
    0xe1a03004U,  // mov r3, r4
    0xef130000U,  // swi #0x130000          ; Huffman, arbre recopié à 0x02140000
    0xe5870008U,  // str r0, [r7, #8]
    0xe28f0050U,  // adr r0, .Lclose
    0xe5840004U,  // str r0, [r4, #4]         ; Close : -3
    0xe3a00621U,  // mov r0, #0x02100000
    0xe3800a01U,  // orr r0, r0, #0x1000
    0xe3a01623U,  // mov r1, #0x02300000
    0xe3811a03U,  // orr r1, r1, #0x3000
    0xe1a03004U,  // mov r3, r4
    0xef150000U,  // swi #0x150000
    0xe587000cU,  // str r0, [r7, #12]
    0xe28f0034U,  // adr r0, .Lrefuse
    0xe5840000U,  // str r0, [r4]              ; Open : -5
    0xe3a00621U,  // mov r0, #0x02100000
    0xe3a01623U,  // mov r1, #0x02300000
    0xe3811901U,  // orr r1, r1, #0x4000
    0xe1a03004U,  // mov r3, r4
    0xef120000U,  // swi #0x120000
    0xe5870010U,  // str r0, [r7, #16]
    0xeafffffeU,  // b .
    // .Lopen:
    0xe5900000U,  // ldr r0, [r0]
    0xe12fff1eU,  // bx lr
    // .Lget32:
    0xe5900000U,  // ldr r0, [r0]
    0xe12fff1eU,  // bx lr
    // .Lclose:
    0xe3e00002U,  // mvn r0, #2
    0xe12fff1eU,  // bx lr
    // .Lrefuse:
    0xe3e00004U,  // mvn r0, #4
    0xe12fff1eU,  // bx lr
    // .Lget8:
    0x47707800U,  // ldrb r0, [r0] ; bx lr      (Thumb)
    // .Lcallbacks:
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
};

const std::initializer_list<std::uint32_t> callback_arm7_program{
    0xe3a07622U,  // mov r7, #0x02200000
    0xe3877c01U,  // orr r7, r7, #0x100
    0xe28f4038U,  // adr r4, .Lcallbacks
    0xe28f0024U,  // adr r0, .Lopen
    0xe5840000U,  // str r0, [r4]
    0xe28f0024U,  // adr r0, .Lget8
    0xe5840008U,  // str r0, [r4, #8]
    0xe3a00621U,  // mov r0, #0x02100000
    0xe3a01623U,  // mov r1, #0x02300000
    0xe3811a05U,  // orr r1, r1, #0x5000
    0xe1a03004U,  // mov r3, r4
    0xef120000U,  // swi #0x120000          ; LZ77 sur l'ARM7 (ARMv4T)
    0xe5870000U,  // str r0, [r7]
    0xeafffffeU,  // b .
    // .Lopen:
    0xe5900000U,  // ldr r0, [r0]
    0xe12fff1eU,  // bx lr
    // .Lget8:
    0xe5d00000U,  // ldrb r0, [r0]
    0xe12fff1eU,  // bx lr
    // .Lcallbacks:
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
    0x00000000U,  // .word 0
};

// Décompressions par fonctions de rappel (SWI 12h, 13h, 15h), exécutées par
// le code ARM du BIOS de RavenCore. Les fonctions du jeu lisent la mémoire ;
// Get_8bit est en Thumb sur l'ARM9. Données compressées écrites à la main
// d'après GBATEK (« BIOS Decompression Functions »).
void callback_decompression_test() {
    Machine machine{test_rom("####", callback_arm9_program, callback_arm7_program)};
    const auto put = [&](std::uint32_t address, std::initializer_list<std::uint8_t> bytes) {
        for (const auto byte : bytes) *machine.hw.memory.main(address++) = byte;
    };
    // LZ77, 10 octets : « ABC », copie de 6 octets à 3 en arrière, « D ».
    put(0x0210'0000U, {0x10, 0x0a, 0x00, 0x00, 0x10, 'A', 'B', 'C', 0x30, 0x02, 'D'});
    // RL, 8 octets : 3 octets tels quels, puis « Q » répété 5 fois.
    put(0x0210'1000U, {0x30, 0x08, 0x00, 0x00, 0x02, 'x', 'y', 'z', 0x82, 'Q'});
    // Huffman sur 8 bits, 8 octets : racine (nœud 0 « A », nœud 1 vers « B » et
    // « C »), codes A = 0, B = 10, C = 11 ; « ABACABAA » = 01001101000.
    put(0x0210'2000U, {0x28, 0x08, 0x00, 0x00, 0x03, 0x80, 'A', 0xc0, 'B', 'C', 0x00, 0x00, 0x00, 0x00, 0x00, 0x4d});
    machine.run_frame();
    machine.run_frame();
    const auto text = [&](std::uint32_t address, std::size_t length) {
        return std::string{reinterpret_cast<const char*>(machine.hw.memory.main(address)), length};
    };
    const auto result = [&](unsigned index) { return main_word(machine, results + index * 4); };
    check(result(0) == 10 && text(0x0230'0000U, 10) == "ABCABCABCD", "LZ77 par fonctions de rappel, écrit par demi-mots");
    check(result(1) == 8 && text(0x0230'1000U, 8) == "xyzQQQQQ", "RL par fonctions de rappel");
    check(result(2) == 8 && text(0x0230'2000U, 8) == "ABACABAA", "Huffman par fonctions de rappel");
    check(*machine.hw.memory.main(0x0214'0000U) == 3 && *machine.hw.memory.main(0x0214'0003U) == 0xc0,
          "Huffman : arbre recopié dans le tampon de r2");
    check(result(3) == static_cast<std::uint32_t>(-3) && text(0x0230'3000U, 8) == "xyzQQQQQ",
          "Close rend son code d'erreur, après la décompression");
    check(result(4) == static_cast<std::uint32_t>(-5) && main_word(machine, 0x0230'4000U) == 0,
          "Open refuse : code d'erreur, rien d'écrit");
    check(main_word(machine, results + 0x100) == 10 && text(0x0230'5000U, 10) == "ABCABCABCD", "LZ77 sur l'ARM7");
    check(machine.bios9.unsupported == 0 && machine.bios7.unsupported == 0, "aucune SWI inconnue");
}

} // namespace

int main() {
    direct_boot_test();
    video_test();
    dma_test();
    two_processors_test();
    refused_rom_test();
    callback_decompression_test();
    return 0;
}
