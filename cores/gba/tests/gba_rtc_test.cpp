#include "memory/bus.hpp"

#include "check.hpp"

#include <algorithm>
#include <ctime>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/**
 * Horloge temps réel S-3511A du cœur GBA **livré**, éprouvée par le protocole.
 *
 * Le modèle Kotlin de référence avait déjà sa suite protocolaire, mais elle ne
 * touchait pas cette implémentation-ci : le RTC et le GPIO n'apparaissaient ni
 * dans les tests de parité, ni dans la suite native. Le composant qui tourne
 * réellement sur l'appareil n'était donc vérifié par rien.
 *
 * Le pilote ci-dessous bat les broches comme le fait la bibliothèque Seiko
 * embarquée dans les jeux, et passe par le `Bus` plutôt que par le composant :
 * ce qui est mesuré est le dialogue complet, du registre GPIO jusqu'à l'octet
 * rendu au jeu, routage mémoire compris.
 */
namespace ravenemu::gba::testing {

using ravenemu::testing::check;

namespace {

constexpr std::int64_t frozen_epoch = 1'767'225'600; // 2026-01-01 00:00:00 UTC

std::tm local_time(std::int64_t epoch) {
    const auto seconds = static_cast<std::time_t>(epoch);
    std::tm result{};
#if defined(_WIN32)
    localtime_s(&result, &seconds);
#else
    localtime_r(&seconds, &result);
#endif
    return result;
}

constexpr int to_bcd(int value) { return ((value / 10) << 4) | (value % 10); }

/** ROM GBA minimale, avec ou sans le marqueur que cherche la détection. */
std::vector<std::uint8_t> rom(bool with_rtc) {
    std::vector<std::uint8_t> image(0x400, 0);
    image[0xb2] = 0x96; // marqueur GBA obligatoire
    if (with_rtc) {
        constexpr std::string_view marker = "SIIRTC_V001";
        std::copy(marker.begin(), marker.end(), image.begin() + 0x200);
    }
    return image;
}

/** Commandes telles que les émettent les jeux : `0110 <commande:3> <lecture:1>`. */
constexpr int cmd_reset = 0x60;
constexpr int cmd_read_status = 0x63;
constexpr int cmd_read_date_time = 0x65;
constexpr int cmd_write_date_time = 0x64;
constexpr int cmd_read_time = 0x67;

constexpr std::int32_t data_register = 0x0800'00c4;
constexpr std::int32_t direction_register = 0x0800'00c6;
constexpr std::int32_t control_register = 0x0800'00c8;

/** Pilote reproduisant le cadençage de la bibliothèque du jeu. */
class Master {
public:
    explicit Master(Bus& bus) : bus_(bus) {}

    void begin() {
        bus_.write16(control_register, 1);   // registres relisibles
        bus_.write16(direction_register, 7); // SCK, SIO et CS pilotés par le jeu
        bus_.write16(data_register, 1);      // SCK haut, CS bas
        bus_.write16(data_register, 5);      // CS monte : la transaction commence
    }

    void end() { bus_.write16(data_register, 1); }

    /** Octet de commande : poids fort en tête. */
    void command(int value) {
        bus_.write16(direction_register, 7);
        for (int bit = 7; bit >= 0; --bit) clock_out((value >> bit) & 1);
    }

    /** Octet écrit par le jeu : poids faible en tête. */
    void write_byte(int value) {
        bus_.write16(direction_register, 7);
        for (int bit = 0; bit < 8; ++bit) clock_out((value >> bit) & 1);
    }

    /** Octet rendu par le composant : présenté sur front descendant, relu horloge haute. */
    int read_byte() {
        bus_.write16(direction_register, 5); // SIO repasse en entrée
        int value{};
        for (int bit = 0; bit < 8; ++bit) {
            bus_.write16(data_register, 4); // SCK bas : le composant présente son bit
            bus_.write16(data_register, 5); // SCK haut : le jeu relit
            value |= ((bus_.read16(data_register) >> 1) & 1) << bit;
        }
        return value;
    }

    int transaction_read(int cmd) {
        begin();
        command(cmd);
        const auto value = read_byte();
        end();
        return value;
    }

private:
    void clock_out(int bit) {
        bus_.write16(data_register, 4 | (bit << 1));     // SCK bas, donnée posée
        bus_.write16(data_register, 4 | (bit << 1) | 1); // SCK haut : échantillonnage
    }

    Bus& bus_;
};

/** Cartouche et bus partageant une horloge figée, pour un résultat reproductible. */
struct Fixture {
    explicit Fixture(
        bool with_rtc = true,
        std::optional<bool> forced_rtc = std::nullopt,
        std::int64_t epoch = frozen_epoch
    )
        : image(std::make_shared<const std::vector<std::uint8_t>>(rom(with_rtc))),
          cartridge(image, GbaSaveType::sram, forced_rtc, [epoch] { return epoch; }),
          bus(cartridge),
          master(bus) {}

    RomImage image;
    Cartridge cartridge;
    Bus bus;
    Master master;
};

void detection_test() {
    check(Fixture{true}.cartridge.gpio() != nullptr, "marqueur SIIRTC présent : GPIO non instancié");
    check(Fixture{false}.cartridge.gpio() == nullptr, "marqueur SIIRTC absent : GPIO instancié quand même");
    check(Fixture{true}.cartridge.rtc_detected(), "marqueur présent non signalé");
    check(!Fixture{false}.cartridge.rtc_detected(), "marqueur absent signalé présent");
}

/**
 * La détection cherche la bibliothèque Seiko des cartouches d'origine ; elle ne
 * peut rien affirmer d'une ROM modifiée. Un jeu peut piloter le composant sans
 * porter la chaîne — c'est le cas des hacks qui ajoutent eux-mêmes le pilote —
 * et une ROM peut la porter sans que le matériel soit là. Le choix explicite de
 * l'appelant prime donc dans les deux sens, et la détection reste consultable.
 */
void forced_rtc_test() {
    Fixture forced_on{false, true};
    check(forced_on.cartridge.gpio() != nullptr, "RTC forcé actif : GPIO absent");
    check(!forced_on.cartridge.rtc_detected(), "RTC forcé actif : détection faussement signalée");

    Fixture forced_off{true, false};
    check(forced_off.cartridge.gpio() == nullptr, "RTC forcé inactif : GPIO présent malgré tout");
    check(forced_off.cartridge.rtc_detected(), "RTC forcé inactif : détection perdue");

    // Le forçage doit produire une horloge réellement fonctionnelle, pas un
    // composant présent mais muet : c'est tout l'objet du réglage.
    const auto expected = local_time(frozen_epoch);
    forced_on.master.begin();
    forced_on.master.command(cmd_read_date_time);
    std::array<int, 7> bytes{};
    for (auto& value : bytes) value = forced_on.master.read_byte();
    forced_on.master.end();
    check(bytes[0] == to_bcd((expected.tm_year + 1900) % 100), "RTC forcé : année incorrecte");
    check(bytes[2] == to_bcd(expected.tm_mday), "RTC forcé : jour incorrect");
    check(bytes[4] == to_bcd(expected.tm_hour), "RTC forcé : heure incorrecte");
}

void status_test() {
    Fixture fixture;
    const auto status = fixture.master.transaction_read(cmd_read_status);
    // Bit 6 à un : format 24 heures, celui que réclament les jeux concernés.
    // Bit 7 à zéro : pas de coupure d'alimentation, autrement dit « pile bonne ».
    check((status & 0x40) != 0, "le registre d'état n'annonce pas le format 24 heures");
    check((status & 0x80) == 0, "le registre d'état annonce une coupure d'alimentation");
}

void date_time_read_test() {
    Fixture fixture;
    const auto expected = local_time(frozen_epoch);

    fixture.master.begin();
    fixture.master.command(cmd_read_date_time);
    std::array<int, 7> bytes{};
    for (auto& value : bytes) value = fixture.master.read_byte();
    fixture.master.end();

    check(bytes[0] == to_bcd((expected.tm_year + 1900) % 100), "année RTC incorrecte");
    check(bytes[1] == to_bcd(expected.tm_mon + 1), "mois RTC incorrect");
    check(bytes[2] == to_bcd(expected.tm_mday), "jour RTC incorrect");
    check(bytes[3] == expected.tm_wday, "jour de la semaine RTC incorrect");
    check(bytes[4] == to_bcd(expected.tm_hour), "heure RTC incorrecte");
    check(bytes[5] == to_bcd(expected.tm_min), "minute RTC incorrecte");
    check(bytes[6] == to_bcd(expected.tm_sec), "seconde RTC incorrecte");
}

void time_only_read_test() {
    Fixture fixture;
    const auto expected = local_time(frozen_epoch);

    fixture.master.begin();
    fixture.master.command(cmd_read_time);
    std::array<int, 3> bytes{};
    for (auto& value : bytes) value = fixture.master.read_byte();
    fixture.master.end();

    check(bytes[0] == to_bcd(expected.tm_hour), "heure seule RTC incorrecte");
    check(bytes[1] == to_bcd(expected.tm_min), "minute seule RTC incorrecte");
    check(bytes[2] == to_bcd(expected.tm_sec), "seconde seule RTC incorrecte");
}

/**
 * Toutes les bibliothèques n'émettent pas l'octet de commande dans le même
 * sens. Quand le motif fixe `0110b` n'apparaît pas en tête, le composant relit
 * l'octet à l'envers avant de le rejeter — c'est ce repli qui rend lisibles les
 * jeux dont la routine est bâtie en sens inverse.
 */
void reversed_command_test() {
    Fixture fixture;
    const auto expected = local_time(frozen_epoch);

    // 0x65 émis à l'envers : le motif fixe se retrouve en queue.
    constexpr int reversed_read_date_time = 0xa6;
    fixture.master.begin();
    fixture.master.command(reversed_read_date_time);
    std::array<int, 7> bytes{};
    for (auto& value : bytes) value = fixture.master.read_byte();
    fixture.master.end();

    check(bytes[0] == to_bcd((expected.tm_year + 1900) % 100), "commande inversée : année incorrecte");
    check(bytes[1] == to_bcd(expected.tm_mon + 1), "commande inversée : mois incorrect");
    check(bytes[2] == to_bcd(expected.tm_mday), "commande inversée : jour incorrect");
    check(bytes[4] == to_bcd(expected.tm_hour), "commande inversée : heure incorrecte");
}

/**
 * Régler l'horloge depuis le jeu ne fait que déplacer un écart : l'horloge
 * continue de suivre la machine hôte. C'est ce qu'attend un joueur qui retrouve
 * sa partie le lendemain.
 */
void date_time_write_test() {
    Fixture fixture;

    fixture.master.begin();
    fixture.master.command(cmd_write_date_time);
    // Le jour de la semaine émis (ici 0x06) est délibérément faux : le composant
    // ne le reprend pas, il le recalcule à partir de la date.
    for (const auto value : {0x26, 0x03, 0x14, 0x06, 0x12, 0x30, 0x00}) {
        fixture.master.write_byte(value);
    }
    fixture.master.end();

    fixture.master.begin();
    fixture.master.command(cmd_read_date_time);
    std::array<int, 7> bytes{};
    for (auto& value : bytes) value = fixture.master.read_byte();
    fixture.master.end();

    check(bytes[0] == 0x26, "année relue après réglage incorrecte");
    check(bytes[1] == 0x03, "mois relu après réglage incorrect");
    check(bytes[2] == 0x14, "jour relu après réglage incorrect");
    check(bytes[4] == 0x12, "heure relue après réglage incorrecte");
    check(bytes[5] == 0x30, "minute relue après réglage incorrecte");

    // Le siècle ne se lit pas dans l'année BCD : 1926 et 2026 y sont
    // indiscernables. C'est le jour de la semaine, recalculé par le composant,
    // qui distingue les deux — le 14 mars 2026 est un samedi, le même jour de
    // 1926 un dimanche. Sans cette vérification, une base d'année erronée à
    // l'écriture passerait inaperçue.
    std::tm target{};
    target.tm_year = 126; // 2026
    target.tm_mon = 2;    // mars
    target.tm_mday = 14;
    target.tm_hour = 12;
    target.tm_min = 30;
    target.tm_isdst = -1;
    const auto epoch = std::mktime(&target);
    check(epoch != static_cast<std::time_t>(-1), "date de référence du test invalide");
    check(bytes[3] == local_time(static_cast<std::int64_t>(epoch)).tm_wday,
          "jour de la semaine relu après réglage incorrect : siècle erroné ?");
}

/** La remise à zéro annule l'écart réglé par le jeu et restaure l'état d'usine. */
void reset_test() {
    Fixture fixture;

    fixture.master.begin();
    fixture.master.command(cmd_write_date_time);
    for (const auto value : {0x30, 0x06, 0x01, 0x00, 0x08, 0x00, 0x00}) {
        fixture.master.write_byte(value);
    }
    fixture.master.end();

    fixture.master.begin();
    fixture.master.command(cmd_reset);
    fixture.master.end();

    const auto expected = local_time(frozen_epoch);
    fixture.master.begin();
    fixture.master.command(cmd_read_date_time);
    std::array<int, 7> bytes{};
    for (auto& value : bytes) value = fixture.master.read_byte();
    fixture.master.end();

    check(bytes[0] == to_bcd((expected.tm_year + 1900) % 100), "la remise à zéro n'a pas annulé l'écart");
    check(bytes[4] == to_bcd(expected.tm_hour), "la remise à zéro n'a pas restauré l'heure hôte");
}

/**
 * Tant que le registre de contrôle n'autorise pas la relecture, les adresses du
 * GPIO rendent les octets de la ROM. C'est ce qui permet à une cartouche sans
 * RTC de se comporter comme une ROM ordinaire à ces adresses.
 */
void write_only_until_enabled_test() {
    Fixture fixture;
    fixture.bus.write16(control_register, 0);
    const auto rom_image = rom(true);
    const auto offset = static_cast<std::size_t>(data_register & 0x01ff'ffff);
    const auto expected = rom_image[offset] | (rom_image[offset + 1] << 8);
    check(fixture.bus.read16(data_register) == expected, "GPIO relu alors que la relecture est désactivée");
}

/** Sans marqueur, les adresses GPIO restent de la ROM, même relecture activée. */
void absent_rtc_reads_rom_test() {
    Fixture fixture{false};
    fixture.bus.write16(control_register, 1);
    const auto rom_image = rom(false);
    const auto offset = static_cast<std::size_t>(data_register & 0x01ff'ffff);
    const auto expected = rom_image[offset] | (rom_image[offset + 1] << 8);
    check(fixture.bus.read16(data_register) == expected, "cartouche sans RTC : lecture GPIO inattendue");
}

} // namespace

} // namespace ravenemu::gba::testing

int main() {
    using namespace ravenemu::gba::testing;
    detection_test();
    forced_rtc_test();
    status_test();
    date_time_read_test();
    time_only_read_test();
    reversed_command_test();
    date_time_write_test();
    reset_test();
    write_only_until_enabled_test();
    absent_rtc_reads_rom_test();
    return 0;
}
