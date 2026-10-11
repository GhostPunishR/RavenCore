#include "cartridge/cartridge_factory.hpp"
#include "cartridge/huc3.hpp"
#include "machine/machine.hpp"
#include "check.hpp"

#include <memory>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

std::vector<std::uint8_t> make_huc3_rom() {
    std::vector<std::uint8_t> rom(2U * 1024U * 1024U);
    for (int bank = 0; bank < 128; ++bank) {
        const auto offset = static_cast<std::size_t>(bank * Cartridge::rom_bank_size);
        rom[offset] = static_cast<std::uint8_t>(bank);
        rom[offset + 1U] = static_cast<std::uint8_t>(bank ^ 0xa5);
    }
    rom[0x143] = 0x00; // cartouche DMG, utilisable en compatibilité CGB
    rom[0x147] = 0xfe;
    rom[0x148] = 0x06;
    rom[0x149] = 0x00; // HuC3 porte malgré tout 32 Kio de SRAM
    return rom;
}

RomImage make_huc3_image() {
    return std::make_shared<const std::vector<std::uint8_t>>(make_huc3_rom());
}

std::unique_ptr<Cartridge> create_huc3(std::int64_t& epoch) {
    return Cartridge::create(make_huc3_image(), [&epoch] { return epoch; });
}

void select_mode(Cartridge& cartridge, int mode) {
    cartridge.write_control(0x0000, mode);
}

int execute_command(Cartridge& cartridge, int mailbox) {
    select_mode(cartridge, 0x0b);
    cartridge.write_ram(0xbfff, mailbox);
    select_mode(cartridge, 0x0d);
    cartridge.write_ram(0xa000, 0xfe);
    cartridge.tick(Huc3Mcu::command_busy_dots);
    select_mode(cartridge, 0x0c);
    return cartridge.read_ram(0xa555) & 0x0f;
}

void set_index(Cartridge& cartridge, int address) {
    static_cast<void>(execute_command(cartridge, 0x40 | (address & 0x0f)));
    static_cast<void>(execute_command(cartridge, 0x50 | ((address >> 4) & 0x0f)));
}

void write_nibble(Cartridge& cartridge, int address, int value) {
    set_index(cartridge, address);
    static_cast<void>(execute_command(cartridge, 0x20 | (value & 0x0f)));
}

int read_nibble(Cartridge& cartridge, int address) {
    set_index(cartridge, address);
    return execute_command(cartridge, 0x00);
}

class ProbeInfraredPort final : public InfraredPortEndpoint {
public:
    [[nodiscard]] bool infrared_led_on() const noexcept override { return output; }
    void set_infrared_light(bool detected) noexcept override { input = detected; }

    bool output{};
    bool input{};
};

void header_banking_and_ram_modes_test() {
    std::int64_t epoch = 1'000;
    auto cartridge = create_huc3(epoch);
    check(cartridge->header().mbc == MbcType::huc3 &&
          cartridge->header().has_ram && cartridge->header().has_battery &&
          cartridge->header().has_rtc && cartridge->header().ram_size == 32 * 1024 &&
          cartridge->has_persistent_data(),
          "capacités d'en-tête HuC3 $FE incorrectes");
    check(cartridge->read_rom(0x0000) == 0 && cartridge->read_rom(0x4000) == 1,
          "banques HuC3 de mise sous tension incorrectes");

    cartridge->write_control(0x2000, 0x7f);
    check(cartridge->read_rom(0x4000) == 0x7f,
          "septième bit de banque ROM HuC3 ignoré");
    cartridge->write_control(0x2000, 0xff);
    check(cartridge->read_rom(0x4000) == 0x7f,
          "bit de banque ROM HuC3 non câblé conservé");
    cartridge->write_control(0x2000, 0x00);
    check(cartridge->read_rom(0x4000) == 0,
          "banque ROM zéro HuC3 non exposée par le registre direct");

    // Mode A : lecture/écriture. Mode 0 : même RAM, lecture seule.
    select_mode(*cartridge, 0x0a);
    for (int bank = 0; bank < 4; ++bank) {
        cartridge->write_control(0x4000, bank);
        cartridge->write_ram(0xa123, 0x40 + bank);
    }
    select_mode(*cartridge, 0x00);
    for (int bank = 0; bank < 4; ++bank) {
        cartridge->write_control(0x4000, bank);
        check(cartridge->read_ram(0xa123) == 0x40 + bank,
              "banking SRAM HuC3 incorrect");
        cartridge->write_ram(0xa123, 0x80 + bank);
        check(cartridge->read_ram(0xa123) == 0x40 + bank,
              "mode SRAM HuC3 lecture seule accepte une écriture");
    }

    select_mode(*cartridge, 0x01);
    check(cartridge->read_ram(0xa000) == 0xff,
          "mode HuC3 non décodé ne renvoie pas l'open bus retenu");
    cartridge->write_control(0x2000, 7);
    cartridge->write_control(0x4000, 2);
    cartridge->write_control(0x6000, 0xff);
    check(cartridge->read_rom(0x4000) == 7,
          "écriture $6000-$7FFF HuC3 a modifié le mapping");
}

void mailbox_semaphore_and_nibble_protocol_test() {
    std::int64_t epoch = 2'000;
    auto cartridge = create_huc3(epoch);
    select_mode(*cartridge, 0x0b);
    check(cartridge->read_ram(0xa000) == 0xff,
          "valeur de reset de la boîte B HuC3 incorrecte");
    select_mode(*cartridge, 0x0c);
    check(cartridge->read_ram(0xbfff) == 0xf0,
          "registre de réponse C HuC3 initial incorrect");
    select_mode(*cartridge, 0x0d);
    check(cartridge->read_ram(0xa321) == 0xff,
          "sémaphore HuC3 initial non prêt");

    select_mode(*cartridge, 0x0b);
    cartridge->write_ram(0xa000, 0xe2); // D7 n'est pas câblé
    check(cartridge->read_ram(0xbfff) == 0xe2,
          "D7 de la boîte B HuC3 n'est pas ignoré à l'écriture");
    select_mode(*cartridge, 0x0d);
    check(cartridge->read_ram(0xa000) == 0xe3,
          "lignes partagées ou état prêt du sémaphore HuC3 incorrects");
    cartridge->write_ram(0xa000, 0xff);
    check(cartridge->read_ram(0xa000) == 0xe3,
          "écrire D0=1 a déclenché une commande HuC3");
    cartridge->write_ram(0xbfff, 0xfe);
    check(cartridge->read_ram(0xa000) == 0xe2,
          "sémaphore HuC3 ne devient pas occupé");
    cartridge->tick(Huc3Mcu::command_busy_dots - 1);
    check(cartridge->read_ram(0xa000) == 0xe2,
          "commande HuC3 terminée avant le délai normalisé");
    cartridge->tick(1);
    check(cartridge->read_ram(0xa000) == 0xe3,
          "commande HuC3 ne libère pas le sémaphore");
    select_mode(*cartridge, 0x0c);
    check(cartridge->read_ram(0xbfff) == 0xe1,
          "commande étendue de présence HuC3 ne répond pas 1");

    set_index(*cartridge, 0x20);
    static_cast<void>(execute_command(*cartridge, 0x2a));
    check(execute_command(*cartridge, 0x00) == 0x0a &&
          execute_command(*cartridge, 0x10) == 0x0a,
          "commandes HuC3 lecture fixe/incrémentée incorrectes");
    static_cast<void>(execute_command(*cartridge, 0x3b)); // écrit $21, index -> $22
    check(read_nibble(*cartridge, 0x21) == 0x0b,
          "commande HuC3 écriture incrémentée incorrecte");

    write_nibble(*cartridge, 0x10, 0x0f);
    check(read_nibble(*cartridge, 0x10) == 0,
          "fenêtre interne HuC3 $08-$1F modifiable par erreur");
    set_index(*cartridge, 0xff);
    static_cast<void>(execute_command(*cartridge, 0x10)); // lecture puis wrap vers $00
    static_cast<void>(execute_command(*cartridge, 0x2c));
    check(read_nibble(*cartridge, 0x00) == 0x0c,
          "index HuC3 ne reboucle pas de $FF à $00");
}

void deterministic_rtc_and_long_period_test() {
    std::int64_t epoch = 10'000;
    auto cartridge = create_huc3(epoch);

    // 23:59 du jour 4095, exprimé dans les compteurs minute/jour 12 bits.
    write_nibble(*cartridge, 0, 0x0f);
    write_nibble(*cartridge, 1, 0x09);
    write_nibble(*cartridge, 2, 0x05);
    write_nibble(*cartridge, 3, 0x0f);
    write_nibble(*cartridge, 4, 0x0f);
    write_nibble(*cartridge, 5, 0x0f);
    write_nibble(*cartridge, 6, 1);
    write_nibble(*cartridge, 7, 0);
    static_cast<void>(execute_command(*cartridge, 0x61));

    epoch += 61;
    static_cast<void>(execute_command(*cartridge, 0x60));
    for (int address = 0; address < 6; ++address) {
        check(read_nibble(*cartridge, address) == 0,
              "RTC HuC3 ne reboucle pas minute/jour après le jour 4095");
    }

    // Une armature invalide ne doit pas écraser l'heure courante.
    write_nibble(*cartridge, 0, 5);
    write_nibble(*cartridge, 3, 2);
    write_nibble(*cartridge, 6, 0);
    static_cast<void>(execute_command(*cartridge, 0x61));
    static_cast<void>(execute_command(*cartridge, 0x60));
    check(read_nibble(*cartridge, 0) == 0 && read_nibble(*cartridge, 3) == 0,
          "commande de réglage HuC3 acceptée sans drapeau $06=1");

    constexpr std::int64_t long_minutes =
        static_cast<std::int64_t>(4'096) * 1'440 * 3 + 2 * 1'440 + 17;
    epoch += long_minutes * 60 + 58; // le reliquat précédent était une seconde
    static_cast<void>(execute_command(*cartridge, 0x60));
    check(read_nibble(*cartridge, 0) == 1 && read_nibble(*cartridge, 1) == 1 &&
          read_nibble(*cartridge, 2) == 0 && read_nibble(*cartridge, 3) == 2 &&
          read_nibble(*cartridge, 4) == 0 && read_nibble(*cartridge, 5) == 0,
          "RTC HuC3 incorrecte après plusieurs tours complets du compteur");

    epoch -= 3'600;
    static_cast<void>(execute_command(*cartridge, 0x60));
    check(read_nibble(*cartridge, 0) == 1 && read_nibble(*cartridge, 1) == 1,
          "recul de l'horloge hôte a fait reculer la RTC HuC3");
    epoch += 3'600;
    static_cast<void>(execute_command(*cartridge, 0x60));
    check(read_nibble(*cartridge, 0) == 1 && read_nibble(*cartridge, 1) == 1,
          "rattrapage de l'horloge hôte a compté deux fois la RTC HuC3");
    ++epoch; // reliquat 59 + 1 seconde : minute 18
    static_cast<void>(execute_command(*cartridge, 0x60));
    check(read_nibble(*cartridge, 0) == 2 && read_nibble(*cartridge, 1) == 1,
          "RTC HuC3 ne reprend pas après le rattrapage de l'horloge hôte");
}

void battery_persistence_and_validation_test() {
    std::int64_t epoch = 20'000;
    auto source = create_huc3(epoch);
    select_mode(*source, 0x0a);
    source->write_control(0x4000, 3);
    source->write_ram(0xa010, 0x83);
    write_nibble(*source, 0x20, 0x0a);
    write_nibble(*source, 0xff, 0x0b);

    // 00:10 du jour 20.
    write_nibble(*source, 0, 0x0a);
    write_nibble(*source, 1, 0);
    write_nibble(*source, 2, 0);
    write_nibble(*source, 3, 4);
    write_nibble(*source, 4, 1);
    write_nibble(*source, 5, 0);
    write_nibble(*source, 6, 1);
    write_nibble(*source, 7, 0);
    static_cast<void>(execute_command(*source, 0x61));
    set_index(*source, 0x2a); // l'index survit au reset grâce à la pile HuC3
    epoch += 125;

    const auto battery = source->export_battery();
    check(battery && battery->size() == 32U * 1024U + Huc3::battery_footer_size &&
          (*battery)[0x6010] == 0x83 && (*battery)[32U * 1024U] == 'R' &&
          (*battery)[32U * 1024U + 1] == 'V' &&
          (*battery)[32U * 1024U + 2] == 'H' &&
          (*battery)[32U * 1024U + 3] == '3',
          "pied de page batterie HuC3 absent ou SRAM altérée");

    epoch += 60;
    auto restored = create_huc3(epoch);
    restored->import_battery(*battery);
    check(!restored->dirty(), "import batterie HuC3 valide marqué sale");
    static_cast<void>(execute_command(*restored, 0x2c)); // index restauré $2A
    check(read_nibble(*restored, 0x2a) == 0x0c &&
          read_nibble(*restored, 0x20) == 0x0a &&
          read_nibble(*restored, 0xff) == 0x0b,
          "mémoire/index alimentés HuC3 non restaurés");
    static_cast<void>(execute_command(*restored, 0x60));
    check(read_nibble(*restored, 0) == 0x0d && read_nibble(*restored, 1) == 0 &&
          read_nibble(*restored, 3) == 4 && read_nibble(*restored, 4) == 1,
          "RTC HuC3 persistée n'a pas avancé pendant l'arrêt");

    std::vector<std::uint8_t> legacy(32U * 1024U, 0x55);
    auto legacy_restored = create_huc3(epoch);
    legacy_restored->import_battery(legacy);
    select_mode(*legacy_restored, 0x00);
    check(!legacy_restored->dirty() && legacy_restored->read_ram(0xa123) == 0x55 &&
          legacy_restored->export_battery()->size() ==
              32U * 1024U + Huc3::battery_footer_size,
          "ancienne SRAM brute HuC3 non acceptée proprement");

    auto corrupted = *battery;
    corrupted[32U * 1024U] = 'X';
    auto rejected = create_huc3(epoch);
    select_mode(*rejected, 0x0a);
    rejected->write_ram(0xa000, 0x11);
    rejected->import_battery(corrupted);
    check(rejected->read_ram(0xa000) == 0x11 && rejected->dirty(),
          "pied de page HuC3 corrompu importé partiellement");
    auto wrong_size = *battery;
    wrong_size.pop_back();
    rejected->import_battery(wrong_size);
    check(rejected->read_ram(0xa000) == 0x11,
          "sauvegarde HuC3 tronquée a modifié la SRAM");

    auto wrong_version = *battery;
    wrong_version[32U * 1024U + 4] = 2;
    rejected->import_battery(wrong_version);
    auto invalid_seconds = *battery;
    invalid_seconds[32U * 1024U + 133] = 60;
    rejected->import_battery(invalid_seconds);
    check(rejected->read_ram(0xa000) == 0x11,
          "version ou compteur de secondes HuC3 invalide importé");
}

void infrared_and_machine_router_test() {
    std::int64_t epoch = 30'000;
    LocalInfraredEndpoint local;
    ProbeInfraredPort remote;
    remote.output = true;
    check(local.attach(remote), "sonde IR HuC3 non attachée");
    auto cartridge = create_huc3(epoch);
    check(cartridge->connect_infrared_endpoint(&local),
          "transceiver de cartouche HuC3 non attaché");
    select_mode(*cartridge, 0x0e);
    check(cartridge->read_ram(0xa000) == 0xf1 &&
          cartridge->read_ram(0xbfff) == 0xf1,
          "registre IR HuC3 ou son miroir ne voit pas la lumière");
    cartridge->write_ram(0xa000, 0xff);
    check(remote.input, "LED HuC3 non propagée au backend IR");
    cartridge->write_ram(0xbfff, 0xfe);
    check(!remote.input, "bit IR HuC3 nul n'éteint pas la LED");

    // Une cartouche DMG HuC3 conserve son transceiver sur un CGB en mode
    // compatibilité, tandis que RP reste inaccessible au logiciel.
    LocalInfraredEndpoint compatibility_link;
    ProbeInfraredPort compatibility_peer;
    compatibility_peer.output = true;
    check(compatibility_link.attach(compatibility_peer),
          "sonde HuC3 de compatibilité non attachée");
    Machine compatibility(make_huc3_image(), [&epoch] { return epoch; },
                          gb::HardwareMode::cgb_compatibility);
    check(compatibility.infrared_router.connect(&compatibility_link),
          "routeur HuC3 de compatibilité non attaché");
    compatibility.bus.write(0x0000, 0x0e);
    check(compatibility.bus.read(0xff56) == 0xff &&
          (compatibility.bus.read(0xa000) & 1) == 1,
          "transceiver HuC3 absent ou RP exposé en compatibilité CGB");
    compatibility.bus.write(0xa000, 1);
    check(compatibility_peer.input,
          "LED HuC3 inactive en compatibilité CGB");
}

void mcu_physical_dot_cadence_test() {
    std::int64_t epoch = 35'000;
    const auto begin_status_command = [](Machine& machine) {
        machine.bus.write(0x0000, 0x0b);
        machine.bus.write(0xa000, 0x62);
        machine.bus.write(0x0000, 0x0d);
        machine.bus.write(0xa000, 0xfe);
    };
    const auto semaphore_ready = [](Machine& machine) {
        machine.bus.write(0x0000, 0x0d);
        return (machine.bus.read(0xa000) & 1) != 0;
    };

    Machine normal(make_huc3_image(), [&epoch] { return epoch; }, gb::HardwareMode::dmg);
    begin_status_command(normal);
    normal.bus.tick_mcycle(); // quatre dots périphériques
    check(semaphore_ready(normal),
          "commande HuC3 normale ne termine pas après quatre dots");

    Machine doubled(make_huc3_image(), [&epoch] { return epoch; },
                    gb::HardwareMode::cgb_native);
    doubled.speed.write_key1(1);
    check(doubled.bus.on_stop(), "transition KEY1 HuC3 non lancée");
    doubled.bus.tick_speed_switch(8'200);
    check(doubled.speed.double_speed(), "précondition double vitesse HuC3 absente");
    begin_status_command(doubled);
    doubled.bus.tick_mcycle(); // deux dots périphériques en double vitesse
    check(!semaphore_ready(doubled),
          "commande HuC3 accélérée par erreur avec le CPU CGB");
    doubled.bus.tick_mcycle();
    check(semaphore_ready(doubled),
          "commande HuC3 ne suit pas ses quatre dots physiques en double vitesse");
}

void save_state_mid_command_and_corruption_test() {
    std::int64_t epoch = 40'000;
    LocalInfraredEndpoint local;
    ProbeInfraredPort remote;
    remote.output = true;
    check(local.attach(remote), "sonde save state HuC3 non attachée");
    auto source = create_huc3(epoch);
    check(source->connect_infrared_endpoint(&local),
          "endpoint save state HuC3 non attaché");
    select_mode(*source, 0x0a);
    source->write_control(0x4000, 2);
    source->write_ram(0xa123, 0x7c);
    source->write_control(0x2000, 0x45);
    select_mode(*source, 0x0b);
    source->write_ram(0xa000, 0x62);
    select_mode(*source, 0x0d);
    source->write_ram(0xa000, 0xfe);
    source->tick(2);
    select_mode(*source, 0x0e);
    source->write_ram(0xa000, 1);

    detail::BinaryWriter writer;
    source->save_state(writer);
    const auto state = std::move(writer).take();

    LocalInfraredEndpoint restored_local;
    ProbeInfraredPort restored_remote;
    restored_remote.output = true;
    check(restored_local.attach(restored_remote), "sonde HuC3 restaurée non attachée");
    auto restored = create_huc3(epoch);
    check(restored->connect_infrared_endpoint(&restored_local),
          "endpoint HuC3 restauré non attaché");
    detail::BinaryReader reader(state);
    restored->load_state(reader);
    detail::BinaryWriter roundtrip_writer;
    restored->save_state(roundtrip_writer);
    check(reader.exhausted(), "save state HuC3 laisse des données excédentaires");
    check(std::move(roundtrip_writer).take() == state,
          "save state HuC3 n'est pas déterministe après restauration");
    check(restored->read_rom(0x4000) == 0x45,
          "save state HuC3 n'a pas restauré la banque ROM");
    check(restored->read_ram(0xa000) == 0xe1 && restored_remote.input,
          "save state HuC3 n'a pas restauré le transceiver IR");

    select_mode(*restored, 0x0d);
    check((restored->read_ram(0xa000) & 1) == 0,
          "save state HuC3 a perdu le sémaphore occupé");
    restored->tick(1);
    check((restored->read_ram(0xa000) & 1) == 0,
          "save state HuC3 a raccourci la commande en cours");
    restored->tick(1);
    check((restored->read_ram(0xa000) & 1) == 1,
          "save state HuC3 n'a pas repris la commande en cours");
    select_mode(*restored, 0x0c);
    check((restored->read_ram(0xa000) & 0x0f) == 1,
          "commande HuC3 restaurée n'a pas produit sa réponse");
    select_mode(*restored, 0x0a);
    restored->write_control(0x4000, 2);
    check(restored->read_ram(0xa123) == 0x7c,
          "SRAM HuC3 absente du save state");

    auto invalid_layout = state;
    invalid_layout[3] = 2;
    expect_failure<SaveStateError>([&] {
        auto invalid = create_huc3(epoch);
        detail::BinaryReader input(invalid_layout);
        invalid->load_state(input);
    }, "layout HuC3 incompatible accepté");

    auto invalid_mode = state;
    invalid_mode[4] = 0x10;
    expect_failure<SaveStateError>([&] {
        auto invalid = create_huc3(epoch);
        detail::BinaryReader input(invalid_mode);
        invalid->load_state(input);
    }, "mode HuC3 corrompu accepté");

    auto invalid_bool = state;
    invalid_bool[7] = 2;
    expect_failure<SaveStateError>([&] {
        auto invalid = create_huc3(epoch);
        detail::BinaryReader input(invalid_bool);
        invalid->load_state(input);
    }, "booléen HuC3 corrompu accepté");

    auto invalid_mailbox = state;
    invalid_mailbox[9] = 0x80;
    expect_failure<SaveStateError>([&] {
        auto invalid = create_huc3(epoch);
        detail::BinaryReader input(invalid_mailbox);
        invalid->load_state(input);
    }, "boîte HuC3 corrompue acceptée");

    auto invalid_busy = state;
    invalid_busy[16] = Huc3Mcu::command_busy_dots + 1;
    expect_failure<SaveStateError>([&] {
        auto invalid = create_huc3(epoch);
        detail::BinaryReader input(invalid_busy);
        invalid->load_state(input);
    }, "durée occupée HuC3 corrompue acceptée");

    auto invalid_nibble = state;
    invalid_nibble[27] = 0x10;
    expect_failure<SaveStateError>([&] {
        auto invalid = create_huc3(epoch);
        detail::BinaryReader input(invalid_nibble);
        invalid->load_state(input);
    }, "nibble interne HuC3 corrompu accepté");

    auto invalid_minute = state;
    invalid_minute[27 + 0x10] = 0;
    invalid_minute[27 + 0x11] = 0;
    invalid_minute[27 + 0x12] = 6; // $600 minutes, hors plage 0..1439
    expect_failure<SaveStateError>([&] {
        auto invalid = create_huc3(epoch);
        detail::BinaryReader input(invalid_minute);
        invalid->load_state(input);
    }, "compteur minute HuC3 hors plage accepté");
}

} // namespace ravenemu::cgb::testing

int main() {
    ravenemu::cgb::testing::header_banking_and_ram_modes_test();
    ravenemu::cgb::testing::mailbox_semaphore_and_nibble_protocol_test();
    ravenemu::cgb::testing::deterministic_rtc_and_long_period_test();
    ravenemu::cgb::testing::battery_persistence_and_validation_test();
    ravenemu::cgb::testing::infrared_and_machine_router_test();
    ravenemu::cgb::testing::mcu_physical_dot_cadence_test();
    ravenemu::cgb::testing::save_state_mid_command_and_corruption_test();
}
