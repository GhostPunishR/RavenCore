#include "machine/scheduler.hpp"
#include "rtc/real_time_clock.hpp"

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

} // namespace

int main() {
    date_time_test();
    registers_test();
    return 0;
}
