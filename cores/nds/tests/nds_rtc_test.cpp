#include "interrupts/interrupt_controller.hpp"
#include "machine/scheduler.hpp"
#include "rtc/real_time_clock.hpp"
#include "rtc/rtc_interrupt.hpp"

#include "check.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

// Horloge temps réel, pilotée bit par bit comme le décrit GBATEK (« DS
// Real-Time Clock (RTC) ») : CS levé, commande puis paramètres, poids faible
// en tête, un bit par front montant de /SCK.
namespace {

using namespace ravenemu::nds;
using ravenemu::testing::check;

constexpr Cycles second = system_clock_hz;

class Wire {
public:
    explicit Wire(RealTimeClock& rtc) : rtc_(rtc) {}

    // Une commande et ses paramètres : `write` les envoie, sinon `count`
    // octets sont lus. `msb_first` retourne l'octet de commande.
    std::vector<std::uint8_t> command(std::uint8_t command, std::initializer_list<std::uint8_t> write, unsigned count,
                                      Cycles now, bool msb_first = false) {
        set(0x72, now);  // CS bas, /SCK haut, lignes en sortie
        set(0x76, now);  // CS haut
        send(msb_first ? reverse(command) : command, now);
        for (const auto byte : write) send(byte, now);
        std::vector<std::uint8_t> bytes;
        for (unsigned index = 0; index < count; ++index) bytes.push_back(receive(now));
        set(0x72, now);  // CS bas : fin
        return bytes;
    }

private:
    static std::uint8_t reverse(std::uint8_t value) {
        std::uint8_t reversed = 0;
        for (unsigned bit = 0; bit < 8; ++bit) reversed = static_cast<std::uint8_t>(reversed | (((std::uint32_t{value} >> bit) & 1U) << (7U - bit)));
        return reversed;
    }
    void set(std::uint32_t lines, Cycles now) { rtc_.write(lines, 0xffffU, now); }
    void send(std::uint8_t value, Cycles now) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto data = (std::uint32_t{value} >> bit) & 1U;
            set(0x74U | data, now);  // /SCK bas, donnée posée
            set(0x76U | data, now);  // front montant : la puce lit
        }
    }
    std::uint8_t receive(Cycles now) {
        std::uint8_t value = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            set(0x64, now);  // /SCK bas, donnée en entrée : la puce sort le bit
            set(0x66, now);
            value = static_cast<std::uint8_t>(value | ((rtc_.read() & 1U) << bit));
        }
        return value;
    }

    RealTimeClock& rtc_;
};

// Commandes poids faible en tête : 0110b, registre en bits 4-6, lecture au bit 7.
constexpr std::uint8_t read_command(unsigned reg) { return static_cast<std::uint8_t>(0x86U | (reg << 4U)); }
constexpr std::uint8_t write_command(unsigned reg) { return static_cast<std::uint8_t>(0x06U | (reg << 4U)); }

void date_time_test() {
    RealTimeClock rtc;
    Wire wire{rtc};
    check(wire.command(read_command(0), {}, 1, 0) == std::vector<std::uint8_t>{0x02}, "état 1 au démarrage : mode 24 h, sans drapeau");
    rtc.set_date_time({2026, 11, 27, 21, 30, 45}, 0);
    check(wire.command(read_command(2), {}, 7, 0) == std::vector<std::uint8_t>{0x26, 0x11, 0x27, 0x04, 0x61, 0x30, 0x45},
          "date et heure en BCD : vendredi (4, 0 = lundi), 21 h avec le bit de l'après-midi");
    check(wire.command(read_command(6), {}, 3, 75 * second) == std::vector<std::uint8_t>{0x61, 0x32, 0x00},
          "l'heure avance avec le temps émulé : 75 s plus tard");
    check(wire.command(read_command(6), {}, 3, 3 * 3600 * second) == std::vector<std::uint8_t>{0x00, 0x30, 0x45} &&
              wire.command(read_command(2), {}, 3, 3 * 3600 * second) == std::vector<std::uint8_t>{0x26, 0x11, 0x28},
          "minuit passé : le lendemain");
    check(wire.command(read_command(6), {}, 3, 0, true) == std::vector<std::uint8_t>{0x61, 0x30, 0x45},
          "commande envoyée poids fort en tête");
    // Écriture de la date : 29 février 2028 à 23:59:58, un mardi (1).
    wire.command(write_command(2), {0x28, 0x02, 0x29, 0x01, 0x23, 0x59, 0x58}, 0, 10 * second);
    const auto date = rtc.date_time(11 * second);
    check(date.year == 2028 && date.month == 2 && date.day == 29 && date.hour == 23 && date.minute == 59 && date.second == 59,
          "date écrite par le jeu, année bissextile : une seconde plus tard");
    check(wire.command(read_command(2), {}, 7, 14 * second) == std::vector<std::uint8_t>{0x28, 0x03, 0x01, 0x02, 0x00, 0x00, 0x02},
          "1er mars 2028 après le 29 février ; le jour de la semaine suit celui écrit");
}

void registers_test() {
    RealTimeClock rtc;
    Wire wire{rtc};
    rtc.set_date_time({2026, 1, 2, 15, 4, 5}, 0);
    wire.command(write_command(0), {0x00}, 0, 0);  // mode 12 h
    check(wire.command(read_command(6), {}, 3, 0) == std::vector<std::uint8_t>{0x43, 0x04, 0x05}, "mode 12 h : 15 h = 3 h, après-midi");
    wire.command(write_command(6), {0x40, 0x00, 0x00}, 0, 0);  // midi en mode 12 h : 00h et après-midi
    check(rtc.date_time(0).hour == 12, "mode 12 h : midi s'écrit 00h avec le bit de l'après-midi");
    wire.command(write_command(7), {0x5a}, 0, 0);
    wire.command(write_command(4), {0x44}, 0, 0);
    check(wire.command(read_command(7), {}, 1, 0) == std::vector<std::uint8_t>{0x5a} &&
              wire.command(read_command(4), {}, 1, 0) == std::vector<std::uint8_t>{0x44},
          "registre libre et état 2");
    wire.command(write_command(1), {0x81, 0x92, 0x15}, 0, 0);  // état 2 bit 2 : alarme 1 sur 3 octets
    check(wire.command(read_command(1), {}, 3, 0) == std::vector<std::uint8_t>{0x81, 0x92, 0x15}, "alarme 1");
    wire.command(write_command(0), {0x01}, 0, 5 * second);  // remise à zéro
    check(wire.command(read_command(0), {}, 1, 5 * second) == std::vector<std::uint8_t>{0x00} &&
              wire.command(read_command(2), {}, 7, 5 * second) == std::vector<std::uint8_t>{0x00, 0x01, 0x01, 0x05, 0x00, 0x00, 0x00},
          "remise à zéro : registres à 0, 1er janvier 2000 (un samedi) à 0 h");
    check(wire.command(0x55, {}, 1, 0) == std::vector<std::uint8_t>{0x00}, "commande sans le code fixe : ignorée");
}

// La broche /INT après avoir fait passer le temps jusqu'à `now`.
bool line(RealTimeClock& rtc, Cycles now) {
    rtc.update(now);
    return rtc.interrupt_line(now);
}

// INT1 : fréquences, minute en front, minute à 30 s et 7,9 ms, alarme 1 ;
// INT2 : alarme 2 ; drapeaux de l'état 1.
void interrupts_test() {
    RealTimeClock rtc;
    Wire wire{rtc};
    rtc.set_date_time({2026, 11, 27, 21, 30, 58}, 0);
    check(line(rtc, 0) && rtc.next_change(0) == Scheduler::never, "sans interruption : broche haute, rien à attendre");

    wire.command(write_command(4), {0x02}, 0, 0);  // chaque minute, en front
    check(rtc.next_change(0) == 2 * second && line(rtc, 2 * second - 1), "front : attendu au début de la minute");
    check(!line(rtc, 2 * second) && !line(rtc, 10 * second), "front : broche basse dès 21:31:00, jusqu'à la lecture de l'état 1");
    check(wire.command(read_command(0), {}, 1, 10 * second) == std::vector<std::uint8_t>{0x12} && line(rtc, 10 * second) &&
              wire.command(read_command(0), {}, 1, 10 * second) == std::vector<std::uint8_t>{0x02},
          "état 1 : drapeau d'INT1, effacé à la lecture, qui relâche la broche");

    wire.command(write_command(4), {0x01}, 0, 0);    // fréquences
    wire.command(write_command(1), {0x03}, 0, 0);    // 1 et 2 Hz
    check(!line(rtc, second / 10) && !line(rtc, second * 3 / 10) && !line(rtc, second * 6 / 10) && line(rtc, second * 8 / 10),
          "1 et 2 Hz : basse si l'une des ondes l'est, haute le dernier quart de seconde");
    check(rtc.next_change(second / 10) == (second + 3) / 4, "fréquences : prochain front d'onde, au quart de seconde");

    wire.command(write_command(4), {0x03}, 0, 0);  // minute, basse 30 s
    check(!line(rtc, 12 * second) && line(rtc, 32 * second + 1) && rtc.next_change(12 * second) == 32 * second,
          "minute à 30 s : basse aux secondes 0 à 29 (21:31:10), haute ensuite (21:31:30)");
    wire.command(write_command(4), {0x07}, 0, 0);  // minute, basse 7,9 ms
    constexpr Cycles pulse = system_clock_hz * 79 / 10'000;
    check(!line(rtc, 62 * second) && line(rtc, 62 * second + pulse) && rtc.next_change(62 * second) == 62 * second + pulse,
          "minute à 7,9 ms : basse au début de 21:32:00, 7,9 ms");
    wire.command(write_command(4), {0x08}, 0, 0);  // 32 kHz
    check(line(rtc, 62 * second) && rtc.next_change(62 * second) == Scheduler::never, "32 kHz : pas modélisée, broche haute");

    // Alarme 1 à 21:33 (heure et minute comparées, jour ignoré), le jour
    // qu'on veut : en mode 24 h, l'heure garde son bit d'après-midi.
    wire.command(write_command(4), {0x04}, 0, 62 * second);
    wire.command(write_command(1), {0x00, 0xe1, 0xb3}, 0, 62 * second);
    check(line(rtc, 64 * second) && rtc.next_change(64 * second) == 122 * second, "alarme 1 : rien à 21:32");
    check(!line(rtc, 122 * second) && !line(rtc, 150 * second), "alarme 1 : broche basse à 21:33:00, jusqu'à la lecture");
    check(wire.command(read_command(0), {}, 1, 150 * second) == std::vector<std::uint8_t>{0x12} && line(rtc, 150 * second),
          "alarme 1 : drapeau d'INT1, broche relâchée");
    check(line(rtc, 182 * second), "alarme 1 : pas à 21:34");

    // Alarme 2 chaque minute (rien de comparé), INT1 coupée.
    wire.command(write_command(4), {0x40}, 0, 182 * second);
    wire.command(write_command(5), {0x00, 0x00, 0x00}, 0, 182 * second);
    check(rtc.next_change(182 * second) == 242 * second && !line(rtc, 242 * second) &&
              wire.command(read_command(0), {}, 1, 242 * second) == std::vector<std::uint8_t>{0x22} && line(rtc, 242 * second),
          "alarme 2 : chaque minute, drapeau d'INT2");

    rtc.set_date_time({2026, 11, 27, 21, 40, 0}, 242 * second);
    check(line(rtc, 242 * second + 1), "date réglée : la minute de départ ne compte pas comme un début de minute");

    wire.command(write_command(4), {0x06}, 0, 243 * second);  // l'autre codage du front, INT2 coupée
    check(wire.command(read_command(0), {}, 1, 302 * second) == std::vector<std::uint8_t>{0x12},
          "0110b : front aussi ; l'horloge rattrape le temps avant de répondre (21:41:00)");
    check(!line(rtc, 362 * second), "front de 21:42:00 levé");
    wire.command(write_command(4), {0x04}, 0, 362 * second);  // alarme 1 de 21:33, sans correspondance
    check(line(rtc, 362 * second), "un autre mode d'INT1 relâche la broche");
}

// /INT sur SI : IRQ 7 au front descendant, en mode général, SI en entrée et
// son interruption permise ; le prochain rendez-vous dans l'ordonnanceur.
void rcnt_test() {
    RealTimeClock rtc;
    Wire wire{rtc};
    Scheduler scheduler;
    InterruptController irq7{irq::arm7_bits};
    RtcInterrupt interrupt;
    rtc.set_date_time({2026, 11, 27, 21, 30, 58}, 0);
    wire.command(write_command(4), {0x02}, 0, 0);
    interrupt.check(rtc, scheduler, irq7, 0);
    check(interrupt.read_rcnt(rtc, 0) == 4U && !scheduler.take_due(2 * second - 1) && scheduler.take_due(2 * second) == Event::rtc,
          "SI haut (bit 2) ; rendez-vous au début de la minute");
    interrupt.check(rtc, scheduler, irq7, 2 * second);
    check(interrupt.read_rcnt(rtc, 2 * second) == 0 && (irq7.flags() & (1U << irq::rtc)) == 0, "SI bas, sans IRQ : RCNT ne la permet pas");

    wire.command(read_command(0), {}, 1, 3 * second);
    interrupt.check(rtc, scheduler, irq7, 3 * second);
    interrupt.write_rcnt(0x8100, 0xffff);
    interrupt.check(rtc, scheduler, irq7, 62 * second);
    check((irq7.flags() & (1U << irq::rtc)) != 0 && interrupt.read_rcnt(rtc, 62 * second) == 0x8100,
          "mode général, interruption de SI permise : IRQ 7 au front descendant");
    irq7.write(0x214, 1U << irq::rtc, 0xffff'ffffU);  // IRQ acquittée
    interrupt.check(rtc, scheduler, irq7, 70 * second);
    check((irq7.flags() & (1U << irq::rtc)) == 0, "broche restée basse : pas de nouvel IRQ sans front");

    InterruptController quiet{irq::arm7_bits};
    wire.command(read_command(0), {}, 1, 63 * second);
    interrupt.check(rtc, scheduler, quiet, 63 * second);
    interrupt.write_rcnt(0x8144, 0xffff);  // SI en sortie, au niveau haut
    interrupt.check(rtc, scheduler, quiet, 122 * second);
    check((quiet.flags() & (1U << irq::rtc)) == 0 && interrupt.read_rcnt(rtc, 122 * second) == 0x8144,
          "SI en sortie : pas d'IRQ, RCNT relit ce qui est écrit");
    interrupt.write_rcnt(0xffff, 0xffff);
    check(interrupt.read_rcnt(rtc, 122 * second) == 0xc1ff, "RCNT : bits 9 à 13 toujours à 0");
}

} // namespace

int main() {
    date_time_test();
    registers_test();
    interrupts_test();
    rcnt_test();
    return 0;
}
