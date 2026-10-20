#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <algorithm>
#include <array>
#include <vector>

using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::nds::VideoFrame;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t lcdc = 0x06800000;
constexpr std::uint32_t dispcnt = 0x04000000;
constexpr std::uint32_t power = 0x04000304;
constexpr std::uint32_t dma0 = 0x040000b0;
constexpr std::array<std::uint32_t, 9> starts{0, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xa0000};
constexpr std::array<std::uint32_t, 9> sizes{0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
constexpr std::array<std::uint32_t, 9> controls{0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244,
                                           0x04000245, 0x04000246, 0x04000248, 0x04000249};

namespace {

bool white(const auto& pixels) { return std::all_of(pixels.begin(), pixels.end(), [](auto pixel) { return pixel == 0x7fff; }); }

void lcdc_test() {
    constexpr std::array<unsigned, 9> masks{0x9b, 0x9b, 0x9f, 0x9f, 0x87, 0x9f, 0x9f, 0x83, 0x83};
    for (unsigned bank = 0; bank < 9; ++bank) {
        Memory memory;
        const auto base = lcdc + starts[bank];
        check(memory.read8(arm9, controls[bank]) == 0, "VRAMCNT initial");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(arm9, base)); }, "banque désactivée accessible");
        memory.write8(arm9, controls[bank], 0xff);
        check(memory.read8(arm9, controls[bank]) == masks[bank], "masque VRAMCNT propre à la banque");
        memory.write8(arm9, controls[bank], 0x80);
        memory.write32(arm9, base, 0x12345678);
        memory.write32(arm9, base + sizes[bank] - 4, 0xaabbccdd);
        check(memory.read8(arm9, base) == 0x78 && memory.read16(arm9, base + 2) == 0x1234 &&
              memory.read32(arm9, base + sizes[bank] - 4) == 0xaabbccdd, "taille et ordre little-endian de la banque VRAM");
        memory.write8(arm9, base, 0);
        check(memory.read32(arm9, base) == 0x12345678, "STRB ARM9 vers VRAM doit être ignoré");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(arm7, base)); }, "ARM7 accède à LCDC ARM9");
        memory.write8(arm9, controls[bank], 0);
        expect_failure<ravenemu::Error>([&] { memory.write8(arm9, base, 0); }, "VRAM désactivée n'est pas diagnostiquée");
        memory.write8(arm9, controls[bank], 0x80);
        check(memory.read32(arm9, base) == 0x12345678, "désactivation VRAM efface les octets");
    }
    Memory memory;
    memory.write32(arm9, controls[0], 0x98908880);
    check(memory.read32(arm9, controls[0]) == 0x98908880, "voies VRAMCNT A-D");
    memory.write32(arm9, controls[4], 0x03808080); // E/F/G et WRAMCNT
    memory.write16(arm9, controls[7], 0x8080);
    check(memory.wram_control() == 3 && memory.read8(arm7, 0x04000241) == 3 &&
          memory.read8(arm9, controls[7]) == 0x80 && memory.read8(arm9, controls[8]) == 0x80, "VRAMCNT et WRAMCNT ne doivent pas être confondus");
    expect_failure<ravenemu::Error>([&] { memory.write32(arm9, controls[7], 0); }, "mot franchissant VRAMCNT H/I accepté");
    check(memory.read16(arm9, controls[7]) == 0x8080, "écriture refusée a effacé VRAMCNT H/I");
}

void mapping_case(unsigned bank, std::uint8_t control, std::uint32_t base) {
    Memory memory;
    memory.write8(arm9, controls[bank], control);
    memory.write32(arm9, base + 8, 0x10203040 + bank);
    memory.write16(arm9, base + sizes[bank] - 2, 0xabcd);
    check(memory.read32(arm9, base + 8) == 0x10203040 + bank, "lecture dans la fenêtre BG/OBJ");
    memory.write8(arm9, controls[bank], 0x80);
    check(memory.read32(arm9, lcdc + starts[bank] + 8) == 0x10203040 + bank &&
          memory.read16(arm9, lcdc + starts[bank] + sizes[bank] - 2) == 0xabcd, "réaffectation BG/OBJ ne retrouve pas les octets physiques");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm9, base + 8)); }, "ancien alias BG/OBJ reste actif");
}

void mapping_test() {
    for (unsigned bank = 0; bank < 4; ++bank)
        for (unsigned offset = 0; offset < 4; ++offset)
            mapping_case(bank, static_cast<std::uint8_t>(0x81 | (offset << 3U)), 0x06000000 + offset * 0x20000);
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned offset = 0; offset < 2; ++offset)
            mapping_case(bank, static_cast<std::uint8_t>(0x82 | (offset << 3U)), 0x06400000 + offset * 0x20000);
    mapping_case(4, 0x81, 0x06000000); mapping_case(4, 0x82, 0x06400000);
    constexpr std::array<unsigned, 4> small_offsets{0, 0x4000, 0x10000, 0x14000};
    for (unsigned bank = 5; bank <= 6; ++bank) for (unsigned offset = 0; offset < 4; ++offset) {
        mapping_case(bank, static_cast<std::uint8_t>(0x81 | (offset << 3U)), 0x06000000 + small_offsets[offset]);
        mapping_case(bank, static_cast<std::uint8_t>(0x82 | (offset << 3U)), 0x06400000 + small_offsets[offset]);
    }
    mapping_case(2, 0x84, 0x06200000); mapping_case(3, 0x84, 0x06600000);
    mapping_case(7, 0x81, 0x06200000); mapping_case(8, 0x81, 0x06208000); mapping_case(8, 0x82, 0x06600000);
    for (unsigned bank = 0; bank < 9; ++bank) {
        Memory memory;
        memory.write8(arm9, controls[bank], 0x80);
        memory.write16(arm9, lcdc + starts[bank], 0x1357);
        // Texture sur A-G, palette étendue sur H/I : aucune fenêtre CPU.
        memory.write8(arm9, controls[bank], static_cast<std::uint8_t>(bank == 7 ? 0x82 : 0x83));
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(arm9, lcdc + starts[bank])); },
                                       "texture/palette conserve un alias LCDC");
        memory.write8(arm9, controls[bank], 0x80);
        check(memory.read16(arm9, lcdc + starts[bank]) == 0x1357, "affectation hors CPU a effacé la banque");
    }
}

void arm7_and_overlap_test() {
    Memory memory;
    memory.write8(arm9, controls[2], 0x80); memory.write8(arm9, controls[3], 0x80);
    memory.write32(arm9, lcdc + starts[2], 0x11223344);
    memory.write32(arm9, lcdc + starts[3], 0xaabbccdd);
    memory.write8(arm9, controls[2], 0x82); memory.write8(arm9, controls[3], 0x8a);
    check(memory.read8(arm7, 0x04000240) == 3 && memory.read32(arm7, 0x06000000) == 0x11223344 &&
          memory.read32(arm7, 0x06020000) == 0xaabbccdd, "allocation C/D aux deux fenêtres ARM7");
    memory.write8(arm7, 0x04000240, 0);
    memory.write8(arm7, 0x06000001, 0x99);
    check(memory.read8(arm7, 0x04000240) == 3 && memory.read32(arm7, 0x06000000) == 0x11229944,
          "VRAMSTAT doit être en lecture seule, STRB ARM7 doit fonctionner");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm9, 0x06000000)); }, "ARM9 conserve la banque ARM7");
    memory.write8(arm9, controls[2], 0x81);
    check(memory.read8(arm7, 0x04000240) == 2 && memory.read32(arm9, 0x06000000) == 0x11229944, "retour de C à ARM9 perd les écritures ARM7");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm7, 0x06000000)); }, "ancien propriétaire ARM7 peut toujours lire C");
    memory.write8(arm9, controls[2], 0x92); // offset 2 réservé, mais VRAMSTAT ignore l'offset
    check(memory.read8(arm7, 0x04000240) == 3, "VRAMSTAT ne doit pas dépendre d'OFS");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(arm7, 0x06000000)); }, "offset ARM7 réservé accepté");
    memory.clear_ram();
    for (unsigned bank = 0; bank < 2; ++bank) memory.write8(arm9, controls[bank], 0x80);
    memory.write32(arm9, lcdc, 0x00ff00ff); memory.write32(arm9, lcdc + starts[1], 0xff00ff00);
    for (unsigned bank = 0; bank < 2; ++bank) memory.write8(arm9, controls[bank], 0x81);
    check(memory.read32(arm9, 0x06000000) == 0xffffffff, "banques superposées : lecture par OU des octets");
    memory.write16(arm9, 0x06000000, 0x5a5a);
    memory.write8(arm9, 0x06000000, 0);
    for (unsigned bank = 0; bank < 2; ++bank) memory.write8(arm9, controls[bank], 0x80);
    check(memory.read16(arm9, lcdc) == 0x5a5a && memory.read16(arm9, lcdc + starts[1]) == 0x5a5a,
          "écriture VRAM doit toucher toutes les banques superposées, sauf STRB ARM9");
}

void dma_and_fault_test(Processor cpu) {
    Memory memory;
    const auto target = cpu == arm9 ? lcdc : 0x06000000U;
    const auto bank = cpu == arm9 ? 0U : 2U;
    memory.write8(arm9, controls[bank], static_cast<std::uint8_t>(cpu == arm9 ? 0x80 : 0x82));
    memory.write32(cpu, 0x02001000, 0x12345678); memory.write32(cpu, 0x02001004, 0xaabbccdd);
    const std::array<std::uint32_t, 3> upload{0x02001000, target, 0xc4000002};
    memory.write_words(cpu, dma0, upload);
    memory.step_dma(cpu); memory.step_dma(cpu);
    check(memory.read32(cpu, target) == 0x12345678 && memory.read32(cpu, target + 4) == 0xaabbccdd &&
          memory.read32(cpu, 0x04000214) == 0x100, "DMA RAM vers VRAM, ARM9 ou ARM7");
    const std::array<std::uint32_t, 3> download{target, 0x02002000, 0x80000004}; // quatre demi-mots
    memory.write_words(cpu, dma0, download);
    for (unsigned i = 0; i < 4; ++i) memory.step_dma(cpu);
    check(memory.read32(cpu, 0x02002000) == 0x12345678 && memory.read32(cpu, 0x02002004) == 0xaabbccdd,
          "DMA VRAM vers RAM 16 bits");
    const auto last = target + sizes[bank] - 4;
    const std::array<std::uint32_t, 2> invalid{0xdeadbeef, 0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(cpu, last, invalid); }, "transfert CPU franchissant VRAM mappée accepté");
    check(memory.read32(cpu, last) == 0, "écriture multiple VRAM refusée a modifié le premier mot");
    const std::array<std::uint32_t, 3> bad_dma{0x02001000, last, 0xc4000002};
    memory.write32(cpu, 0x04000214, 0x100);
    memory.write_words(cpu, dma0, bad_dma);
    memory.step_dma(cpu);
    expect_failure<ravenemu::nds::DmaTransferError>([&] { memory.step_dma(cpu); }, "DMA franchit une banque non affectée");
    check(memory.read32(cpu, last) == 0x12345678 && memory.read32(cpu, 0x04000214) == 0,
          "DMA fautif a annulé une unité terminée ou demandé une fin prématurée");
}

void frame_test() {
    Memory memory;
    const auto initial = memory.render_frame();
    check(white(initial.upper) && white(initial.lower), "écrans initiaux éteints doivent être blancs dans ce modèle");
    memory.write32(arm9, power, 0xffffffff);
    check(memory.read32(arm9, power) == 0x820f, "masque POWCNT1");
    for (unsigned bank = 0; bank < 4; ++bank) {
        memory.write8(arm9, controls[bank], 0x80);
        memory.write32(arm9, lcdc + starts[bank], 0xfc00801f);
        memory.write16(arm9, lcdc + starts[bank] + (VideoFrame::width * VideoFrame::height - 1) * 2, 0x03e0);
        memory.write32(arm9, dispcnt, 0x00020000 | (bank << 18U));
        const auto upper = memory.render_frame();
        check(upper.upper[0] == 0x001f && upper.upper[1] == 0x7c00 && upper.upper.back() == 0x03e0 && white(upper.lower),
              "affichage RGB555, sélection A-D, bit15 ignoré et dernière ligne");
        memory.write16(arm9, power, 1); // A vers écran inférieur
        const auto lower = memory.render_frame();
        check(white(lower.upper) && lower.lower[0] == 0x001f && lower.lower.back() == 0x03e0, "permutation des deux écrans");
        memory.write8(arm9, controls[bank], 0x81);
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.render_frame()); }, "affichage VRAM depuis banque BG accepté");
        if (bank >= 2) {
            memory.write8(arm9, controls[bank], 0x84);
            expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.render_frame()); }, "mode 4 C/D confondu avec LCDC");
        }
        memory.write8(arm9, controls[bank], 0x80);
        memory.write16(arm9, power, 0x8001);
    }
    memory.write8(arm9, dispcnt, 0x80);
    const auto blank = memory.render_frame();
    check(white(blank.upper) && white(blank.lower), "forced blank doit produire du blanc");
    memory.write16(arm9, power, 0x8003);
    memory.write32(arm9, dispcnt, 0x00010007);
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.render_frame()); }, "mode BG réservé affiché silencieusement");
    memory.write32(arm9, dispcnt, 0x00030000);
    check(memory.render_frame().upper[0] == 0 && memory.render_frame().upper.back() == 0, "FIFO avant balayage doit conserver son image initiale noire");
    memory.write16(arm9, power, 0);
    check(white(memory.render_frame().upper), "LCD désactivé ne produit pas le blanc du modèle");
}

void copy_reset_test() {
    Memory memory;
    memory.write8(arm9, controls[0], 0x80);
    memory.write32(arm9, lcdc, 0x12345678);
    auto copy = memory;
    copy.write32(arm9, lcdc, 0xaabbccdd);
    copy.write8(arm9, controls[0], 0x81);
    check(memory.read32(arm9, lcdc) == 0x12345678 && memory.read8(arm9, controls[0]) == 0x80,
          "copie de bus partage VRAM ou affectation");
    memory.clear_ram();
    check(memory.read8(arm9, controls[0]) == 0 && memory.read32(arm9, dispcnt) == 0 && memory.read16(arm9, power) == 0,
          "reset des registres vidéo");
    memory.write8(arm9, controls[0], 0x80);
    check(memory.read32(arm9, lcdc) == 0, "reset conserve les octets VRAM");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

std::uint16_t pattern(unsigned x, unsigned y) {
    return static_cast<std::uint16_t>((x >> 3U) | ((y >> 3U) << 5U) | (((x + y) >> 4U) << 10U) | (((x ^ y) & 1U) << 15U));
}

void machine_program_test() {
    constexpr unsigned pixels = VideoFrame::width * VideoFrame::height;
    constexpr std::uint32_t program_size = 0x100 + pixels * 2;
    std::vector<std::uint8_t> rom(0x200 + program_size + 4);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, 0x02000000); put32(rom, 0x28, 0x02000000); put32(rom, 0x2c, program_size);
    put32(rom, 0x30, 0x200 + program_size); put32(rom, 0x34, 0x03800000); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 4);
    put32(rom, 0x200 + program_size, 0xeafffffe); // ARM7 B .
    put32(rom, 0x200, 0xe59f0078); // LDR r0,[pc,#120] -> +0x80, VRAMCNT_A
    put32(rom, 0x204, 0xe3a01080); // MOV r1,#80
    put32(rom, 0x208, 0xe5c01000); // STRB r1,[r0] : LCDC
    put32(rom, 0x20c, 0xe59f0070); // LDR r0,[pc,#112] -> +0x84, DISPCNT
    put32(rom, 0x210, 0xe59f1070); // LDR r1,[pc,#112] -> +0x88, mode2
    put32(rom, 0x214, 0xe5801000); // STR r1,[r0]
    put32(rom, 0x218, 0xe59f006c); // LDR r0,[pc,#108] -> +0x8c, POWCNT1
    put32(rom, 0x21c, 0xe59f106c); // LDR r1,[pc,#108] -> +0x90, 8001
    put32(rom, 0x220, 0xe1c010b0); // STRH r1,[r0]
    put32(rom, 0x224, 0xe59f0068); // LDR r0,[pc,#104] -> +0x94, DMA0
    put32(rom, 0x228, 0xe59f1068); // LDR r1,[pc,#104] -> +0x98, source
    put32(rom, 0x22c, 0xe5801000); // STR r1,[r0]
    put32(rom, 0x230, 0xe59f1064); // LDR r1,[pc,#100] -> +0x9c, destination
    put32(rom, 0x234, 0xe5801004); // STR r1,[r0,#4]
    put32(rom, 0x238, 0xe59f1064); // LDR r1,[pc,#100] -> +0xa4, contrôle
    put32(rom, 0x23c, 0xe5801008); // STR r1,[r0,#8] : démarrer la copie des pixels
    put32(rom, 0x240, 0xe2844001); // ADD r4,r4,#1 après DMA
    put32(rom, 0x244, 0xeafffffe); // B .
    put32(rom, 0x280, controls[0]); put32(rom, 0x284, dispcnt); put32(rom, 0x288, 0x00020000);
    put32(rom, 0x28c, power); put32(rom, 0x290, 0x8001); put32(rom, 0x294, dma0);
    put32(rom, 0x298, 0x02000100); put32(rom, 0x29c, lcdc); put32(rom, 0x2a4, 0x84000000 | (pixels / 2));
    for (unsigned y = 0; y < VideoFrame::height; ++y) for (unsigned x = 0; x < VideoFrame::width; x += 2)
        put32(rom, 0x300 + (y * VideoFrame::width + x) * 2, pattern(x, y) | (static_cast<std::uint32_t>(pattern(x + 1, y)) << 16U));
    ravenemu::nds::Machine machine;
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    machine.run_steps(arm9, 16);
    const auto cpu_before = machine.cpu_state(arm9);
    check(machine.memory().dma_pending(arm9), "programme ARM n'a pas lancé la copie vidéo");
    machine.run_steps(arm9, pixels / 2);
    check(machine.cpu_state(arm9) == cpu_before && !machine.memory().dma_pending(arm9), "copie vidéo : budget DMA ou suspension CPU");
    const auto frame = machine.render_frame();
    check(frame.upper.size() == pixels && frame.lower.size() == pixels && white(frame.lower), "dimensions et écran non piloté");
    for (unsigned y = 0; y < VideoFrame::height; ++y) for (unsigned x = 0; x < VideoFrame::width; ++x)
        check(frame.upper[y * VideoFrame::width + x] == (pattern(x, y) & 0x7fffU), "programme ARM/DMA : pixel RGB555 incorrect");
    machine.step(arm9);
    check(machine.cpu_state(arm9).registers[4] == 1, "programme ARM ne reprend pas après l'envoi vidéo");
    auto copy = machine;
    copy.memory().write16(arm9, lcdc, 0x7fff);
    check(copy.render_frame().upper[0] == 0x7fff && machine.render_frame().upper[0] == 0 && frame.upper[0] == 0,
          "copie de machine ou instantané partage son framebuffer");
    auto invalid = rom;
    put32(invalid, 0x34, 0x04000000); put32(invalid, 0x38, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(ravenemu::nds::Cartridge{invalid}); }, "chargement invalide accepté");
    check(machine.memory().read32(arm9, dispcnt) == 0x00020000 && machine.render_frame().upper[1] == (pattern(1, 0) & 0x7fffU),
          "chargement refusé perd l'image et les registres vidéo");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    check(white(machine.render_frame().upper) && machine.memory().read8(arm9, controls[0]) == 0, "chargement conserve l'ancienne image");
}

} // namespace

int main() {
    lcdc_test();
    mapping_test();
    arm7_and_overlap_test();
    for (const auto cpu : {arm9, arm7}) dma_and_fault_test(cpu);
    frame_test();
    copy_reset_test();
    machine_program_test();
}
