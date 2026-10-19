#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <limits>
#include <vector>

using ravenemu::nds::CpuMode;
using ravenemu::nds::LcdTiming;
using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t status = 0x04000004;
constexpr std::uint32_t vcount = 0x04000006;
constexpr std::uint32_t flags = 0x04000214;
constexpr std::uint32_t dma0 = 0x040000b0;
constexpr std::uint32_t source = 0x02001000;
constexpr std::uint32_t destination = 0x02002000;
constexpr std::uint32_t enabled = 0x80000000;
constexpr std::uint32_t irq = 0x40000000;
constexpr std::uint32_t repeat = 0x02000000;
constexpr std::uint32_t word = 0x04000000;
constexpr std::uint64_t line_cycles = 2130;
constexpr std::uint64_t frame_cycles = line_cycles * 263;

namespace {

void configure(Memory& memory, Processor cpu, unsigned channel, std::uint32_t control) {
    const std::array<std::uint32_t, 3> registers{source, destination, control};
    memory.write_words(cpu, dma0 + channel * 12, registers);
}

std::uint32_t vblank_mode(Processor cpu) { return cpu == arm9 ? 0x08000000U : 0x10000000U; }

void registers_test() {
    Memory memory;
    check(memory.read32(arm9, status) == 4 && memory.read32(arm7, status) == 4, "LCD initial : ligne zéro et comparaison égale");
    memory.write32(arm9, status, 0xffffffff);
    check(memory.read32(arm9, status) == 0xffb8 && memory.read32(arm7, status) == 4,
          "bits DISPSTAT réservés, VCOUNT ignoré et contrôles propres à chaque CPU");
    memory.write8(arm9, status + 1, 1);
    check(memory.read16(arm9, status) == 0x01b8, "voie haute DISPSTAT");
    memory.write8(arm9, status, 0);
    memory.advance_cycles(line_cycles);
    check(memory.read32(arm9, status) == 0x00010104 && memory.read16(arm7, status) == 0,
          "comparaison LYC indépendante de l'autre CPU");
    memory.advance_cycles(255 * line_cycles);
    check(memory.read8(arm9, vcount) == 0 && memory.read8(arm7, vcount + 1) == 1 &&
          memory.read16(arm9, vcount) == 256, "neuvième bit VCOUNT et compteur partagé");
    memory.write16(arm7, vcount, 0);
    check(memory.read16(arm9, vcount) == 256, "écriture VCOUNT doit être ignorée dans ce modèle");
    memory.write16(arm7, status, 0x0080); // LYC=256 via DISPSTAT bit7
    check(memory.read16(arm7, status) == 0x0085, "LYC bit8 ou VBlank");
    const std::array<std::uint32_t, 2> invalid{0xffff, 0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm7, status, invalid); }, "transfert LCD puis trou accepté");
    check(memory.read16(arm7, status) == 0x0085, "transfert refusé modifie DISPSTAT");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm9, 0x04001004)); }, "faux DISPSTAT du moteur B accepté");
}

void boundaries_test() {
    Memory memory;
    memory.advance_cycles(1536);
    check(memory.read16(arm9, status) == 4 && memory.read16(arm7, status) == 4,
          "le drapeau HBlank ne doit pas suivre immédiatement la fin du tracé");
    memory.advance_cycles(69);
    check(memory.read16(arm9, status) == 4, "HBlank ARM9 avant 1606 cycles");
    memory.advance_cycles(1);
    check(memory.read16(arm9, status) == 6 && memory.read16(arm7, status) == 4, "décalage des drapeaux HBlank ARM9/ARM7");
    memory.advance_cycles(6);
    check(memory.read16(arm7, status) == 4, "HBlank ARM7 avant 1613 cycles");
    memory.advance_cycles(1);
    check(memory.read16(arm7, status) == 6, "HBlank ARM7 à 1613 cycles");
    memory.advance_cycles(line_cycles - 1613);
    check(memory.read16(arm9, vcount) == 1 && memory.read16(arm9, status) == 0 && memory.read16(arm7, status) == 0,
          "fin de ligne : HBlank et comparaison doivent être recalculés");
    memory.advance_cycles(191 * line_cycles - 1);
    check(memory.read16(arm9, vcount) == 191 && !(memory.read16(arm9, status) & 1U), "VBlank avant ligne 192");
    memory.advance_cycles(1);
    check(memory.read16(arm9, vcount) == 192 && memory.read16(arm7, status) == 1, "entrée VBlank ligne 192");
    memory.advance_cycles(69 * line_cycles);
    check(memory.read16(arm9, vcount) == 261 && memory.read16(arm9, status) == 1, "dernière ligne avec drapeau VBlank");
    memory.advance_cycles(line_cycles);
    check(memory.read16(arm9, vcount) == 262 && memory.read16(arm9, status) == 0, "VBlank doit retomber dès ligne 262");
    memory.advance_cycles(line_cycles);
    check(memory.read32(arm9, status) == 4, "nouvelle image à la ligne zéro");
}

void interrupts_test() {
    Memory memory;
    memory.write16(arm9, status, 0x06b8); // comparaison 262, trois IRQ LCD
    memory.write16(arm7, status, 0x0120); // comparaison 1 uniquement
    check(memory.read32(arm9, flags) == 0 && memory.read32(arm7, flags) == 0, "configuration LCD ne doit pas injecter de demande");
    memory.advance_cycles(1535);
    check(memory.read32(arm9, flags) == 0, "IRQ HBlank trop tôt");
    memory.advance_cycles(1);
    check(memory.read32(arm9, flags) == 2 && memory.read32(arm7, flags) == 0, "IRQ HBlank par CPU");
    memory.write32(arm9, flags, 2);
    memory.advance_cycles(1);
    check(memory.read32(arm9, flags) == 0, "acquitter IF pendant HBlank ne doit pas le réaffirmer");
    memory.advance_cycles(line_cycles - 1537);
    check(memory.read32(arm7, flags) == 4 && memory.read32(arm9, flags) == 0, "IRQ de comparaison à l'entrée de la ligne");
    memory.write32(arm7, flags, 4);
    memory.advance_cycles(191 * line_cycles);
    check(memory.read32(arm9, flags) == 3 && memory.read32(arm7, flags) == 0 && !memory.irq_pending(arm9),
          "IRQ VBlank et HBlank doivent rester dans IF avec IME/IE masqués");
    memory.write32(arm9, flags, 7);
    memory.advance_cycles(1536);
    check(memory.read32(arm9, flags) == 2, "IRQ HBlank doit aussi exister pendant VBlank");
    memory.advance_cycles(70 * line_cycles - 1536);
    check(memory.read32(arm9, flags) == 6 && !(memory.read16(arm9, status) & 1U), "IRQ de comparaison sur ligne 262");
    memory.advance_cycles(2 * line_cycles);
    check(memory.read32(arm7, flags) == 4, "comparaison doit se reproduire à l'image suivante");
    memory.write32(arm9, 0x04000210, 7);
    memory.write32(arm9, 0x04000208, 1);
    check(memory.irq_pending(arm9) && !memory.irq_pending(arm7), "ligne IRQ LCD et contrôleurs indépendants");
    memory.clear_ram();
    memory.advance_cycles(5 * line_cycles);
    memory.write16(arm9, status, 0x0520);
    check((memory.read16(arm9, status) & 4U) && memory.read32(arm9, flags) == 0,
          "écriture LYC : drapeau immédiat mais pas d'entrée de ligne artificielle");
    memory.advance_cycles(0);
    check(memory.read32(arm9, flags) == 0, "zéro cycle a créé une comparaison LCD");
    memory.advance_cycles(frame_cycles);
    check(memory.read32(arm9, flags) == 4, "comparaison LYC perdue sur image complète");
}

void batching_and_large_intervals_test() {
    Memory batched;
    batched.write16(arm9, status, 0x1138);
    batched.write16(arm7, status, 0x01b8); // comparaison 257
    auto split = batched;
    for (const auto cycles : {1U, 1535U, 70U, 7U, 517U, 100000U, 3U * 560190U + 17U}) {
        batched.advance_cycles(cycles);
        auto remaining = cycles;
        while (remaining != 0) {
            const auto part = remaining > 347 ? 347U : remaining;
            split.advance_cycles(part);
            remaining -= part;
        }
        for (const auto cpu : {arm9, arm7})
            check(batched.read32(cpu, status) == split.read32(cpu, status) && batched.read32(cpu, flags) == split.read32(cpu, flags),
                  "LCD dépend du découpage des cycles");
    }
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    batched.clear_ram();
    for (const auto cpu : {arm9, arm7}) batched.write16(cpu, status, 0x1138);
    batched.advance_cycles(maximum);
    auto phase = maximum % frame_cycles;
    check(batched.read16(arm9, vcount) == phase / line_cycles && batched.read32(arm9, flags) == 7 && batched.read32(arm7, flags) == 7,
          "grand intervalle LCD : phase ou IRQ");
    batched.advance_cycles(maximum);
    phase = (phase + maximum % frame_cycles) % frame_cycles;
    check(batched.read16(arm9, vcount) == phase / line_cycles && ((batched.read16(arm9, status) & 2U) != 0) == (phase % line_cycles >= 1606),
          "addition de phase LCD avec UINT64_MAX");
    LcdTiming timing;
    const auto event = timing.advance_cycles(192 * line_cycles);
    check(event.vblank && event.hblank && event.arm9 == 0 && event.arm7 == 0 && timing.vcount() == 192 && timing.cycle_in_line() == 0,
          "événements LCD indépendants des autorisations IRQ");
}

void vblank_dma_test(Processor cpu) {
    for (unsigned channel = 0; channel < 4; ++channel) {
        Memory memory;
        memory.write32(cpu, source, 0x12345678);
        configure(memory, cpu, channel, enabled | irq | word | vblank_mode(cpu) | 1);
        memory.advance_cycles(192 * line_cycles - 1);
        check(!memory.dma_pending(cpu), "DMA VBlank déclenché avant ligne 192");
        memory.advance_cycles(1);
        check(memory.dma_pending(cpu) && memory.read32(cpu, destination) == 0, "événement LCD doit armer sans exécuter le DMA");
        memory.step_dma(cpu);
        check(memory.read32(cpu, destination) == 0x12345678 && memory.read32(cpu, flags) == (1U << (8U + channel)) &&
              !(memory.read32(cpu, dma0 + channel * 12 + 8) & enabled), "DMA VBlank ponctuel ou IRQ de fin");
        memory.advance_cycles(frame_cycles);
        check(!memory.dma_pending(cpu), "DMA ponctuel s'est répété");
    }
    Memory memory;
    memory.write32(cpu, source, 11); memory.write32(cpu, source + 4, 22);
    configure(memory, cpu, 0, enabled | repeat | word | vblank_mode(cpu) | 1);
    memory.advance_cycles(192 * line_cycles);
    memory.step_dma(cpu);
    check(!memory.dma_pending(cpu) && (memory.read32(cpu, dma0 + 8) & enabled), "repeat doit rester armé entre deux VBlank");
    auto copy = memory;
    copy.advance_cycles(frame_cycles);
    copy.step_dma(cpu);
    check(copy.read32(cpu, destination + 4) == 22 && memory.read32(cpu, destination + 4) == 0,
          "copie du canal répété ou de l'horloge LCD partagée");
    memory.advance_cycles(frame_cycles);
    memory.step_dma(cpu);
    check(memory.read32(cpu, destination) == 11 && memory.read32(cpu, destination + 4) == 22,
          "DMA répété doit continuer source et destination incrémentées");
    memory.write32(cpu, dma0 + 8, 0);
    memory.advance_cycles(frame_cycles);
    check(!memory.dma_pending(cpu), "canal répété annulé réactivé");
}

void hblank_dma_test() {
    Memory memory;
    for (unsigned i = 0; i < 6; ++i) memory.write32(arm9, source + i * 4, 10 + i);
    const auto control = enabled | irq | word | repeat | 0x10000000U | (3U << 21U) | 2;
    configure(memory, arm9, 0, control); // HBlank, incrément/reload destination
    memory.advance_cycles(1535);
    check(!memory.dma_pending(arm9), "DMA HBlank trop tôt");
    memory.advance_cycles(1);
    memory.step_dma(arm9);
    memory.advance_cycles(line_cycles); // un second HBlank pendant le bloc : pas de file d'événements
    memory.step_dma(arm9);
    check(!memory.dma_pending(arm9) && memory.read32(arm9, destination) == 10 && memory.read32(arm9, destination + 4) == 11,
          "événement pendant un bloc a redémarré/dupliqué le DMA");
    memory.advance_cycles(0);
    check(!memory.dma_pending(arm9), "fin de bloc ne doit pas rejouer un HBlank passé");
    memory.advance_cycles(line_cycles);
    memory.step_dma(arm9); memory.step_dma(arm9);
    check(memory.read32(arm9, destination) == 12 && memory.read32(arm9, destination + 4) == 13,
          "destination non rechargée ou source réinitialisée en répétition");
    memory.write32(arm9, dma0 + 4, destination + 0x20);
    memory.write32(arm9, dma0 + 8, (control & ~0x1fffffU) | 1U);
    memory.advance_cycles(line_cycles);
    memory.step_dma(arm9);
    check(!memory.dma_pending(arm9) && memory.read32(arm9, destination + 0x20) == 14,
          "répétition ne recharge pas la nouvelle longueur et destination programmées");
    memory.clear_ram();
    memory.advance_cycles(192 * line_cycles);
    memory.write16(arm9, status, 0x10);
    configure(memory, arm9, 0, enabled | word | 0x10000000U | 1);
    configure(memory, arm7, 0, enabled | word | 0x20000000U | 1); // cartouche DS sur ARM7, pas HBlank
    memory.advance_cycles(1536);
    check(!memory.dma_pending(arm9) && !memory.dma_pending(arm7) && memory.read32(arm9, flags) == 2,
          "HBlank pendant VBlank : IRQ oui, DMA visible non ; mode ARM7 distinct");
    memory.advance_cycles(71 * line_cycles - 1536);
    check(!memory.dma_pending(arm9), "DMA HBlank sur dernière ligne cachée ou début de ligne zéro");
    memory.advance_cycles(1536);
    check(memory.dma_pending(arm9) && !memory.dma_pending(arm7), "reprise des DMA HBlank à la nouvelle image");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

void machine_program_test() {
    std::vector<std::uint8_t> rom(0x3e0);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, 0x02000000); put32(rom, 0x28, 0x02000000); put32(rom, 0x2c, 0xf0);
    put32(rom, 0x30, 0x2f0); put32(rom, 0x34, 0x03800000); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 0xf0);
    for (const auto offset : {0x200U, 0x2f0U}) {
        const auto cpu = offset == 0x200 ? arm9 : arm7;
        const auto base = cpu == arm9 ? 0x02000000U : 0x03800000U;
        put32(rom, offset,      0xe59f0078); // LDR r0,[pc,#120] -> +0x80, DISPSTAT
        put32(rom, offset + 4,  0xe3a01008); // MOV r1,#8
        put32(rom, offset + 8,  0xe1c010b0); // STRH r1,[r0] : IRQ VBlank
        put32(rom, offset + 12, 0xe59f0074); // LDR r0,[pc,#116] -> +0x88, IME
        put32(rom, offset + 16, 0xe59f1074); // LDR r1,[pc,#116] -> +0x8c, IE=101
        put32(rom, offset + 20, 0xe5801008); // STR r1,[r0,#8]
        put32(rom, offset + 24, 0xe3a01001); // MOV r1,#1
        put32(rom, offset + 28, 0xe5801000); // STR r1,[r0] : IME
        put32(rom, offset + 32, 0xe321f01f); // MSR cpsr_c,#0x1f
        put32(rom, offset + 36, 0xe59f006c); // LDR r0,[pc,#108] -> +0x98, DMA0
        put32(rom, offset + 40, 0xe59f106c); // LDR r1,[pc,#108] -> +0x9c, source
        put32(rom, offset + 44, 0xe5801000); // STR r1,[r0]
        put32(rom, offset + 48, 0xe59f1068); // LDR r1,[pc,#104] -> +0xa0, destination
        put32(rom, offset + 52, 0xe5801004); // STR r1,[r0,#4]
        put32(rom, offset + 56, 0xe59f1064); // LDR r1,[pc,#100] -> +0xa4, contrôle
        put32(rom, offset + 60, 0xe5801008); // STR r1,[r0,#8] : armer DMA VBlank
        put32(rom, offset + 64, 0xe2844001); // ADD r4,r4,#1 après retour IRQ
        put32(rom, offset + 68, 0xeafffffe); // B .
        put32(rom, offset + 0x80, status); put32(rom, offset + 0x88, 0x04000208); put32(rom, offset + 0x8c, 0x101);
        put32(rom, offset + 0x98, dma0); put32(rom, offset + 0x9c, base + 0xe0); put32(rom, offset + 0xa0, base + 0x200);
        put32(rom, offset + 0xa4, enabled | irq | word | vblank_mode(cpu) | 2);
        put32(rom, offset + 0xe0, 0x11223344); put32(rom, offset + 0xe4, 0xaabbccdd);
    }
    ravenemu::nds::Machine machine;
    for (const auto cpu : {arm9, arm7}) {
        std::vector<std::uint8_t> bios(cpu == arm9 ? 0x1000 : 0x4000);
        put32(bios, 0x18, 0xea000008); // B +0x40
        put32(bios, 0x40, 0xe59f0018); // LDR r0,[pc,#24] -> +0x60, IF
        put32(bios, 0x44, 0xe59f1018); // LDR r1,[pc,#24] -> +0x64, 101
        put32(bios, 0x48, 0xe5801000); // STR r1,[r0] : acquitter LCD et DMA
        put32(bios, 0x4c, 0xe2833001); // ADD r3,r3,#1
        put32(bios, 0x50, 0xe25ef004); // SUBS pc,lr,#4
        put32(bios, 0x60, flags); put32(bios, 0x64, 0x101);
        machine.memory().load_bios(cpu, bios);
    }
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7}) {
        machine.run_steps(cpu, 16);
        check(!machine.memory().dma_pending(cpu) && machine.memory().read16(cpu, vcount) == 0,
              "programme ARM doit armer sans déclencher ni avancer le LCD");
    }
    machine.advance_cycles(192 * line_cycles);
    for (const auto cpu : {arm9, arm7}) {
        const auto base = cpu == arm9 ? 0x02000000U : 0x03800000U;
        const auto before = machine.cpu_state(cpu);
        check(machine.memory().dma_pending(cpu) && machine.memory().read32(cpu, flags) == 1, "VBlank réel doit déclencher DMA et IF");
        machine.run_steps(cpu, 2);
        check(machine.cpu_state(cpu) == before && machine.memory().read32(cpu, base + 0x200) == 0x11223344 &&
              machine.memory().read32(cpu, base + 0x204) == 0xaabbccdd && machine.memory().read32(cpu, flags) == 0x101,
              "DMA VBlank doit précéder l'entrée IRQ sans avancer le CPU");
        machine.step(cpu);
        check(machine.cpu_state(cpu).mode == CpuMode::irq, "entrée IRQ VBlank/DMA");
        machine.run_steps(cpu, 6);
        machine.step(cpu);
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[3] == 1 &&
              machine.cpu_state(cpu).registers[4] == 1 && !machine.memory().irq_pending(cpu), "acquittement LCD/DMA et reprise ARM");
    }
    auto copy = machine;
    copy.advance_cycles(70 * line_cycles);
    check(copy.memory().read16(arm9, vcount) == 262 && machine.memory().read16(arm9, vcount) == 192,
          "copie de machine partage l'horloge LCD");
    auto invalid = rom;
    put32(invalid, 0x34, 0x04000000); put32(invalid, 0x38, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(ravenemu::nds::Cartridge{invalid}); }, "chargement invalide accepté");
    check(machine.memory().read16(arm9, vcount) == 192 && machine.memory().read16(arm9, status) == 9,
          "chargement refusé perd l'état LCD");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7})
        check(machine.memory().read32(cpu, status) == 4 && !machine.memory().dma_pending(cpu) &&
              machine.memory().read32(cpu, flags) == 0, "chargement réussi ne réinitialise pas LCD et demandes");
}

} // namespace

int main() {
    registers_test();
    boundaries_test();
    interrupts_test();
    batching_and_large_intervals_test();
    for (const auto cpu : {arm9, arm7}) vblank_dma_test(cpu);
    hblank_dma_test();
    machine_program_test();
}
