#include "cartridge/header.hpp"
#include "cpu/cp15.hpp"
#include "gpu/lcd_timing.hpp"
#include "ipc/ipc.hpp"
#include "math/divider.hpp"
#include "math/square_root.hpp"
#include "memory/memory.hpp"
#include "timers/timers.hpp"

#include "check.hpp"

#include <cstdint>
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
            lcd.on_event(scheduler, at);
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
    return 0;
}
