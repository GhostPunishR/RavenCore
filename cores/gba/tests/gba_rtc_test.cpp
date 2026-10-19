#include "memory/bus.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
using ravenemu::testing::expect_failure;

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
constexpr int cmd_write_status = 0x62;
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
    explicit Master(Bus& bus, bool cfru = false) : bus_(bus), cfru_(cfru) {}

    void begin() {
        bus_.write16(control_register, 1);   // registres relisibles
        if (!cfru_) bus_.write16(direction_register, 7);
        bus_.write16(data_register, 1);      // SCK haut, CS bas
        bus_.write16(data_register, 5);      // CS monte : la transaction commence
        if (cfru_) bus_.write16(direction_register, 7); // ordre du pilote CFRU
    }

    void end() {
        bus_.write16(data_register, 1);
        if (cfru_) bus_.write16(data_register, 1);
    }

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
            for (unsigned repeat = 0; repeat < (cfru_ ? 5U : 1U); ++repeat)
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
        for (unsigned repeat = 0; repeat < (cfru_ ? 3U : 1U); ++repeat)
            bus_.write16(data_register, 4 | (bit << 1)); // SCK bas, donnée posée
        bus_.write16(data_register, 4 | (bit << 1) | 1); // SCK haut : échantillonnage
    }

    Bus& bus_;
    bool cfru_;
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
 * Le choix explicite prime sur la signature Seiko et sur les indices CFRU.
 * D'autres pilotes de hacks peuvent échapper à ces critères ; le forçage reste
 * disponible dans les deux sens, et la détection reste consultable.
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
    check((bytes[4] & 0x7f) == to_bcd(expected.tm_hour), "RTC forcé : heure incorrecte");
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
    check((bytes[4] & 0x7f) == to_bcd(expected.tm_hour), "heure RTC incorrecte");
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

    check((bytes[0] & 0x7f) == to_bcd(expected.tm_hour), "heure seule RTC incorrecte");
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
    check((bytes[4] & 0x7f) == to_bcd(expected.tm_hour), "commande inversée : heure incorrecte");
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
    check(bytes[4] == 0x92, "heure relue après réglage incorrecte");
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
    check(fixture.master.transaction_read(cmd_read_status) == 0, "force-reset : registre de contrôle non remis à zéro");
    fixture.master.begin(); fixture.master.command(cmd_write_status); fixture.master.write_byte(0x40); fixture.master.end();

    const auto expected = local_time(frozen_epoch);
    fixture.master.begin();
    fixture.master.command(cmd_read_date_time);
    std::array<int, 7> bytes{};
    for (auto& value : bytes) value = fixture.master.read_byte();
    fixture.master.end();

    check(bytes[0] == to_bcd((expected.tm_year + 1900) % 100), "la remise à zéro n'a pas annulé l'écart");
    check((bytes[4] & 0x7f) == to_bcd(expected.tm_hour), "la remise à zéro n'a pas restauré l'heure hôte");
}

std::array<int, 7> read_date_time(Master& master) {
    master.begin();
    master.command(cmd_read_date_time);
    std::array<int, 7> bytes{};
    for (auto& value : bytes) value = master.read_byte();
    master.end();
    return bytes;
}

constexpr std::size_t sram_size = 32U * 1024U;

/**
 * L'heure réglée par le jeu est enregistrée à la suite de la sauvegarde, dans
 * le bloc de 16 octets de mGBA, et la sauvegarde est signalée à réécrire. Une
 * heure plus tard, une autre machine relit ce bloc : l'horloge repart de
 * l'heure réglée et a avancé avec l'appareil.
 */
void clock_setting_saved_with_battery_test() {
    Fixture source;
    check(!source.cartridge.save()->dirty(), "sauvegarde neuve marquée à réécrire");
    source.master.begin();
    source.master.command(cmd_write_date_time);
    for (const auto value : {0x30, 0x06, 0x01, 0x00, 0x08, 0x00, 0x00}) source.master.write_byte(value);
    source.master.end();
    check(source.cartridge.save()->dirty(), "heure réglée par le jeu non signalée à enregistrer");

    const auto battery = source.cartridge.export_battery();
    check(battery.size() == sram_size + 16, "réglages de l'horloge absents de la sauvegarde");
    const auto* trailer = battery.data() + sram_size;
    check(trailer[0] == 0x30 && trailer[1] == 0x06 && trailer[2] == 0x01 && trailer[4] == 0x08 && trailer[5] == 0x00 &&
              trailer[6] == 0x00,
          "date du jeu mal enregistrée");
    check(trailer[7] == 0x40, "registre d'état mal enregistré");
    std::uint64_t latch{};
    for (unsigned byte = 0; byte < 8; ++byte) latch |= std::uint64_t{trailer[8 + byte]} << (8U * byte);
    check(latch == static_cast<std::uint64_t>(frozen_epoch), "heure de l'appareil mal enregistrée");

    Fixture later{true, std::nullopt, frozen_epoch + 3600};
    later.cartridge.import_battery(battery);
    check(!later.cartridge.save()->dirty(), "sauvegarde relue marquée à réécrire");
    const auto bytes = read_date_time(later.master);
    check(bytes[0] == 0x30 && bytes[1] == 0x06 && bytes[2] == 0x01, "date réglée perdue au rechargement");
    check(bytes[4] == 0x09 && bytes[5] == 0x00, "l'horloge relue n'a pas avancé avec l'appareil");
}

/** Une horloge jamais réglée suit l'appareil ; réécrire le même état ne change rien. */
void unchanged_clock_follows_device_test() {
    Fixture source;
    source.master.begin();
    source.master.command(cmd_write_status);
    source.master.write_byte(0x40);
    source.master.end();
    check(!source.cartridge.save()->dirty(), "état inchangé signalé à enregistrer");

    constexpr std::int64_t next_day = frozen_epoch + 86'400 + 754;
    Fixture later{true, std::nullopt, next_day};
    later.cartridge.import_battery(source.cartridge.export_battery());
    const auto expected = local_time(next_day);
    const auto bytes = read_date_time(later.master);
    check(bytes[2] == to_bcd(expected.tm_mday) && (bytes[4] & 0x7f) == to_bcd(expected.tm_hour) &&
              bytes[5] == to_bcd(expected.tm_min) && bytes[6] == to_bcd(expected.tm_sec),
          "horloge jamais réglée décalée après rechargement");
}

/**
 * Une sauvegarde sans bloc (ancienne version, autre émulateur) ou avec un bloc
 * vierge laisse l'horloge à l'heure de l'appareil. FlashGBX remplit l'octet
 * d'état de 0x01 : l'heure est reprise, pas cet octet.
 */
void missing_or_blank_clock_settings_test() {
    const auto expected = local_time(frozen_epoch);
    const auto shows_device_time = [&](const std::vector<std::uint8_t>& battery) {
        Fixture fixture;
        fixture.cartridge.import_battery(battery);
        const auto bytes = read_date_time(fixture.master);
        return (bytes[4] & 0x7f) == to_bcd(expected.tm_hour) && bytes[5] == to_bcd(expected.tm_min) &&
               fixture.master.transaction_read(cmd_read_status) == 0x40;
    };
    check(shows_device_time(std::vector<std::uint8_t>(sram_size, 0)), "sauvegarde sans bloc : horloge décalée");
    check(shows_device_time(std::vector<std::uint8_t>(sram_size + 16, 0xff)), "bloc vierge accepté");

    auto flashgbx = Fixture{}.cartridge.export_battery();
    flashgbx[sram_size + 4] = 0x23;  // 23 h
    flashgbx[sram_size + 7] = 0x01;
    Fixture fixture;
    fixture.cartridge.import_battery(flashgbx);
    check(read_date_time(fixture.master)[4] == 0xa3, "heure d'un bloc FlashGBX ignorée");
    check(fixture.master.transaction_read(cmd_read_status) == 0x40, "octet de remplissage FlashGBX pris pour l'état");
}

/**
 * Au passage à l'heure d'hiver, 2 h 30 revient deux fois. Le bloc ne dit pas
 * laquelle : relu, il doit garder une horloge jamais réglée à l'heure exacte,
 * sans la décaler d'une heure pour toujours.
 */
void ambiguous_hour_keeps_device_time_test() {
#if !defined(_WIN32)
    const char* previous = std::getenv("TZ");
    const std::string saved = previous ? previous : "";
    setenv("TZ", "Europe/Paris", 1);
    tzset();
    constexpr std::int64_t summer = 1'792'888'200;  // 2026-10-25 02:30 CEST
    constexpr std::int64_t winter = summer + 3600;  // 2026-10-25 02:30 CET
    if (local_time(summer).tm_hour == 2 && local_time(winter).tm_hour == 2) {
        for (const auto& [epoch, hour_later] : {std::pair{summer, 0x02}, std::pair{winter, 0x03}}) {
            Fixture source{true, std::nullopt, epoch};
            Fixture later{true, std::nullopt, epoch + 3600};
            later.cartridge.import_battery(source.cartridge.export_battery());
            check(read_date_time(later.master)[4] == hour_later, "heure ambiguë : horloge décalée d'une heure");
        }
    }
    if (previous)
        setenv("TZ", saved.c_str(), 1);
    else
        unsetenv("TZ");
    tzset();
#endif
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

/** Sans marqueur ni pilote reconnu, le GPIO reste de la ROM, même relecture activée. */
void absent_rtc_reads_rom_test() {
    Fixture fixture{false};
    fixture.bus.write16(control_register, 1);
    const auto rom_image = rom(false);
    const auto offset = static_cast<std::size_t>(data_register & 0x01ff'ffff);
    const auto expected = rom_image[offset] | (rom_image[offset + 1] << 8);
    check(fixture.bus.read16(data_register) == expected, "cartouche sans RTC : lecture GPIO inattendue");
}

/**
 * Un état restauré au milieu d'une lecture série doit reprendre le dialogue
 * à l'identique ; un état dont l'index d'octet sort du tampon de transfert
 * (sept octets) est refusé, faute de quoi le prochain front d'horloge lisait
 * ou écrivait hors du tableau.
 */
void mid_transfer_state_validation_test() {
    Fixture source;
    source.master.begin();
    source.master.command(cmd_read_date_time);
    static_cast<void>(source.master.read_byte());
    static_cast<void>(source.master.read_byte());
    const auto state = source.cartridge.gpio()->export_state();

    Fixture restored;
    restored.cartridge.gpio()->import_state(state);
    for (int index = 2; index < 7; ++index) {
        check(restored.master.read_byte() == source.master.read_byte(),
              "lecture RTC reprise depuis un état différente de l'original");
    }

    // Indices GPIO : 0-2 broches du port, puis l'état RTC à partir de 3.
    constexpr std::size_t rtc = 3;
    for (const auto& [field, value] : std::array<std::pair<std::size_t, std::int32_t>, 10>{{
             {rtc + 8, 7}, {rtc + 8, 100'000}, {rtc + 8, -1},   // byte_index_
             {rtc + 9, 8},                                       // byte_count_
             {rtc + 4, 4},                                       // phase_
             {rtc + 6, 8},                                       // bit_index_
             {rtc + 0, 2},                                       // SCK
             {rtc + 5, 0x100},                                   // shift_
             {rtc + 13, 0x100},                                  // octet de transfert
             {rtc + 11, 0x7fff'ffff}}}) {                        // décalage d'horloge
        auto corrupt = state;
        corrupt[field] = value;
        Fixture target;
        expect_failure<SaveStateError>(
            [&] { target.cartridge.gpio()->import_state(corrupt); },
            "état RTC GBA hors limites accepté");
    }
}

/** Broches et directions GPIO sont des registres 4 bits. */
void gpio_pin_state_validation_test() {
    Fixture source;
    source.master.begin();
    const auto state = source.cartridge.gpio()->export_state();
    Fixture{}.cartridge.gpio()->import_state(state);
    for (const auto& [field, value] : std::array<std::pair<std::size_t, std::int32_t>, 4>{{
             {0, 16}, {0, -1}, {1, 16}, {1, -1}}}) {
        auto corrupt = state;
        corrupt[field] = value;
        Fixture target;
        expect_failure<SaveStateError>(
            [&] { target.cartridge.gpio()->import_state(corrupt); },
            "broches GPIO GBA hors limites acceptées");
    }
}

} // namespace

// Heure mutable : un même composant doit continuer à avancer, même sans
// exécuter de trames, et verrouiller ses sept octets pendant une transaction.
struct LiveFixture {
    std::int64_t now{frozen_epoch};
    RomImage image{std::make_shared<const std::vector<std::uint8_t>>(ravenemu::testing::pokemon_rtc_gba_rom())};
    Cartridge cartridge{image, std::nullopt, std::nullopt, [this] { return now; }};
    Bus bus{cartridge};
    Master master{bus, true};
};

void pokemon_driver_detection_test() {
    using ravenemu::testing::pokemon_rtc_gba_rom;
    check(RomTraits::detect(pokemon_rtc_gba_rom()).rtc, "pilote Pokémon sans SIIRTC non détecté");
    check(!RomTraits::detect(pokemon_rtc_gba_rom(false)).rtc, "identité de Rouge Feu original prise pour une RTC");
    auto image = pokemon_rtc_gba_rom();
    std::fill_n(image.begin() + 0x108, 20, 0);
    check(!RomTraits::detect(image).rtc, "BPRE et nombres GPIO sans identité interne suffisent à activer RTC");
    image = pokemon_rtc_gba_rom(); image[0x2c0] = 0;
    check(!RomTraits::detect(image).rtc, "pilote incomplet accepté sans registre de contrôle");
    image[0x900] = 0xc8; image[0x903] = 8;
    check(!RomTraits::detect(image).rtc, "références GPIO sans proximité prises pour un pilote");
    image = pokemon_rtc_gba_rom();
    std::fill_n(image.begin() + 0xac, 4, 'X');
    check(RomTraits::detect(image).rtc, "détection dépend du code BPRE au lieu du pilote");
    for (std::size_t size = 0; size < 0x130; ++size)
        check(!RomTraits::detect(std::span{image}.first(size)).rtc, "en-tête tronqué accepté");
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(pokemon_rtc_gba_rom());
    Cartridge disabled(shared, std::nullopt, false, [] { return frozen_epoch; });
    check(disabled.rtc_detected() && !disabled.rtc(), "forçage off perdu sur un hack détecté");
    LiveFixture live;
    check(live.cartridge.rtc() && live.cartridge.save_type() == GbaSaveType::flash_128k,
          "RTC automatique ou Flash 128 Kio perdue");
    check(live.master.transaction_read(cmd_read_status) == 0x40, "probe CFRU : état 24 h absent");
    const auto data = read_date_time(live.master);
    const auto expected = local_time(live.now);
    check(data[0] == to_bcd((expected.tm_year + 1900) % 100) && data[1] == to_bcd(expected.tm_mon + 1) &&
              data[2] == to_bcd(expected.tm_mday) && data[3] == expected.tm_wday &&
              (data[4] & 0x7f) == to_bcd(expected.tm_hour) && data[5] == to_bcd(expected.tm_min) && data[6] == to_bcd(expected.tm_sec),
          "pilote CFRU : date complète incorrecte sans forçage");
}

void gpio_edges_and_direction_test() {
    Fixture f;
    f.master.begin();
    // Un front SCK produit par DIR, plutôt que DATA, doit être pris en compte.
    for (int bit = 7; bit >= 0; --bit) {
        f.bus.write16(direction_register, 6);
        f.bus.write16(data_register, 5 | (((cmd_read_status >> bit) & 1) << 1));
        f.bus.write16(direction_register, 7);
    }
    check(f.master.read_byte() == 0x40, "transitions SCK via GPIO direction ignorées");
    f.master.end();
    check((f.bus.read16(data_register) & 2) != 0, "SIO doit être haut au repos");
    // Des écritures STRB ne doivent ni activer le port, ni commander la RTC.
    f.bus.write16(control_register, 0); f.bus.write8(control_register, 1);
    check(!f.cartridge.gpio()->readable(), "STRB a activé le GPIO");
    f.bus.write16(control_register, 1); f.bus.write16(direction_register, 5);
    f.bus.write8(direction_register, 7);
    check(f.bus.read16(direction_register) == 5, "STRB a modifié les directions");
    f.master.begin(); f.master.command(cmd_read_date_time);
    f.bus.write16(direction_register, 3); // CS relâché : doit tomber et annuler la lecture
    f.bus.write16(direction_register, 7); // CS remonte avec le latch conservé
    f.master.command(cmd_read_status);
    check(f.master.read_byte() == 0x40, "relâcher CS n'a pas interrompu la transaction");
    f.master.end();
    // La sortie de la RTC prime sur un bit SIO écrit alors que la broche est en entrée.
    f.master.begin(); f.master.command(cmd_read_status); f.bus.write16(direction_register, 5);
    int status = 0;
    for (int bit = 0; bit < 8; ++bit) {
        f.bus.write16(data_register, 6); f.bus.write16(data_register, 7);
        const auto pins = f.bus.read16(data_register);
        status |= ((pins >> 1) & 1) << bit;
        check((pins & 5) == 5, "sorties SCK/CS relues incorrectement");
    }
    check(status == 0x40, "latch de sortie SIO a écrasé les données d'entrée");
    f.master.end();
    f.bus.write16(direction_register, 0xffff);
    check(f.bus.read16(direction_register) == 15 && f.bus.read16(0x0a0000c6) == 15 &&
              (u32(f.bus.read32(data_register)) >> 16U) == 15, "masques et miroirs des registres GPIO");
}

void invalid_commands_and_dates_test() {
    Fixture f;
    f.master.begin(); f.master.command(cmd_write_date_time);
    for (const auto byte : {0x30,0x02,0x28,0,0x12,0x34,0x56}) f.master.write_byte(byte);
    f.master.end();
    const auto before = f.cartridge.rtc()->settings();
    for (const auto command : {0x00,0xff,0x10,0x11}) {
        f.master.begin(); f.master.command(command); f.master.end();
        check(f.cartridge.rtc()->settings().offset_seconds == before.offset_seconds, "commande sans code fixe a changé l'heure");
    }
    for (const auto date : {std::array{0x30,0x02,0x30,0,0x12,0,0}, std::array{0x30,0x13,1,0,0x12,0,0},
                            std::array{0x30,1,1,0,0x2a,0,0}, std::array{0x30,1,1,0,0x12,0x60,0}}) {
        f.master.begin(); f.master.command(cmd_write_date_time);
        for (const auto byte : date) f.master.write_byte(byte);
        f.master.end();
        check(f.cartridge.rtc()->settings().offset_seconds == before.offset_seconds, "date invalide normalisée et appliquée");
    }
    auto battery = f.cartridge.export_battery(); battery[sram_size + 1] = 2; battery[sram_size + 2] = 0x30;
    Fixture restored; restored.cartridge.import_battery(battery);
    check(restored.cartridge.rtc()->settings().offset_seconds == 0, "bloc batterie de calendrier invalide accepté");
}

void time_formats_and_live_clock_test() {
    LiveFixture f;
    // Une date locale indépendante du fuseau de la machine exécutant le test.
    std::tm start{}; start.tm_year = 126; start.tm_mon = 11; start.tm_mday = 31;
    start.tm_hour = 23; start.tm_min = 59; start.tm_sec = 58; start.tm_isdst = -1;
    f.now = static_cast<std::int64_t>(std::mktime(&start));
    auto data = read_date_time(f.master);
    check(data[4] == 0xa3 && data[5] == 0x59 && data[6] == 0x58, "heure 24 h ou indicateur PM incorrect");
    f.master.begin(); f.master.command(cmd_write_status); f.master.write_byte(0xff); f.master.end();
    check(f.master.transaction_read(cmd_read_status) == 0x6a, "bits réservés ou état d'alimentation écrits");
    f.master.begin(); f.master.command(cmd_write_status); f.master.write_byte(0); f.master.end();
    check(f.master.transaction_read(cmd_read_status) == 0, "mode 12 h refusé");
    check(read_date_time(f.master)[4] == 0x91, "23 h doit se lire 11 PM en mode 12 h");
    auto battery = f.cartridge.export_battery();
    constexpr auto flash_size = 128U * 1024U;
    check(battery[flash_size + 4] == 0x23 && battery[flash_size + 7] == 0,
          "format batterie historique 24 h ou état 12 h modifié");
    LiveFixture restored; restored.now = f.now; restored.cartridge.import_battery(battery);
    check(read_date_time(restored.master)[4] == 0x91, "mode 12 h perdu par le bloc de 16 octets");
    f.now += 5;
    data = read_date_time(f.master);
    check(data[0] == 0x27 && data[1] == 1 && data[2] == 1 && data[4] == 0 && data[5] == 0 && data[6] == 3,
          "horloge n'a pas passé minuit et le changement d'année");
    // Midi est 00 PM dans le composant Seiko, pas 12 PM.
    f.master.begin(); f.master.command(0x66);
    for (const auto byte : {0x80,0x12,0x34}) f.master.write_byte(byte);
    f.master.end();
    check(read_date_time(f.master)[4] == 0x80, "écriture de midi 12 h / PM incorrecte");
    f.master.begin(); f.master.command(cmd_write_status); f.master.write_byte(0x40); f.master.end();
    data = read_date_time(f.master);
    check(data[4] == 0x92 && data[5] == 0x12 && data[6] == 0x34, "retour au format 24 h a changé l'heure");
    f.master.begin(); f.master.command(cmd_read_date_time);
    std::array<int,7> latched{};
    for (auto& byte : latched) { byte = f.master.read_byte(); f.now += 61; }
    f.master.end();
    check(latched == data && read_date_time(f.master) != data, "date non verrouillée ou horloge n'avançant plus");
    LiveFixture leap;
    leap.master.begin(); leap.master.command(cmd_write_date_time);
    for (const auto byte : {0x28,2,0x29,2,0x23,0x59,0x58}) leap.master.write_byte(byte);
    leap.master.end();
    data = read_date_time(leap.master);
    check(data[0] == 0x28 && data[1] == 2 && data[2] == 0x29 && data[3] == 2, "date bissextile ou jour de semaine incorrect");
    leap.now += 3; data = read_date_time(leap.master);
    check(data[1] == 3 && data[2] == 1 && data[3] == 3 && data[6] == 1, "fin février bissextile incorrecte");
}

void daylight_write_and_reload_test() {
#if !defined(_WIN32)
    const char* old = std::getenv("TZ"); const std::string saved = old ? old : ""; const bool had = old != nullptr;
    for (const auto zone : {"UTC0", "EST5EDT,M3.2.0/2,M11.1.0/2", "LHST-10:30LHDT-11,M10.1.0/2,M4.1.0/2"}) {
        setenv("TZ",zone,1); tzset();
        for (const auto daylight : {0,1}) {
            std::tm start{}; start.tm_year = 126; start.tm_mon = zone[0] == 'L' ? 3 : 10;
            start.tm_mday = zone[0] == 'L' ? 5 : 1; start.tm_hour = 1; start.tm_min = 45; start.tm_isdst = daylight;
            LiveFixture f; f.now = static_cast<std::int64_t>(std::mktime(&start));
            const auto initial = read_date_time(f.master);
            f.master.begin(); f.master.command(cmd_write_date_time);
            for (const auto byte : initial) f.master.write_byte(byte);
            f.master.end();
            check(f.cartridge.rtc()->settings().offset_seconds == 0, "écriture d'une heure ambiguë a introduit un décalage");
            LiveFixture later; later.now = f.now + 1800; later.cartridge.import_battery(f.cartridge.export_battery());
            check(later.cartridge.rtc()->settings().offset_seconds == 0 &&
                      (read_date_time(later.master)[4] & 0x7f) == to_bcd(local_time(later.now).tm_hour),
                  "persistance décalée par le fuseau ou un changement DST de trente minutes");
        }
        LiveFixture season;
        std::tm summer{}; summer.tm_year = 126; summer.tm_mon = 6; summer.tm_mday = 1;
        summer.tm_hour = 12; summer.tm_isdst = -1;
        season.now = static_cast<std::int64_t>(std::mktime(&summer));
        season.master.begin(); season.master.command(cmd_write_date_time);
        for (const auto byte : {0x26,1,1,4,0x12,0x34,0x56}) season.master.write_byte(byte);
        season.master.end();
        const auto winter = read_date_time(season.master);
        check(winter[1] == 1 && winter[2] == 1 && winter[4] == 0x92 && winter[5] == 0x34,
              "écriture d'une date d'une autre saison a décalé l'heure");
    }
    if (had) setenv("TZ",saved.c_str(),1); else unsetenv("TZ");
    tzset();
#endif
}

} // namespace ravenemu::gba::testing

int main() {
    using namespace ravenemu::gba::testing;
    pokemon_driver_detection_test();
    gpio_edges_and_direction_test();
    invalid_commands_and_dates_test();
    time_formats_and_live_clock_test();
    daylight_write_and_reload_test();
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
    mid_transfer_state_validation_test();
    gpio_pin_state_validation_test();
    clock_setting_saved_with_battery_test();
    unchanged_clock_follows_device_test();
    missing_or_blank_clock_settings_test();
    ambiguous_hour_keeps_device_time_test();
    return 0;
}
