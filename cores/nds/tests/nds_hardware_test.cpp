#include "bios/decompression.hpp"
#include "cartridge/game_card.hpp"
#include "cartridge/header.hpp"
#include "cpu/cp15.hpp"
#include "gpu/engine2d.hpp"
#include "gpu/vram.hpp"
#include "gpu/lcd_timing.hpp"
#include "ipc/ipc.hpp"
#include "math/divider.hpp"
#include "math/square_root.hpp"
#include "memory/memory.hpp"
#include "timers/timers.hpp"

#include "check.hpp"

#include <cstdint>
#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Composants du DS pris un à un ; valeurs attendues d'après GBATEK.
namespace {

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

constexpr std::uint32_t all = 0xffff'ffffU;

void scheduler_test() {
    Scheduler scheduler;
    check(scheduler.next() == Scheduler::never && !scheduler.take_due(1'000'000), "ordonnanceur vide");
    scheduler.schedule(Event::timers7, 50);
    scheduler.schedule(Event::lcd, 20);
    check(scheduler.next() == 20 && !scheduler.take_due(19), "prochain événement");
    check(scheduler.take_due(60) == Event::lcd && scheduler.take_due(60) == Event::timers7 && !scheduler.take_due(60),
          "événements échus, du plus ancien au plus récent");
}

void ipc_test() {
    InterruptController arm9{irq::arm9_bits};
    InterruptController arm7{irq::arm7_bits};
    Ipc ipc{arm9, arm7};
    arm9.write(0x210, all, all);
    arm7.write(0x210, all, all);
    // IPCSYNC : sortie vers l'autre CPU, IRQ seulement si l'autre l'a permise.
    ipc.write_sync(1, 0x4000, all);
    ipc.write_sync(0, 0x2500, all);
    check((ipc.read_sync(1) & 15U) == 5 && (ipc.read_sync(0) >> 8U & 15U) == 5, "IPCSYNC : 4 bits vers l'autre CPU");
    check((arm7.flags() & (1U << irq::ipc_sync)) != 0 && (arm9.flags() & (1U << irq::ipc_sync)) == 0,
          "IPCSYNC : IRQ de synchronisation chez l'autre CPU");
    // FIFO : ignorée tant que l'émetteur ne l'active pas, puis ordre conservé.
    ipc.send(0, 1);
    check((ipc.read_control(1) & 0x100U) != 0, "FIFO désactivée : écriture ignorée");
    ipc.write_control(0, 0x8004, all);  // active, IRQ « envoi vide »
    ipc.write_control(1, 0x8400, all);  // active, IRQ « réception »
    check((arm9.flags() & (1U << irq::ipc_send_empty)) != 0, "IRQ d'envoi vide sur front montant");
    for (std::uint32_t value = 10; value < 26; ++value) ipc.send(0, value);
    check((ipc.read_control(0) & 0x0002U) != 0 && (arm7.flags() & (1U << irq::ipc_receive)) != 0,
          "16 mots : FIFO pleine, IRQ de réception");
    ipc.send(0, 99);
    check((ipc.read_control(0) & 0x4000U) != 0, "FIFO pleine : erreur à l'envoi");
    check(ipc.receive(1) == 10 && ipc.receive(1) == 11, "FIFO : premier entré, premier sorti");
    for (int index = 0; index < 14; ++index) static_cast<void>(ipc.receive(1));
    check(ipc.receive(1) == 25 && (ipc.read_control(1) & 0x4000U) != 0, "FIFO vide : dernier mot reçu, et erreur");
}

void divider_test() {
    Divider divider;
    const auto divide = [&](std::uint32_t mode, std::uint64_t numerator, std::uint64_t denominator) {
        divider.write(0x280, mode, all, 0);
        divider.write(0x290, static_cast<std::uint32_t>(numerator), all, 0);
        divider.write(0x294, static_cast<std::uint32_t>(numerator >> 32U), all, 0);
        divider.write(0x298, static_cast<std::uint32_t>(denominator), all, 0);
        divider.write(0x29c, static_cast<std::uint32_t>(denominator >> 32U), all, 100);
        const auto quotient = divider.read(0x2a0, 200) | (std::uint64_t{divider.read(0x2a4, 200)} << 32U);
        const auto remainder = divider.read(0x2a8, 200) | (std::uint64_t{divider.read(0x2ac, 200)} << 32U);
        return std::pair{quotient, remainder};
    };
    const auto minus = [](std::int64_t value) { return static_cast<std::uint64_t>(value); };
    check(divide(0, minus(-1234), 10) == std::pair{minus(-123), minus(-4)}, "32/32 signé");
    check((divider.read(0x280, 117) & 0x8000U) != 0 && (divider.read(0x280, 118) & 0x8000U) == 0,
          "32/32 : occupé 18 cycles");
    using Result = std::pair<std::uint64_t, std::uint64_t>;
    check(divide(0, 5, 0) == Result{0x0000'0000'ffff'ffffULL, 5} && (divider.read(0x280, 200) & 0x4000U) != 0,
          "division par zéro : -1, reste = numérateur, 32 bits hauts inversés, DIV0");
    check(divide(0, 0x8000'0000U, 0xffff'ffffU).first == 0x0000'0000'8000'0000ULL, "-MAX / -1 en 32 bits : +MAX sur 64 bits");
    check(divide(1, minus(-(std::int64_t{1} << 40)), 0xffff'fffeU).first == (std::uint64_t{1} << 39),
          "64/32 : dénominateur de 32 bits signé");
    check(divide(2, minus(-9), minus(-2)) == Result{4, minus(-1)}, "64/64");
    divide(0, 1, 0x1'0000'0000ULL);
    check((divider.read(0x280, 200) & 0x4000U) == 0, "DIV0 seulement si les 64 bits du dénominateur sont nuls");
}

void square_root_test() {
    SquareRoot root;
    root.write(0x2b8, 1'000'000, all, 0);
    check(root.read(0x2b4, 13) == 1000 && (root.read(0x2b0, 12) & 0x8000U) != 0, "racine de 32 bits, occupée 13 cycles");
    root.write(0x2bc, 1, all, 0);
    check(root.read(0x2b4, 13) == 1000, "mode 32 bits : moitié haute ignorée");
    root.write(0x2b0, 1, all, 0);
    check(root.read(0x2b4, 13) == 65543, "mode 64 bits : racine de 2^32 + 10^6");
    check(integer_square_root(0xffff'ffff'ffff'ffffULL) == 0xffff'ffffU && integer_square_root(15) == 3,
          "racine entière exacte");
}

void timers_test() {
    InterruptController irq{irq::arm9_bits};
    Timers timers{irq};
    // Timer 0 : F/64, IRQ, départ à 0xFFF0 ; timer 1 en cascade.
    timers.write(0x0, 0x00c1'fff0U, all, 0);
    timers.write(0x4, 0x0084'0000U, all, 0);
    check(timers.next_overflow() == 16 * 64, "prochain débordement");
    check((timers.read(0x0, 64 * 3) & 0xffffU) == 0xfff3, "comptage à F/64");
    timers.advance(16 * 64);
    check((irq.flags() & (1U << irq::timer0)) != 0 && (timers.read(0x0, 16 * 64) & 0xffffU) == 0xfff0,
          "débordement : IRQ et rechargement");
    check((timers.read(0x4, 16 * 64) & 0xffffU) == 1, "cascade : le timer 1 compte les débordements");
    timers.write(0x0, 0x1234, 0xffffU, 2000);
    check((timers.read(0x0, 2000) & 0xffffU) != 0x1234, "rechargement écrit sans toucher au compteur");
}

void lcd_test() {
    InterruptController arm9{irq::arm9_bits};
    InterruptController arm7{irq::arm7_bits};
    LcdTiming lcd{arm9, arm7};
    Scheduler scheduler;
    lcd.write(0, 0x0a38U, all);  // IRQ V-Blank, H-Blank, V-Count ; LYC = 10
    lcd.start(scheduler, 0);
    const auto run_to = [&](Cycles time) {
        while (scheduler.next() <= time) {
            const auto at = scheduler.next();
            static_cast<void>(scheduler.take_due(at));
            static_cast<void>(lcd.on_event(scheduler, at));
        }
    };
    run_to(LcdTiming::hblank_start[0]);
    check(lcd.line() == 0 && (lcd.read(0) & 2U) != 0 && (lcd.read(1) & 2U) == 0, "H-Blank de l'ARM9 avant celui de l'ARM7");
    run_to(10 * LcdTiming::line_cycles);
    check(lcd.line() == 10 && (lcd.read(0) & 4U) != 0 && (arm9.flags() & (1U << irq::vcount)) != 0, "V-Count");
    run_to(192 * LcdTiming::line_cycles);
    check((lcd.read(0) & 1U) != 0 && (arm9.flags() & 1U) != 0 && lcd.frames() == 1, "V-Blank à la ligne 192");
    run_to(262 * LcdTiming::line_cycles);
    check(lcd.line() == 262 && (lcd.read(0) & 1U) == 0, "pas de drapeau V-Blank sur la dernière ligne");
    check((lcd.read(0) >> 16U) == 262, "VCOUNT sur 9 bits");
}

void cp15_test() {
    Cp15 cp15;
    cp15.reset();
    check(cp15.read({15, 0, 0, 0, 0}) == 0x4105'9461U && cp15.read({15, 0, 0, 0, 2}) == 0x0014'0180U, "identifiants");
    check(cp15.vector_base() == 0xffff'0000U && cp15.control() == 0x2078U, "mise sous tension : vecteurs hauts");
    cp15.write({15, 0, 1, 0, 0}, all);
    check(cp15.control() == 0x000f'f0fdU && !cp15.load_interworking(), "contrôle : bits modifiables de GBATEK");
    cp15.write({15, 0, 1, 0, 0}, Cp15::itcm_enable | Cp15::dtcm_enable);
    cp15.write({15, 0, 9, 1, 0}, 0x027c'000aU);
    cp15.write({15, 0, 9, 1, 1}, 0x0000'0020U);
    check(cp15.in_dtcm(0x027c'3ffcU) && !cp15.in_dtcm(0x027c'4000U) && cp15.dtcm_base() == 0x027c'0000U, "DTCM de 16 Kio");
    check(cp15.in_itcm(0x01ff'fffcU) && !cp15.in_itcm(0x0200'0000U), "ITCM : 32 Mio de miroirs");
    *cp15.itcm(0x0000'8000U) = 0x5a;
    check(*cp15.itcm(0) == 0x5a, "ITCM : miroir tous les 32 Kio");
    check(cp15.write({15, 0, 7, 0, 4}, 0) && cp15.write({15, 0, 7, 8, 2}, 0) && !cp15.write({15, 0, 7, 5, 0}, 0),
          "Wait For Interrupt");
}

void memory_test() {
    Memory memory;
    memory.wram_control = 1;  // ARM9 : 2e moitié ; ARM7 : 1re
    *memory.wram9(0x0300'0000U) = 1;
    *memory.wram7(0x0300'0000U) = 2;
    check(memory.shared_wram[0x4000] == 1 && memory.shared_wram[0] == 2, "WRAMCNT = 1");
    memory.wram_control = 3;
    check(memory.wram9(0x0300'0000U) == nullptr && memory.wram7(0x037f'8000U) == &memory.shared_wram[0],
          "WRAMCNT = 3 : tout à l'ARM7, rien à l'ARM9");
    memory.wram_control = 0;
    check(memory.wram7(0x0300'0010U) == &memory.arm7_wram[0x10], "ARM7 sans WRAM partagée : miroirs de sa WRAM");
    check(memory.main(0x0240'0004U) == &memory.main_ram[4], "RAM principale en miroir tous les 4 Mio");
}

std::vector<std::uint8_t> header_rom(std::string_view code) {
    std::vector<std::uint8_t> rom(0x8000);
    for (std::size_t index = 0; index < code.size(); ++index) rom[0x0c + index] = static_cast<std::uint8_t>(code[index]);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) rom[at + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
    };
    put(0x20, 0x4000);
    put(0x24, 0x0200'0000U);
    put(0x28, 0x0200'0000U);
    put(0x2c, 0x1000);
    put(0x30, 0x6000);
    put(0x34, 0x037f'8000U);
    put(0x38, 0x037f'8000U);
    put(0x3c, 0x100);
    const auto crc = crc16(std::span<const std::uint8_t>{rom}.first(0x15e));
    rom[0x15e] = static_cast<std::uint8_t>(crc);
    rom[0x15f] = static_cast<std::uint8_t>(crc >> 8U);
    return rom;
}

void header_test() {
    const std::vector<std::uint8_t> check_value{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(crc16(check_value) == 0x4b37, "CRC-16 : valeur de contrôle de l'algorithme");
    auto rom = header_rom("ABCE");
    const auto header = parse_header(rom);
    check(header.game_code == "ABCE" && header.arm7_address == 0x037f'8000U, "lecture de l'en-tête");
    check(secure_area(rom, header) == SecureArea::absent, "zone sécurisée nulle : absente");
    rom[0x4000] = 0x12;
    check(secure_area(rom, header) == SecureArea::encrypted, "zone sécurisée chiffrée");
    const std::string_view id{"encryObj"};
    for (std::size_t index = 0; index < id.size(); ++index) rom[0x4000 + index] = static_cast<std::uint8_t>(id[index]);
    check(secure_area(rom, header) == SecureArea::decrypted, "zone sécurisée déchiffrée");
    auto homebrew = header_rom("####");
    homebrew[0x4000] = 0x12;
    check(secure_area(homebrew, parse_header(homebrew)) == SecureArea::absent, "homebrew : pas de zone sécurisée");
    rom[0x10] ^= 1U;
    expect_failure<CartridgeError>([&] { static_cast<void>(parse_header(rom)); }, "CRC d'en-tête faux accepté");
    auto outside = header_rom("ABCE");
    outside[0x2e] = 0x10;  // ARM9 de 1 Mio + 0x1000, au-delà de la ROM
    const auto crc = crc16(std::span<const std::uint8_t>{outside}.first(0x15e));
    outside[0x15e] = static_cast<std::uint8_t>(crc);
    outside[0x15f] = static_cast<std::uint8_t>(crc >> 8U);
    expect_failure<CartridgeError>([&] { static_cast<void>(parse_header(outside)); }, "binaire hors de la ROM accepté");
}

void vram_test() {
    Vram vram;
    vram.set_control(0, 0x80);  // A : LCDC
    vram.write(Vram::lcdc, 0x10, 0x1234, 2);
    check(vram.read(Vram::lcdc, 0x10, 2) == 0x1234 && vram.read(Vram::bg_a, 0x10, 2) == 0, "A en LCDC à 0x06800000");
    vram.set_control(0, 0x81 | (1U << 3U));  // A : fonds du moteur A, OFS = 1
    check(vram.read(Vram::bg_a, 0x20010, 2) == 0x1234 && vram.read(Vram::lcdc, 0x10, 2) == 0,
          "A sur les fonds du moteur A, à 0x20000 × OFS");
    vram.set_control(4, 0x81);  // E : fonds du moteur A, à 0
    vram.set_control(5, 0x81 | (2U << 3U));  // F : 64 Kio × OFS.1
    vram.write(Vram::bg_a, 0x10000, 0x00ff, 2);
    check(vram.read(Vram::lcdc, 0, 2) == 0 && vram.bank_halfword(5, 0) == 0x00ff, "F placé par OFS.1");
    vram.set_control(6, 0x81 | (2U << 3U));  // G au même endroit que F
    vram.write(Vram::bg_a, 0x10002, 0x0f0f, 2);
    vram.set_control(6, 0x80);
    vram.write(Vram::lcdc, 0x94000, 0xf000, 2);  // G en LCDC : 0x06894000
    vram.set_control(6, 0x81 | (2U << 3U));
    check(vram.read(Vram::bg_a, 0x10002, 2) == 0x0f0f && vram.read(Vram::bg_a, 0x10000, 2) == 0xf0ff,
          "banques superposées : écriture dans les deux, lecture combinée");
    vram.set_control(2, 0x82);  // C : ARM7
    check(vram.arm7_status() == 1, "VRAMSTAT : C donnée à l'ARM7");
}

void engine_test() {
    Vram vram;
    Memory memory;
    vram.set_control(0, 0x81);  // A : fonds du moteur A
    Engine2d engine{true};
    const auto palette = [&](unsigned index, std::uint16_t color) { store(&memory.palette[index * 2], color, 2); };
    palette(0, 0x001f);   // fond : rouge
    palette(1, 0x03e0);   // vert
    palette(17, 0x7c00);  // bleu (palette 1, couleur 1)
    // BG0 : tuiles à 0, carte à 0x800 ; tuile 1 pleine de couleur 1 ; carte : tuile 1, puis tuile 1 en palette 1.
    for (unsigned byte = 0; byte < 32; ++byte) vram.write(Vram::bg_a, 32 + byte, 0x11, 1);
    vram.write(Vram::bg_a, 0x800, 0x0001, 2);
    vram.write(Vram::bg_a, 0x802, 0x1001, 2);
    engine.write(0x08, 1U << 8U, 0xffffU);         // BG0CNT : carte au bloc 1
    engine.write(0x00, 0x0001'0100U, all);         // affichage normal, BG0
    std::array<std::uint16_t, 256> line{};
    engine.render_line(0, vram, memory, line);
    check(line[0] == 0x03e0 && line[8] == 0x7c00 && line[16] == 0x001f, "fond texte, 16 couleurs, et couleur de fond");
    engine.write(0x10, 4, 0x1ffU);  // BG0HOFS = 4
    engine.render_line(0, vram, memory, line);
    check(line[3] == 0x03e0 && line[4] == 0x7c00 && line[12] == 0x001f, "défilement horizontal");
    engine.write(0x00, 0x0002'0000U, all);  // affichage de la VRAM, banque A
    vram.set_control(0, 0x80);
    vram.write(Vram::lcdc, (5 * 256 + 7) * 2, 0x1234, 2);
    engine.render_line(5, vram, memory, line);
    check(line[7] == 0x1234, "mode 2 : la banque A telle quelle");
    engine.write(0x00, 0, all);
    engine.render_line(0, vram, memory, line);
    check(line[0] == 0x7fff, "affichage éteint : blanc");
    check(to_argb(0x001f, 0) == 0xffff'0000U && to_argb(0x7c00, 0) == 0xff00'00ffU, "BGR555 vers ARGB8888");
    check(to_argb(0, 0x4010) == 0xffff'ffffU && to_argb(0x7fff, 0x8008) == 0xff82'8282U,
          "luminosité maîtresse vers le blanc et vers le noir");
}

// Mémoire plate pour les décompressions du BIOS.
struct ByteBus {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x1000);
    std::uint32_t read8(std::uint32_t at) const { return bytes[at]; }
    std::uint32_t read16(std::uint32_t at) const { return load(&bytes[at], 2); }
    std::uint32_t read32(std::uint32_t at) const { return load(&bytes[at], 4); }
    void write8(std::uint32_t at, std::uint32_t value) { bytes[at] = static_cast<std::uint8_t>(value); }
    void write16(std::uint32_t at, std::uint32_t value) { store(&bytes[at], value, 2); }
    void write32(std::uint32_t at, std::uint32_t value) { store(&bytes[at], value, 4); }
    void put(std::uint32_t at, std::initializer_list<std::uint8_t> data) {
        for (const auto byte : data) bytes[at++] = byte;
    }
    [[nodiscard]] std::string text(std::uint32_t at, std::size_t length) const {
        return {reinterpret_cast<const char*>(&bytes[at]), length};
    }
};

void decompression_test() {
    ByteBus bus;
    // LZ77 : « ABC », puis 6 octets copiés 3 en arrière, puis « X ».
    bus.put(0x100, {0x10, 10, 0, 0, 0x10, 'A', 'B', 'C', 0x30, 0x02, 'X'});
    decompression::lz77(bus, 0x100, 0x400);
    check(bus.text(0x400, 10) == "ABCABCABCX", "LZ77 : blocs bruts et références");
    // RL : 5 « A » répétés, puis « BC » tels quels.
    bus.put(0x200, {0x30, 7, 0, 0, 0x82, 'A', 0x01, 'B', 'C'});
    decompression::run_length(bus, 0x200, 0x500);
    check(bus.text(0x500, 7) == "AAAAABC", "RL : répétition et octets bruts");
    bus.put(0x300, {0x81, 4, 0, 0, 1, 1, 1, 0xff});
    decompression::differential(bus, 0x300, 0x600, false);
    check(bus.read32(0x600) == 0x0203'0201U, "filtre différentiel de 8 bits");
    bus.put(0x320, {0x82, 4, 0, 0, 0x00, 0x01, 0x01, 0x00});
    decompression::differential(bus, 0x320, 0x610, true);
    check(bus.read16(0x610) == 0x0100 && bus.read16(0x612) == 0x0101, "filtre différentiel de 16 bits");
    // BitUnPack : 1 bit vers 4 bits ; décalage 2 sur les unités non nulles.
    bus.put(0x340, {0xa5});
    bus.put(0x350, {1, 0, 1, 4, 0, 0, 0, 0});
    decompression::bit_unpack(bus, 0x340, 0x700, 0x350);
    check(bus.read32(0x700) == 0x1010'0101U, "BitUnPack de 1 à 4 bits");
    bus.put(0x350, {1, 0, 1, 4, 2, 0, 0, 0});
    decompression::bit_unpack(bus, 0x340, 0x704, 0x350);
    check(bus.read32(0x704) == 0x3030'0303U, "BitUnPack : décalage des unités non nulles");
}

void objects_test() {
    Vram vram;
    Memory memory;
    vram.set_control(1, 0x82);  // B : objets du moteur A
    vram.set_control(0, 0x81);  // A : fonds du moteur A
    const auto put16 = [](std::vector<std::uint8_t>& bytes, unsigned at, std::uint32_t value) { store(&bytes[at], value, 2); };
    put16(memory.palette, 0, 0x001f);          // fond : rouge
    put16(memory.palette, 0x200 + 2, 0x03e0);  // objets, couleur 1 : vert
    put16(memory.palette, 2, 0x7c00);          // fonds, couleur 1 : bleu
    // Tuile 0 : couleur 1 partout ; tuile 1 : couleur 1 dans la seule colonne 0.
    for (unsigned byte = 0; byte < 32; ++byte) vram.write(Vram::obj_a, byte, 0x11, 1);
    for (unsigned row = 0; row < 8; ++row) vram.write(Vram::obj_a, 32 + row * 4, 0x01, 1);
    const auto object = [&](unsigned index, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2) {
        put16(memory.oam, index * 8, a0);
        put16(memory.oam, index * 8 + 2, a1);
        put16(memory.oam, index * 8 + 4, a2);
    };
    object(0, 10, 20, 0x0000);                   // 8 × 8 à (20, 10)
    object(1, 10, 40 | 0x1000U, 0x0001);         // tuile 1 retournée horizontalement
    object(5, 10 | 0x100U, 100, 0x0000);         // affine, paramètres du groupe 0
    object(6, 10 | 0x200U, 150, 0x0000);         // masqué
    put16(memory.oam, 6, 0x100);                 // PA = 1
    put16(memory.oam, 30, 0x100);                // PD = 1
    Engine2d engine{true};
    engine.write(0x00, 0x0001'1010U, all);       // normal, objets, correspondance 1D
    std::array<std::uint16_t, 256> line{};
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x03e0 && line[27] == 0x03e0 && line[19] == 0x001f && line[28] == 0x001f, "objet de 8 × 8");
    check(line[47] == 0x03e0 && line[40] == 0x001f, "retournement horizontal");
    check(line[100] == 0x03e0 && line[107] == 0x03e0 && line[108] == 0x001f, "objet affine, matrice identité");
    check(line[150] == 0x001f, "objet masqué");
    engine.render_line(18, vram, memory, line);
    check(line[20] == 0x001f, "objet limité à ses 8 lignes");
    // BG0 de priorité 0 partout ; l'objet 0 passe en priorité 1, puis 0.
    for (unsigned byte = 0; byte < 32; ++byte) vram.write(Vram::bg_a, 32 + byte, 0x11, 1);
    for (unsigned entry = 0; entry < 32 * 32; ++entry) vram.write(Vram::bg_a, 0x800 + entry * 2, 1, 2);
    engine.write(0x08, 1U << 8U, 0xffffU);
    engine.write(0x00, 0x0001'1110U, all);
    object(0, 10, 20, 0x0400);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x7c00, "fond de priorité 0 devant un objet de priorité 1");
    object(0, 10, 20, 0x0000);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x03e0, "objet devant un fond de même priorité");
}

void affine_backgrounds_test() {
    Vram vram;
    Memory memory;
    vram.set_control(0, 0x81);  // A : fonds du moteur A
    store(&memory.palette[0], 0x001f, 2);  // fond : rouge
    store(&memory.palette[2], 0x03e0, 2);  // couleur 1 : vert
    for (unsigned byte = 0; byte < 64; ++byte) vram.write(Vram::bg_a, 64 + byte, 1, 1);  // tuile 1 (256 couleurs)
    for (unsigned entry = 0; entry < 16; ++entry) vram.write(Vram::bg_a, 0x800 + entry, 1, 1);  // carte : rangée 0
    Engine2d engine{true};
    const auto affine = [&](std::uint32_t pa, std::uint32_t pd) {
        engine.write(0x30, pa, 0xffffU);           // BG3PA
        engine.write(0x34, pd << 16U, 0xffff'0000U);  // BG3PD
        engine.write(0x38, 0, all);                // BG3X
        engine.write(0x3c, 0, all);                // BG3Y
    };
    engine.write(0x0c, (1U << 8U) << 16U, 0xffff'0000U);  // BG3CNT : carte au bloc 1, 128 × 128
    engine.write(0x00, 0x0001'0801U, all);                 // mode 1, BG3 (affine)
    affine(0x100, 0x100);
    std::array<std::uint16_t, 256> line{};
    for (unsigned y = 0; y <= 8; ++y) {
        engine.render_line(y, vram, memory, line);
        if (y == 7) check(line[0] == 0x03e0 && line[127] == 0x03e0 && line[128] == 0x001f, "fond affine : identité, sans répétition");
    }
    check(line[0] == 0x001f, "fond affine : PD avance la ligne lue");
    engine.write(0x0c, ((1U << 8U) | 0x2000U) << 16U, 0xffff'0000U);  // répétition
    engine.render_line(0, vram, memory, line);
    check(line[200] == 0x03e0, "fond affine : répétition au-delà de 128 points");
    engine.write(0x0c, (1U << 8U) << 16U, 0xffff'0000U);
    affine(0x200, 0x100);
    engine.render_line(0, vram, memory, line);
    check(line[63] == 0x03e0 && line[64] == 0x001f, "fond affine : PA = 2 réduit de moitié");
    // Bitmap de 256 couleurs (mode 5, BG3, bit 7) au bloc de 16 Kio n° 2.
    vram.write(Vram::bg_a, 0x8000 + 5, 1, 1);
    engine.write(0x0c, ((2U << 8U) | 0x80U) << 16U, 0xffff'0000U);
    engine.write(0x00, 0x0001'0805U, all);
    affine(0x100, 0x100);
    engine.render_line(0, vram, memory, line);
    check(line[5] == 0x03e0 && line[4] == 0x001f, "bitmap de 256 couleurs");
    // Couleurs directes (bits 7 et 2) : bit 15 levé = point visible.
    vram.write(Vram::bg_a, 0x8000 + 3 * 2, 0x8000 | 0x7c00, 2);
    engine.write(0x0c, ((2U << 8U) | 0x84U) << 16U, 0xffff'0000U);
    engine.render_line(0, vram, memory, line);
    check(line[3] == 0x7c00 && line[4] == 0x001f, "bitmap en couleurs directes");
    // Grand bitmap (mode 6, BG2) : toute la VRAM des fonds, depuis 0.
    vram.write(Vram::bg_a, 10, 1, 1);
    engine.write(0x20, 0x100, 0xffffU);
    engine.write(0x24, 0x100U << 16U, 0xffff'0000U);
    engine.write(0x00, 0x0001'0406U, all);
    engine.render_line(0, vram, memory, line);
    check(line[10] == 0x03e0, "grand bitmap du mode 6");
}

void windows_and_effects_test() {
    Vram vram;
    Memory memory;
    vram.set_control(0, 0x81);  // A : fonds
    vram.set_control(1, 0x82);  // B : objets
    store(&memory.palette[0], 0x001f, 2);          // fond : rouge
    store(&memory.palette[2], 0x03e0, 2);          // BG, couleur 1 : vert
    store(&memory.palette[0x200 + 2], 0x7c00, 2);  // objets, couleur 1 : bleu
    for (unsigned byte = 0; byte < 32; ++byte) vram.write(Vram::bg_a, 32 + byte, 0x11, 1);
    for (unsigned entry = 0; entry < 32 * 32; ++entry) vram.write(Vram::bg_a, 0x800 + entry * 2, 1, 2);
    Engine2d engine{true};
    engine.write(0x08, 1U << 8U, 0xffffU);       // BG0 : carte au bloc 1, vert partout
    engine.write(0x00, 0x0001'2100U, all);       // normal, BG0, WIN0
    engine.write(0x40, (16U << 8U) | 32U, 0xffffU);             // WIN0H : x de 16 à 31
    engine.write(0x44, 192U, 0xffffU);                          // WIN0V : lignes 0 à 191
    engine.write(0x48, 0, 0xffffU);                             // WININ : rien dans WIN0
    engine.write(0x48, 0x3fU << 16U, 0xffff'0000U);             // WINOUT : tout dehors
    std::array<std::uint16_t, 256> line{};
    engine.render_line(0, vram, memory, line);
    check(line[20] == 0x001f && line[40] == 0x03e0 && line[15] == 0x03e0, "WIN0 : BG0 masqué dans la fenêtre");
    engine.write(0x00, 0x0001'0100U, all);                      // sans fenêtre
    engine.write(0x50, 0x00c1, 0xffffU);                        // BLDCNT : assombrir BG0
    engine.write(0x54, 8, 0xffffU);                             // BLDY = 8
    engine.render_line(0, vram, memory, line);
    check(line[40] == 0x0200, "assombrissement de moitié");
    engine.write(0x50, 0x2041U | (8U << 16U), all);             // transparence BG0 sur le fond ; EVA = 8
    engine.write(0x50, 8U << 24U, 0xff00'0000U);                // EVB = 8
    engine.render_line(0, vram, memory, line);
    check(line[40] == 0x01ef, "transparence : moitié vert, moitié rouge");
    // Objet semi-transparent (mode 1) sur BG0, cible seconde BG0, sans autre effet.
    store(&memory.oam[0], 10U | (1U << 10U), 2);
    store(&memory.oam[2], 20, 2);
    for (unsigned byte = 0; byte < 32; ++byte) vram.write(Vram::obj_a, byte, 0x11, 1);
    engine.write(0x00, 0x0001'1110U, all);                      // BG0, objets, 1D
    engine.write(0x50, 0x0100U | (8U << 16U), all);             // seconde cible BG0 ; EVA = 8
    engine.write(0x50, 8U << 24U, 0xff00'0000U);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x3dE0 && line[30] == 0x03e0, "objet semi-transparent mélangé au fond dessous");
}

// Bus de la cartouche, d'après GBATEK (« DS Cartridge I/O Ports », « DS
// Cartridge Protocol ») : chaque transfert est mené événement par événement.
void game_card_test() {
    std::vector<std::uint8_t> rom(0x20000);
    for (std::size_t index = 0; index < rom.size(); ++index) rom[index] = static_cast<std::uint8_t>(index ^ (index >> 8U));
    for (unsigned byte = 0; byte < 4; ++byte) rom[0x60 + byte] = static_cast<std::uint8_t>(0x0058'6000U >> (8U * byte));
    GameCard card;
    card.insert(rom, 0x0000'00c2U);  // puce de 1 Mio
    Scheduler scheduler;
    InterruptController irq{irq::arm9_bits};
    check(card.read(0x04, 0) == 0x2058'6000U, "ROMCTRL après le démarrage : celui de l'en-tête, reset relâché");
    const auto command = [&](std::uint8_t first, std::uint32_t address) {
        card.write(0x08, first | ((address >> 24U) << 8U) | (((address >> 16U) & 0xffU) << 16U) | (((address >> 8U) & 0xffU) << 24U),
                   all, 0, scheduler);
        card.write(0x0c, address & 0xffU, all, 0, scheduler);
    };
    // Mène le transfert : à chaque événement, le mot prêt est lu aussitôt.
    std::vector<Cycles> times;
    const auto transfer = [&](std::uint32_t control) {
        std::vector<std::uint8_t> bytes;
        times.clear();
        card.write(0x04, control, all, 0, scheduler);
        while ((card.read(0x04, 0) & 0x8000'0000U) != 0) {
            const auto at = scheduler.time_of(Event::card);
            static_cast<void>(scheduler.take_due(at));
            times.push_back(at);
            if (!card.on_event(irq)) continue;
            check((card.read(0x04, 0) & 0x0080'0000U) != 0, "mot prêt : DRQ (ROMCTRL bit 23)");
            const auto word = card.read_data(at, scheduler, irq);
            check((card.read(0x04, 0) & 0x0080'0000U) == 0, "mot lu : DRQ retombé");
            for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(static_cast<std::uint8_t>(word >> (8U * byte)));
        }
        return bytes;
    };
    const auto rom_at = [&](std::size_t offset, std::size_t length) {
        return std::vector<std::uint8_t>(rom.begin() + static_cast<std::ptrdiff_t>(offset),
                                         rom.begin() + static_cast<std::ptrdiff_t>(offset + length));
    };
    command(0xb7, 0x8000);
    card.write(0x04, 0xa158'6000U, all, 0, scheduler);
    check((card.read(0x04, 0) & 0x8000'0000U) == 0, "slot coupé (AUXSPICNT bit 15) : pas de transfert");
    card.write(0x00, 0xc000U, 0xffffU, 0, scheduler);  // slot activé, IRQ de fin
    check(transfer(0xa158'6000U) == rom_at(0x8000, 0x200), "B7 : 0x200 octets à 0x8000");
    check(times.front() == (8 + 4) * 5 && times[1] == times[0] + 4 * 5, "commande, puis 4 octets par mot, à 5 cycles par octet");
    check((irq.flags() & (1U << irq::card_done)) != 0, "IRQ de fin de transfert");
    command(0xb7, 0x8f00);
    const auto wrapped = transfer(0xa158'6000U);
    check(rom_at(0x8f00, 0x100) == std::vector<std::uint8_t>(wrapped.begin(), wrapped.begin() + 0x100) &&
              wrapped[0x100] == rom[0x8000],
          "B7 : le flux boucle dans son bloc de 4 Kio");
    command(0xb7, 0x0010);
    check(transfer(0xa758'6000U) == rom_at(0x8010, 4), "B7 : 0x0010 lit 0x8010");
    command(0xb7, 0x10'8000);
    check(transfer(0xa758'6000U) == rom_at(0x8000, 4), "B7 : au-delà de la puce de 1 Mio, miroir");
    command(0xb7, 0x3'0000);
    check(transfer(0xa758'6000U) == std::vector<std::uint8_t>(4, 0xff), "B7 : dans la puce, après la ROM : 0xFF");
    command(0xb8, 0);
    check(transfer(0xa758'6000U) == std::vector<std::uint8_t>{0xc2, 0, 0, 0}, "B8 : identifiant de la puce");
    command(0xb7, 0x8000);
    check(transfer(0xa058'6000U).empty() && times == std::vector<Cycles>{8 * 5}, "transfert sans données : fini après la commande");
    command(0xb7, 0x8000);
    static_cast<void>(transfer(0xaa58'6000U));  // 0x400 octets, horloge de 8 cycles
    check(times[0x80] == times[0x7f] + (4 + 0x18) * 8 && times[0x81] == times[0x80] + 4 * 8,
          "gap2 (0x18 périodes) tous les 0x200 octets, horloge de 8 cycles");
}

// Puce de sauvegarde, d'après GBATEK (« DS Cartridge Backup », « DS Firmware
// Serial Flash Memory ») : une commande par sélection de la puce.
void save_memory_test() {
    SaveMemory chip;
    const auto command = [&](std::initializer_list<std::uint8_t> bytes) {
        std::vector<std::uint8_t> replies;
        for (const auto byte : bytes) replies.push_back(chip.transfer(byte));
        chip.deselect();
        return replies;
    };
    check(command({0x05, 0x00}) == std::vector<std::uint8_t>{0xff, 0xff}, "pas de puce : 0xFF");
    // EEPROM de 0,5 Kio : adresse d'un octet, 0Ah/0Bh pour 100h-1FFh.
    chip.set_type(SaveType::eeprom_512);
    check(command({0x05, 0})[1] == 0xf0, "EEPROM de 0,5 Kio : état F0h");
    command({0x02, 0x10, 'A', 'B'});
    check(command({0x03, 0x10, 0})[2] == 0xff, "écriture sans WREN : ignorée");
    command({0x06});
    check(command({0x05, 0})[1] == 0xf2, "WREN : WEL levé");
    command({0x02, 0x10, 'A', 'B'});
    check(command({0x05, 0})[1] == 0xf0 && command({0x03, 0x10, 0, 0}) == std::vector<std::uint8_t>{0xff, 0xff, 'A', 'B'},
          "écriture puis lecture ; WEL retombé en fin d'écriture");
    command({0x06});
    command({0x0a, 0x05, 'H'});
    check(chip.data()[0x105] == 'H' && command({0x0b, 0x05, 0})[2] == 'H', "WRHI et RDHI : 100h-1FFh");
    command({0x06});
    command({0x02, 0x0e, 1, 2, 3, 4});
    check(chip.data()[0x0e] == 1 && chip.data()[0x0f] == 2 && chip.data()[0x00] == 3 && chip.data()[0x01] == 4,
          "écriture bouclée dans la page de 16 octets");
    // EEPROM de 8 Kio : adresse de 2 octets, protection par WP.
    chip.set_type(SaveType::eeprom_8k);
    command({0x06});
    command({0x02, 0x12, 0x34, 0x5a});
    check(chip.data()[0x1234] == 0x5a && chip.dirty() && command({0x03, 0x12, 0x34, 0})[3] == 0x5a,
          "EEPROM de 8 Kio : adresse de 2 octets ; écriture signalée à l'hôte");
    command({0x06});
    command({0x01, 0x0c});  // WRSR : toute la mémoire protégée
    command({0x06});
    command({0x02, 0x00, 0x00, 0x77});
    check(command({0x05, 0})[1] == 0x0c && chip.data()[0] == 0xff, "WP = 3 : écriture refusée, WEL retombé");
    // Flash de 256 Kio : RDID, PW, PP, PE, SE, FAST.
    chip.set_type(SaveType::flash_256k);
    check(command({0x9f, 0, 0, 0}) == std::vector<std::uint8_t>{0xff, 0x20, 0x40, 0x12}, "Flash : identifiant JEDEC");
    command({0x06});
    command({0x0a, 0x01, 0x02, 0x03, 0xf0});
    command({0x06});
    command({0x02, 0x01, 0x02, 0x03, 0x3c});
    check(chip.data()[0x10203] == 0x30, "PW écrit, PP ne fait que passer des bits à 0");
    check(command({0x0b, 0x01, 0x02, 0x03, 0x00, 0x00})[5] == 0x30, "FAST : un octet muet après l'adresse");
    command({0x06});
    command({0xdb, 0x01, 0x02, 0x80});
    check(chip.data()[0x10203] == 0xff, "PE : page de 0x100 octets effacée");
    command({0x06});
    command({0x02, 0x00, 0x00, 0x00, 0x00});
    command({0x06});
    command({0xd8, 0x00, 0x80, 0x00});
    check(chip.data()[0] == 0xff, "SE : secteur de 0x10000 octets effacé");
    command({0xb9});
    check(command({0x9f, 0})[1] == 0xff, "DP : la puce n'écoute plus");
    command({0xab});
    check(command({0x9f, 0})[1] == 0x20, "RDP : la puce répond de nouveau");
    // Type déduit de la taille d'un fichier.
    SaveMemory loaded;
    check(loaded.load(std::vector<std::uint8_t>(64 * 1024, 0x11)) && loaded.type() == SaveType::eeprom_64k &&
              !loaded.load(std::vector<std::uint8_t>(100)),
          "fichier de 64 Kio : EEPROM de 64 Kio ; taille inconnue refusée");
}

// Détection du type de la puce de sauvegarde, d'après GBATEK (« Detection (in
// emulators) ») : commande et adresse viennent d'une instruction, les données
// d'une autre.
void save_detection_test() {
    constexpr std::uint32_t command_instruction = 0x0380'0670U;
    constexpr std::uint32_t data_instruction = 0x0380'06b4U;
    SaveMemory chip;
    const auto command = [&](std::initializer_list<std::uint8_t> head, std::initializer_list<std::uint8_t> data) {
        std::vector<std::uint8_t> replies;
        for (const auto byte : head) replies.push_back(chip.transfer(byte, command_instruction));
        for (const auto byte : data) replies.push_back(chip.transfer(byte, data_instruction));
        chip.deselect();
        return replies;
    };
    chip.set_type(SaveType::automatic);
    check(command({0x05}, {0})[1] == 0x00 && chip.type() == SaveType::automatic, "RDSR avant détection : 00h");
    command({0x03, 0x00, 0x00, 0x00, 0x00}, {});
    check(chip.type() == SaveType::automatic, "tout depuis la même instruction : type encore inconnu");
    command({0x0b, 0x05}, {0x00});
    check(chip.type() == SaveType::eeprom_512, "1 octet d'adresse : EEPROM de 0,5 Kio");
    chip.set_type(SaveType::automatic);
    command({0x06}, {});
    command({0x02, 0x01, 0x23}, {0x5a});
    check(chip.type() == SaveType::eeprom_64k && chip.data()[0x0123] == 0x5a, "2 octets d'adresse : EEPROM de 64 Kio, écriture rejouée");
    chip.set_type(SaveType::automatic);
    command({0x03, 0x00, 0x00, 0x10}, {0x00});
    check(chip.type() == SaveType::flash_1m, "3 octets, lecture : Flash de 1 Mio en attendant une écriture");
    command({0x06}, {});
    command({0x02, 0x00, 0x00, 0x10}, {0x0f});
    command({0x06}, {});
    command({0x02, 0x00, 0x00, 0x10}, {0xf0});
    check(chip.type() == SaveType::eeprom_128k && chip.data()[0x10] == 0xf0, "puis WR : EEPROM de 128 Kio, qui écrit sans effacer");
    chip.set_type(SaveType::automatic);
    command({0x03, 0x00, 0x00, 0x10}, {0x00});
    command({0x06}, {});
    command({0x0a, 0x00, 0x00, 0x10}, {0x3c});
    check(chip.type() == SaveType::flash_1m && chip.data()[0x10] == 0x3c, "puis PW : la Flash se confirme");
    chip.set_type(SaveType::automatic);
    command({0x06}, {});
    command({0xd8, 0x01, 0x00, 0x00}, {});
    check(chip.type() == SaveType::flash_1m, "effacement sans données : décidé en fin de commande, Flash");
    chip.set_type(SaveType::automatic);
    check(chip.load(std::vector<std::uint8_t>(512 * 1024, 0)) && chip.type() == SaveType::flash_512k,
          "fichier de 512 Kio sur une puce inconnue : Flash de 512 Kio");
}

// SPI de la cartouche : AUXSPIDATA échange un octet ; bit 6 maintient la puce
// sélectionnée ; le bus reste occupé 64 << débit cycles.
void card_spi_test() {
    GameCard card;
    card.insert(std::vector<std::uint8_t>(0x1000), 0xc2U);
    card.save().set_type(SaveType::eeprom_8k);
    Scheduler scheduler;
    const auto send = [&](std::uint8_t byte, bool hold, Cycles now) {
        card.write(0x00, 0xa000U | (hold ? 0x40U : 0U) | 1U, 0xffffU, now, scheduler);  // slot, SPI, débit 1
        card.write(0x00, std::uint32_t{byte} << 16U, 0x00ff'0000U, now, scheduler);
        return static_cast<std::uint8_t>(card.read(0x00, now + 128) >> 16U);
    };
    send(0x06, false, 0);
    send(0x02, true, 0);
    send(0x00, true, 0);
    send(0x10, true, 0);
    send(0x42, false, 0);
    check(card.save().data()[0x10] == 0x42, "écriture par AUXSPIDATA, puce relâchée après le dernier octet");
    check((card.read(0x00, 127) & 0x80U) != 0 && (card.read(0x00, 128) & 0x80U) == 0, "occupé 128 cycles au débit 1");
    send(0x03, true, 1000);
    send(0x00, true, 1000);
    send(0x10, true, 1000);
    check(send(0x00, false, 1000) == 0x42, "lecture par AUXSPIDATA");
    card.write(0x00, 0xc000U, 0xffffU, 2000, scheduler);  // mode ROM
    card.write(0x00, 0x05U << 16U, 0x00ff'0000U, 2000, scheduler);
    check(((card.read(0x00, 3000) >> 16U) & 0xffU) == 0x42 && (card.read(0x00, 2000) & 0x80U) == 0,
          "hors du mode SPI : rien n'est échangé, AUXSPIDATA garde le dernier octet");
}

void extended_palettes_test() {
    Vram vram;
    Memory memory;
    vram.set_control(0, 0x81);  // A : fonds
    vram.set_control(1, 0x82);  // B : objets
    store(&memory.palette[0], 0x001f, 2);  // fond : rouge
    store(&memory.palette[2], 0x03e0, 2);  // palette standard, couleur 1 : vert
    // E en LCDC (0x06880000) le temps d'écrire, puis en palettes étendues des fonds.
    vram.set_control(4, 0x80);
    vram.write(Vram::lcdc, 0x80000 + 2, 0x7c00, 2);                   // emplacement 0, palette 0, couleur 1
    vram.write(Vram::lcdc, 0x80000 + (2 * 256 + 1) * 2, 0x7fff, 2);  // emplacement 0, palette 2, couleur 1
    vram.write(Vram::lcdc, 0x80000 + 0x4000 + 2, 0x0210, 2);          // emplacement 2, palette 0, couleur 1
    vram.set_control(4, 0x84);
    check(vram.read(Vram::lcdc, 0x80000 + 2, 2) == 0, "E en palettes étendues : hors de l'espace du CPU");
    // BG0 en 256 couleurs : tuile 1 pleine de l'indice 1 ; carte : palette 0, puis palette 2.
    for (unsigned byte = 0; byte < 64; ++byte) vram.write(Vram::bg_a, 64 + byte, 1, 1);
    vram.write(Vram::bg_a, 0x800, 0x0001, 2);
    vram.write(Vram::bg_a, 0x802, 0x2001, 2);
    Engine2d engine{true};
    engine.write(0x08, (1U << 8U) | 0x80U, 0xffffU);  // BG0CNT : 256 couleurs, carte au bloc 1
    engine.write(0x00, 0x4001'0100U, all);            // palettes étendues des fonds, BG0
    std::array<std::uint16_t, 256> line{};
    engine.render_line(0, vram, memory, line);
    check(line[0] == 0x7c00 && line[8] == 0x7fff && line[16] == 0x001f, "fond texte : palettes étendues 0 et 2");
    engine.write(0x08, (1U << 8U) | 0x80U | 0x2000U, 0xffffU);
    engine.render_line(0, vram, memory, line);
    check(line[0] == 0x0210, "BG0 sur l'emplacement 2 (BGxCNT bit 13)");
    engine.write(0x00, 0x0001'0100U, all);
    engine.render_line(0, vram, memory, line);
    check(line[0] == 0x03e0 && line[8] == 0x03e0, "sans DISPCNT bit 30 : palette standard");
    // F, OFS = 1 : emplacements 2 et 3, à la place de E.
    vram.set_control(4, 0);
    vram.set_control(5, 0x80);  // F en LCDC : 0x06890000
    vram.write(Vram::lcdc, 0x90000 + 2, 0x1234, 2);
    vram.write(Vram::lcdc, 0x90000 + 0x2000 + 2, 0x4210, 2);
    vram.set_control(5, 0x84 | (1U << 3U));
    engine.write(0x00, 0x4001'0100U, all);
    engine.render_line(0, vram, memory, line);
    check(line[0] == 0x1234, "F, OFS = 1 : emplacement 2");
    // BG3 étendu (mode 5, carte de 16 bits) : emplacement 3.
    vram.write(Vram::bg_a, 0x1000, 0x0001, 2);
    engine.write(0x0c, (2U << 8U) << 16U, 0xffff'0000U);  // BG3CNT : carte au bloc 2, 128 × 128
    engine.write(0x30, 0x100, 0xffffU);                   // PA = 1
    engine.write(0x34, 0x100U << 16U, 0xffff'0000U);      // PD = 1
    engine.write(0x00, 0x4001'0805U, all);
    engine.render_line(0, vram, memory, line);
    check(line[0] == 0x4210, "fond étendu : emplacement 3");
    // Objet de 256 couleurs, palette 3, sur la palette étendue des objets (G).
    vram.set_control(6, 0x80);  // G en LCDC : 0x06894000
    vram.write(Vram::lcdc, 0x94000 + (3 * 256 + 1) * 2, 0x5555, 2);
    vram.set_control(6, 0x85);
    store(&memory.palette[0x200 + 2], 0x03ff, 2);
    for (unsigned byte = 0; byte < 64; ++byte) vram.write(Vram::obj_a, byte, 1, 1);
    store(&memory.oam[0], 10U | 0x2000U, 2);  // y = 10, 256 couleurs
    store(&memory.oam[2], 20, 2);
    store(&memory.oam[4], 0x3000U, 2);  // tuile 0, palette 3
    engine.write(0x00, 0x8001'1010U, all);  // palettes étendues des objets, objets en 1D
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x5555, "objet de 256 couleurs : palette étendue 3");
    engine.write(0x00, 0x0001'1010U, all);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x03ff, "sans DISPCNT bit 31 : palette standard des objets");
}

void bitmap_objects_test() {
    Vram vram;
    Memory memory;
    vram.set_control(1, 0x82);  // B : objets du moteur A
    store(&memory.palette[0], 0x001f, 2);  // fond : rouge
    const auto object = [&](std::uint32_t a0, std::uint32_t a1, std::uint32_t a2) {
        store(&memory.oam[0], a0, 2);
        store(&memory.oam[2], a1, 2);
        store(&memory.oam[4], a2, 2);
    };
    // 2D de 128 points : le point (x, y) de la tuile t est à (t & 15) × 16 + (t & ~15) × 128 + (y × 128 + x) × 2.
    vram.write(Vram::obj_a, 0, 0x8000 | 0x7c00, 2);
    vram.write(Vram::obj_a, 3 * 2, 0x03e0, 2);  // bit 15 à 0 : invisible
    vram.write(Vram::obj_a, (128 + 2) * 2, 0x8000 | 0x03e0, 2);
    vram.write(Vram::obj_a, 0x10 + 0x800, 0x8000 | 0x7fff, 2);  // tuile 0x11
    object(10U | (3U << 10U), 20, 0);  // bitmap de 8 × 8 à (20, 10)
    Engine2d engine{true};
    engine.write(0x00, 0x0001'1000U, all);  // objets, bitmaps en 2D de 128 points
    std::array<std::uint16_t, 256> line{};
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x7c00 && line[23] == 0x001f, "objet bitmap : bit 15 = point visible");
    engine.render_line(11, vram, memory, line);
    check(line[22] == 0x03e0, "objet bitmap 2D : rangées de 128 points");
    object(10U | (3U << 10U), 20, 0x11);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x7fff, "objet bitmap 2D : tuile 0x11");
    vram.write(Vram::obj_a, 0x10 + 0x20 * 0x80, 0x8000 | 0x0210, 2);  // tuile 0x21, largeur 256
    object(10U | (3U << 10U), 20, 0x21);
    engine.write(0x00, 0x0001'1020U, all);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x0210, "objet bitmap 2D de 256 points");
    // 1D : tuile × 128 octets (× 256 avec DISPCNT bit 22), largeur source = largeur de l'objet.
    vram.write(Vram::obj_a, 2 * 128 + (8 + 4) * 2, 0x8000 | 0x1234, 2);
    object(10U | (3U << 10U), 20, 2);
    engine.write(0x00, 0x0001'1040U, all);
    engine.render_line(11, vram, memory, line);
    check(line[24] == 0x1234, "objet bitmap 1D, pas de 128 octets");
    vram.write(Vram::obj_a, 2 * 256, 0x8000 | 0x4321, 2);
    engine.write(0x00, 0x0041'1040U, all);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x4321, "objet bitmap 1D, pas de 256 octets");
    engine.write(0x00, 0x0001'1060U, all);
    engine.render_line(10, vram, memory, line);
    check(line[20] == 0x001f, "objet bitmap : combinaison réservée");
}

void mosaic_test() {
    Vram vram;
    Memory memory;
    vram.set_control(0, 0x81);  // A : fonds
    vram.set_control(1, 0x82);  // B : objets
    for (unsigned index = 1; index <= 8; ++index) {  // couleurs 1 à 8 : valeurs 1 à 8
        store(&memory.palette[index * 2], index, 2);
        store(&memory.palette[0x200 + index * 2], index, 2);
    }
    store(&memory.palette[0], 0x7c00, 2);  // fond : bleu
    // Tuile 1 du fond : le point (x, y) a l'indice 1 + ((x + y) & 7).
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; x += 2) {
            const auto low = 1U + ((x + y) & 7U);
            const auto high = 1U + ((x + 1U + y) & 7U);
            vram.write(Vram::bg_a, 32 + y * 4 + x / 2, low | (high << 4U), 1);
        }
    }
    for (unsigned entry = 0; entry < 32 * 32; ++entry) vram.write(Vram::bg_a, 0x800 + entry * 2, 1, 2);
    Engine2d engine{true};
    engine.write(0x08, (1U << 8U) | 0x40U, 0xffffU);  // BG0 : mosaïque, carte au bloc 1
    engine.write(0x00, 0x0001'0100U, all);
    std::array<std::uint16_t, 256> line{};
    engine.render_line(0, vram, memory, line);
    check(line[5] == 6, "mosaïque de 1 × 1 : sans effet");
    engine.write(0x4c, 0x33, 0xffffU);  // blocs de 4 × 4 pour les fonds
    engine.render_line(0, vram, memory, line);
    check(line[3] == 1 && line[5] == 5, "mosaïque horizontale d'un fond");
    engine.render_line(2, vram, memory, line);
    check(line[5] == 5 && line[0] == 1, "mosaïque verticale : la ligne 2 reprend la ligne 0");
    engine.render_line(5, vram, memory, line);
    check(line[1] == 5, "mosaïque verticale : la ligne 5 reprend la ligne 4");
    // BG3 bitmap de 256 couleurs (mode 5) : un seul point, en (5, 0).
    store(&memory.palette[2], 0x03e0, 2);
    vram.write(Vram::bg_a, 0x8000 + 5, 1, 1);
    engine.write(0x0c, ((2U << 8U) | 0x80U | 0x40U) << 16U, 0xffff'0000U);
    engine.write(0x30, 0x100, 0xffffU);
    engine.write(0x34, 0x100U << 16U, 0xffff'0000U);
    engine.write(0x00, 0x0001'0805U, all);
    engine.write(0x4c, 0x10, 0xffffU);  // blocs de 1 × 2
    engine.render_line(0, vram, memory, line);
    engine.render_line(1, vram, memory, line);
    check(line[5] == 0x03e0, "fond affine : la ligne 1 garde le point de référence de la ligne 0");
    engine.render_line(2, vram, memory, line);
    check(line[5] == 0x7c00, "fond affine : la ligne 2 repart de son propre point de référence");
    // Objet de 8 × 8 à (22, 10), indice 1 + x à chaque rangée ; blocs de 4 × 4 pour les objets.
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; x += 2) vram.write(Vram::obj_a, y * 4 + x / 2, (1U + x) | ((2U + x) << 4U), 1);
    }
    store(&memory.oam[0], 10U | 0x1000U, 2);  // y = 10, mosaïque
    store(&memory.oam[2], 22, 2);
    engine.write(0x00, 0x0001'1010U, all);
    engine.write(0x4c, 0x3300, 0xffffU);
    engine.render_line(11, vram, memory, line);
    check(line[24] == 0x7c00, "objet : le bloc des lignes 8 à 11 commence au-dessus de lui");
    engine.render_line(13, vram, memory, line);
    check(line[22] == 0x7c00 && line[24] == 3 && line[27] == 3 && line[28] == 7,
          "objet : grille de l'écran, bloc vide quand son coin tombe hors de l'objet");
}

} // namespace

int main() {
    scheduler_test();
    ipc_test();
    divider_test();
    square_root_test();
    timers_test();
    lcd_test();
    cp15_test();
    memory_test();
    header_test();
    vram_test();
    engine_test();
    decompression_test();
    objects_test();
    affine_backgrounds_test();
    windows_and_effects_test();
    game_card_test();
    save_memory_test();
    save_detection_test();
    card_spi_test();
    extended_palettes_test();
    bitmap_objects_test();
    mosaic_test();
    return 0;
}
