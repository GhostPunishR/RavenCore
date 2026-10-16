#include "machine/machine.hpp"

#include "check.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

/**
 * Fidélité matérielle du cœur GBA, d'après GBATEK : appels du BIOS, HALTCNT,
 * port série sans câble, capture vidéo par DMA3, redémarrage d'un DMA, temps
 * de rendu des objets, bus ouvert et durées des multiplications.
 */
namespace ravenemu::gba::testing {

using ravenemu::testing::check;

constexpr std::uint32_t code_address = 0x0300'0100U;

class Fixture {
public:
    Fixture() : machine_(make_rom(), std::nullopt, std::nullopt, [] { return std::int64_t{0}; }) {}

    Machine& machine() noexcept { return machine_; }
    Bus& bus() noexcept { return machine_.bus; }

    void arm(std::initializer_list<std::uint32_t> code) {
        auto at = code_address;
        for (const auto word : code) { store32(at, word); at += 4; }
        machine_.cpu.reset(static_cast<std::int32_t>(code_address));
    }
    void run_code() {
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
    void store32(std::uint32_t address, std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index)
            machine_.bus.iwram[(address + index) & 0x7fffU] = static_cast<std::uint8_t>(value >> (8U * index));
    }
    [[nodiscard]] int io16(int offset) const {
        return machine_.bus.io[static_cast<std::size_t>(offset)] | machine_.bus.io[static_cast<std::size_t>(offset + 1)] << 8;
    }

private:
    // ROM minimale : boucle `b .` en 0x08000000.
    static RomImage make_rom() {
        std::vector<std::uint8_t> rom(0x200);
        rom[0] = 0xfe; rom[1] = 0xff; rom[2] = 0xff; rom[3] = 0xea;
        rom[0xb2] = 0x96;
        return std::make_shared<const std::vector<std::uint8_t>>(std::move(rom));
    }
    Machine machine_;
};

void bios_checksum_and_midi_key() {
    Fixture fixture;
    fixture.store32(0x0300'0004U, 0x0100'0000U);  // WaveData.frequency
    fixture.arm({
        0xef0d'0000U,  // swi GetBiosChecksum
        0xe1a0'4000U,  // mov r4, r0
        0xe3a0'0403U,  // mov r0, #0x03000000
        0xe3a0'10a8U,  // mov r1, #168
        0xe3a0'2000U,  // mov r2, #0
        0xef1f'0000U,  // swi MidiKey2Freq
        0xeaff'fffeU,  // b .
    });
    fixture.run_code();
    check(fixture.reg(4) == 0xbaae'187fU, "GetBiosChecksum : somme du BIOS GBA attendue");
    check(fixture.reg(0) == 0x0080'0000U, "MidiKey2Freq : une octave sous 180 divise par deux");
}

void sound_bias_keeps_resolution() {
    Fixture fixture;
    fixture.bus().write16(i32(0x0400'0088U), 0xc000);
    fixture.arm({0xe3a0'0001U, 0xef19'0000U, 0xeaff'fffeU});  // mov r0, #1 ; swi SoundBias
    fixture.run_code();
    check(fixture.io16(0x88) == 0xc200, "SoundBias : niveau 0x200 attendu, résolution conservée");
    fixture.arm({0xe3a0'0000U, 0xef19'0000U, 0xeaff'fffeU});
    fixture.run_code();
    check(fixture.io16(0x88) == 0xc000, "SoundBias : niveau 0 attendu");
}

// SoftReset efface 0x03007E00..0x03007FFF et repart en EWRAM si [0x03007FFA] != 0.
void soft_reset_follows_return_flag() {
    Fixture fixture;
    fixture.machine().bus.iwram[0x7ffa] = 1;
    fixture.machine().bus.iwram[0x7e00] = 0x55;
    fixture.arm({0xef00'0000U});  // swi SoftReset
    fixture.machine().cpu.step();
    check(fixture.reg(15) == 0x0200'0000U, "SoftReset : retour en EWRAM attendu");
    check(fixture.machine().bus.iwram[0x7e00] == 0 && fixture.machine().bus.iwram[0x7ffa] == 0,
          "SoftReset : zone réservée de l'IWRAM non effacée");
}

// Écrire HALTCNT arrête le CPU jusqu'à une interruption activée et demandée.
void haltcnt_write_halts() {
    Fixture fixture;
    auto& machine = fixture.machine();
    machine.bus.write16(i32(0x0400'0004U), 0x0008);  // DISPSTAT : IRQ VBlank
    machine.bus.write16(i32(0x0400'0200U), 0x0001);  // IE : VBlank
    machine.bus.write8(i32(0x0400'0301U), 0);
    check(machine.cpu.state.halted, "HALTCNT n'a pas arrêté le CPU");
    machine.run_frame();
    check(!machine.cpu.state.halted, "le CPU arrêté ne s'est pas réveillé au VBlank");
}

// Port série sans câble : l'horloge interne termine le transfert, l'externe attend.
void serial_without_cable() {
    Fixture internal;
    internal.bus().write16(i32(0x0400'0128U), 0x4081);  // départ, horloge interne, IRQ
    internal.machine().run_frame();
    check((internal.io16(0x128) & 0x80) == 0, "transfert série à horloge interne jamais terminé");
    check(internal.machine().bus.io[0x12a] == 0xff, "sans câble, les bits reçus valent 1");
    check((internal.machine().interrupts.flags & 0x80) != 0, "IRQ série absente");

    Fixture external;
    external.bus().write16(i32(0x0400'0128U), 0x4080);  // horloge externe
    external.machine().run_frame();
    check((external.io16(0x128) & 0x80) != 0, "horloge externe sans partenaire : le transfert doit attendre");

    Fixture multi;
    multi.bus().write16(i32(0x0400'012aU), 0x1234);       // SIOMLT_SEND
    multi.bus().write16(i32(0x0400'0128U), 0x6083);       // multijoueur, 115200 bauds, départ, IRQ
    multi.machine().run_frame();
    check((multi.io16(0x128) & 0x80) == 0, "transfert multijoueur jamais terminé");
    check(multi.io16(0x120) == 0x1234 && multi.io16(0x122) == 0xffff, "SIOMULTI : enfants absents à 0xFFFF");
}

// Capture vidéo : DMA3 en déclenchement spécial copie une unité par ligne,
// de VCOUNT 2 à 161, puis s'arrête à la ligne 162.
void dma3_video_capture() {
    Fixture fixture;
    auto& bus = fixture.bus();
    for (std::uint32_t index = 0; index < 0x200; ++index) {
        bus.write32(i32(0x0200'0000U + index * 4), i32(index + 1));
    }
    bus.write32(i32(0x0400'00d4U), i32(0x0200'0000U));
    bus.write32(i32(0x0400'00d8U), i32(0x0201'0000U));
    bus.write16(i32(0x0400'00dcU), 1);
    bus.write16(i32(0x0400'00deU), 0xb600);  // actif, spécial, 32 bits, répétition
    fixture.machine().run_frame();
    check(u32(bus.read32(i32(0x0201'0000U))) == 1, "capture : première ligne absente");
    check(u32(bus.read32(i32(0x0201'0000U + 159 * 4))) == 160, "capture : 160 lignes attendues");
    check(bus.read32(i32(0x0201'0000U + 160 * 4)) == 0, "capture : transfert au-delà de la ligne 161");
    check((fixture.io16(0xde) & 0x8000) == 0, "capture : DMA3 non arrêté à la ligne 162");
}

// Décompressions du BIOS (SWI 0x10 à 0x18), sur des flux construits d'après
// les formats de GBATEK. Les jeux n'en exercent qu'une partie (LZ77 et RLE) :
// ce test couvre aussi BitUnPack, Huffman et les filtres différentiels.
std::vector<int> decompress(int swi, std::initializer_list<int> stream, int output_bytes, std::int32_t parameters = 0) {
    Fixture fixture;
    auto& bus = fixture.bus();
    const auto source = i32(0x0200'0000U);
    const auto destination = i32(0x0201'0000U);
    auto at = source;
    for (const auto byte : stream) {
        bus.write8(at, byte);
        at = add32(at, 1);
    }
    auto& registers = fixture.machine().cpu.state.regs;
    registers[0] = source;
    registers[1] = destination;
    registers[2] = parameters;
    fixture.machine().bios.handle_swi(swi);
    std::vector<int> output;
    for (int index = 0; index < output_bytes; ++index) output.push_back(bus.read8(add32(destination, index)));
    return output;
}

void bios_decompression() {
    // LZ77 : en-tête 0x10, 9 octets ; trois littéraux puis une copie de 6
    // octets à distance 3 (longueur - 3 = 3, distance - 1 = 2).
    const std::vector<int> abcabcabc{'A', 'B', 'C', 'A', 'B', 'C', 'A', 'B', 'C'};
    check(decompress(0x11, {0x10, 9, 0, 0, 0x10, 'A', 'B', 'C', 0x30, 0x02}, 9) == abcabcabc, "LZ77 (WRAM)");
    check(decompress(0x12, {0x10, 9, 0, 0, 0x10, 'A', 'B', 'C', 0x30, 0x02}, 9) == abcabcabc, "LZ77 (VRAM)");
    // RLE : 5 × 'A' (0x80 | 5 - 3), puis 2 octets bruts (2 - 1).
    const std::vector<int> rle{'A', 'A', 'A', 'A', 'A', 'X', 'Y'};
    check(decompress(0x14, {0x30, 7, 0, 0, 0x82, 'A', 0x01, 'X', 'Y'}, 7) == rle, "RLE (WRAM)");
    check(decompress(0x15, {0x30, 7, 0, 0, 0x82, 'A', 0x01, 'X', 'Y'}, 7) == rle, "RLE (VRAM)");
    // Filtres différentiels : chaque valeur ajoute un écart à la précédente.
    check(decompress(0x16, {0x81, 4, 0, 0, 1, 1, 1, 1}, 4) == std::vector<int>{1, 2, 3, 4}, "Diff8 (WRAM)");
    check(decompress(0x17, {0x81, 4, 0, 0, 1, 1, 1, 1}, 4) == std::vector<int>{1, 2, 3, 4}, "Diff8 (VRAM)");
    check(decompress(0x18, {0x82, 4, 0, 0, 0x00, 0x01, 0x01, 0x00}, 4) == std::vector<int>{0x00, 0x01, 0x01, 0x01},
          "Diff16");
    // Huffman 8 bits : racine en S+5 (0xC0, deux feuilles), feuilles 'A' et
    // 'B' en S+6 et S+7, flux en S+8 lu bit 31 en premier : 0, 1, 1, 0.
    check(decompress(0x13, {0x28, 4, 0, 0, 0x01, 0xc0, 'A', 'B', 0, 0, 0, 0x60}, 4) ==
              std::vector<int>{'A', 'B', 'B', 'A'},
          "Huffman");
    // BitUnPack : l'octet 0x93 donne, de 2 bits en 2 bits, 3, 0, 1, 2 ; le
    // décalage 1 s'applique sauf aux zéros (bit 31 du décalage à 0), puis
    // chaque valeur occupe 4 bits du mot écrit : 0x00003204.
    const auto parameters = i32(0x0200'0100U);
    Fixture fixture;
    auto& bus = fixture.bus();
    bus.write8(i32(0x0200'0000U), 0x93);
    bus.write16(parameters, 1);
    bus.write8(add32(parameters, 2), 2);
    bus.write8(add32(parameters, 3), 4);
    bus.write32(add32(parameters, 4), 1);
    auto& registers = fixture.machine().cpu.state.regs;
    registers[0] = i32(0x0200'0000U);
    registers[1] = i32(0x0201'0000U);
    registers[2] = parameters;
    fixture.machine().bios.handle_swi(0x10);
    check(u32(bus.read32(i32(0x0201'0000U))) == 0x0000'3204U, "BitUnPack");
}

// Un DMA ne redémarre que quand le bit 15 de son contrôle passe de 0 à 1.
// GTA Advance arrête son DMA de son en réécrivant 0xB600 en 0x8400 (bit 15
// gardé, déclenchement immédiat), puis efface le bit 15 : si la réécriture
// relançait le canal, 0x4000 mots partiraient de FIFO A à travers les E/S.
void rewriting_an_enabled_dma_keeps_it_stopped() {
    Fixture fixture;
    auto& bus = fixture.bus();
    for (std::uint32_t index = 0; index < 0x100; ++index) {
        bus.write32(i32(0x0200'0000U + index * 4), i32(0x4a10'4a10U));
    }
    bus.write16(i32(0x0400'0200U), 0x0005);         // IE : V-Blank et V-Count
    bus.write32(i32(0x0400'00bcU), i32(0x0200'0000U));
    bus.write32(i32(0x0400'00c0U), i32(0x0400'00a0U));  // FIFO A
    bus.write16(i32(0x0400'00c4U), 0);
    bus.write16(i32(0x0400'00c6U), 0xb600);         // actif, FIFO, 32 bits, répétition
    bus.write16(i32(0x0400'00c6U), 0x8400);         // bit 15 gardé, immédiat
    fixture.machine().run_frame(4096);
    check(fixture.io16(0x200) == 0x0005, "DMA relancé sans passage de 0 à 1 : E/S écrasées");
    check(fixture.io16(0xc6) == 0x8400, "réglages du DMA actif non conservés");
    bus.write16(i32(0x0400'00c6U), 0x0400);
    check(fixture.io16(0xc6) == 0x0400, "DMA non arrêté");

    // Un DMA immédiat efface son bit 15 en fin de transfert : la réécriture
    // suivante est un nouveau passage de 0 à 1, qui relance la copie.
    bus.write32(i32(0x0200'1000U), 0x1111);
    bus.write32(i32(0x0400'00d4U), i32(0x0200'1000U));
    bus.write32(i32(0x0400'00d8U), i32(0x0200'2000U));
    bus.write16(i32(0x0400'00dcU), 1);
    bus.write16(i32(0x0400'00deU), 0x8400);
    fixture.machine().run_frame(256);
    check(u32(bus.read32(i32(0x0200'2000U))) == 0x1111, "DMA immédiat non lancé");
    check((fixture.io16(0xde) & 0x8000) == 0, "DMA immédiat resté actif");
    bus.write32(i32(0x0200'1000U), 0x2222);
    bus.write16(i32(0x0400'00deU), 0x8400);
    fixture.machine().run_frame(256);
    check(u32(bus.read32(i32(0x0200'2000U))) == 0x2222, "DMA immédiat non relancé après sa fin");
}

// Temps de rendu des objets : 1210 cycles par ligne (954 si H-Blank libre).
// Des objets 64×64 hors écran consomment ce temps ; l'objet visible suivant
// n'est tracé que sur les colonnes qu'il reste.
int visible_sprite_columns(int hidden_sprites, bool hblank_free) {
    Fixture fixture;
    auto& bus = fixture.bus();
    const auto oam16 = [&](int offset, int value) {
        bus.oam[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value);
        bus.oam[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>(value >> 8);
    };
    for (int sprite = 0; sprite < 128; ++sprite) oam16(sprite * 8, 0x0200);  // désactivés
    for (int sprite = 0; sprite <= hidden_sprites; ++sprite) {
        const auto x = sprite < hidden_sprites ? 300 : 0;  // 300 : hors écran, mais sur la ligne
        oam16(sprite * 8, 0x0000); oam16(sprite * 8 + 2, 0xc000 | x); oam16(sprite * 8 + 4, 0);
    }
    std::fill(bus.vram.begin() + 0x10000, bus.vram.begin() + 0x10000 + 64 * 32, std::uint8_t{0x11});
    bus.palette[0x202] = 0x1f;  // couleur 1 des objets : rouge
    bus.io[0] = static_cast<std::uint8_t>(hblank_free ? 0x60 : 0x40); bus.io[1] = 0x10;  // OBJ, 1D
    fixture.machine().ppu.tick(1000);  // trace la ligne 0
    const auto backdrop = fixture.machine().ppu.frame[200];
    int columns = 0;
    while (columns < 64 && fixture.machine().ppu.frame[static_cast<std::size_t>(columns)] != backdrop) ++columns;
    return columns;
}

void object_rendering_budget() {
    check(visible_sprite_columns(0, false) == 64, "objet seul : 64 colonnes attendues");
    check(visible_sprite_columns(18, false) == 1210 - 18 * 64, "1210 cycles : 58 colonnes restantes attendues");
    check(visible_sprite_columns(19, false) == 0, "temps épuisé : objet suivant tracé");
    check(visible_sprite_columns(14, true) == 954 - 14 * 64, "H-Blank libre : budget de 954 cycles attendu");
}

// Une zone non câblée rend le dernier mot préchargé par le pipeline.
void open_bus_returns_prefetch() {
    Fixture fixture;
    fixture.arm({
        0xe3a0'1201U,  // mov r1, #0x10000000
        0xe591'0000U,  // ldr r0, [r1]           (en 0x104, précharge 0x10C)
        0xe1a0'0000U,  // nop
        0xeaff'fffeU,  // b .                    (en 0x10C)
    });
    fixture.run_code();
    check(fixture.reg(0) == 0xeaff'fffeU, "bus ouvert : dernier mot préchargé attendu");
}

// Durées (GBATEK) : MUL 1S+mI, UMULL/SMULL 1S+(m+1)I, décalage par registre +1I.
void multiply_and_shift_timings() {
    Fixture fixture;
    fixture.arm({
        0xe3a0'20ffU,  // mov r2, #0xff
        0xe000'0291U,  // mul r0, r1, r2         (m = 1)
        0xe3a0'2412U,  // mov r2, #0x12000000
        0xe000'0291U,  // mul r0, r1, r2         (m = 4)
        0xe3e0'2000U,  // mvn r2, #0             (0xFFFFFFFF)
        0xe000'0291U,  // mul r0, r1, r2         (m = 1 en signé)
        0xe081'0293U,  // umull r0, r1, r3, r2   (m = 4 en non signé)
        0xe0c1'0293U,  // smull r0, r1, r3, r2   (m = 1)
        0xe081'0312U,  // add r0, r1, r2, lsl r3
        0xe081'0182U,  // add r0, r1, r2, lsl #3
    });
    auto& cpu = fixture.machine().cpu;
    std::vector<int> costs;
    for (int step = 0; step < 10; ++step) costs.push_back(cpu.step());
    check(costs[1] == 2 && costs[3] == 5 && costs[5] == 2, "MUL : 1S + mI selon l'opérande");
    check(costs[6] == 6 && costs[7] == 3, "UMULL/SMULL : 1S + (m + 1)I");
    check(costs[8] == 2 && costs[9] == 1, "décalage par registre : 1I de plus");
}

// Un état restauré doit reprendre au cycle près : le CPU ne s'arrête jamais
// ici, si bien qu'une seule attente de trop décale toute la suite. L'accès
// séquentiel en cours n'était pas sauvegardé (bogue antérieur au format 12).
void save_state_resumes_cycle_exact() {
    std::vector<std::uint8_t> rom(0x200);
    rom[0xb2] = 0x96;
    const auto put = [&](std::size_t at, std::uint32_t word) {
        for (unsigned index = 0; index < 4; ++index) rom[at + index] = static_cast<std::uint8_t>(word >> (8U * index));
    };
    put(0, 0xea00'002eU);  // b 0xc0
    for (std::size_t index = 0; index < 16; ++index) put(0xc0 + index * 4, 0xe283'3001U);  // add r3, r3, #1
    put(0x100, 0xeaff'ffeeU);  // b 0xc0
    auto core = make_gba_core(std::nullopt);
    core->set_clock_epoch(0);
    core->load_rom(rom, {});
    std::vector<std::int32_t> frame(240 * 160);
    const auto run = [&](int frames) { for (int index = 0; index < frames; ++index) core->run_frame(frame, false); };
    run(7);
    const auto saved = core->save_state();
    run(10);
    const auto continued = core->save_state();
    core->load_state(saved);
    run(10);
    check(core->save_state() == continued, "état restauré : reprise décalée de quelques cycles");
}

} // namespace ravenemu::gba::testing

int main() {
    using namespace ravenemu::gba::testing;
    bios_checksum_and_midi_key();
    sound_bias_keeps_resolution();
    soft_reset_follows_return_flag();
    haltcnt_write_halts();
    serial_without_cable();
    bios_decompression();
    dma3_video_capture();
    rewriting_an_enabled_dma_keeps_it_stopped();
    object_rendering_budget();
    open_bus_returns_prefetch();
    multiply_and_shift_timings();
    save_state_resumes_cycle_exact();
    return 0;
}
