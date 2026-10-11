#include "cartridge/cartridge_factory.hpp"
#include "check.hpp"

#include <memory>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

void finalize_mbc6_header(std::vector<std::uint8_t>& rom) {
    std::uint8_t checksum{};
    for (std::size_t offset = 0x134; offset <= 0x14c; ++offset) {
        checksum = static_cast<std::uint8_t>(checksum - rom[offset] - 1U);
    }
    rom[0x14d] = checksum;
}

std::vector<std::uint8_t> make_mbc6_rom() {
    std::vector<std::uint8_t> rom(1U * 1024U * 1024U);
    const int banks = static_cast<int>(rom.size() / Mbc6::rom_window_size);
    for (int bank = 0; bank < banks; ++bank) {
        const auto base = static_cast<std::size_t>(bank * Mbc6::rom_window_size);
        rom[base] = static_cast<std::uint8_t>(bank);
        rom[base + 1] = static_cast<std::uint8_t>(bank >> 8);
    }
    rom[0x143] = 0x80;
    rom[0x147] = 0x20;
    // La taille SRAM est imposée par le matériel MBC6, indépendamment de
    // cet octet. Le laisser à zéro vérifie ce contrat dans le parseur.
    rom[0x149] = 0x00;
    finalize_mbc6_header(rom);
    return rom;
}

std::unique_ptr<Cartridge> create_mbc6() {
    auto image = std::make_shared<const std::vector<std::uint8_t>>(make_mbc6_rom());
    return Cartridge::create(std::move(image), [] { return std::int64_t{0}; });
}

int visible_bank(Cartridge& cartridge, int address) {
    return cartridge.read_rom(address) | (cartridge.read_rom(address + 1) << 8);
}

void select_command_banks(Cartridge& cartridge) {
    cartridge.write_control(0x2000, 2); // 5555 -> adresse flash 05555
    cartridge.write_control(0x3000, 1); // 6AAA -> adresse flash 02AAA
}

void enable_flash(Cartridge& cartridge) {
    cartridge.write_control(0x0c00, 1);
    cartridge.write_control(0x2800, 0x08);
    cartridge.write_control(0x3800, 0x08);
    select_command_banks(cartridge);
}

void begin_flash_command(Cartridge& cartridge, int command) {
    select_command_banks(cartridge);
    cartridge.write_control(0x5555, 0xaa);
    cartridge.write_control(0x6aaa, 0x55);
    cartridge.write_control(0x5555, command);
}

void write_followup_unlock(Cartridge& cartridge) {
    cartridge.write_control(0x5555, 0xaa);
    cartridge.write_control(0x6aaa, 0x55);
}

void reset_flash(Cartridge& cartridge) {
    cartridge.write_control(0x4000, 0xf0);
}

int read_flash(Cartridge& cartridge, std::size_t address) {
    cartridge.write_control(0x2000,
                            static_cast<int>(address / Mbc6::rom_window_size));
    return cartridge.read_rom(0x4000 +
                              static_cast<int>(address & (Mbc6::rom_window_size - 1)));
}

void program_flash_byte(Cartridge& cartridge, std::size_t address, int value) {
    begin_flash_command(cartridge, 0xa0);
    // Les lignes A19-A7 sont ignorées pendant le remplissage. Elles ne
    // désignent le bloc cible qu'au second accès consécutif au même offset.
    cartridge.write_control(0x2000, 0);
    cartridge.write_control(0x4000 + static_cast<int>(address & 0x7fU), value);
    cartridge.write_control(0x2000,
                            static_cast<int>(address / Mbc6::rom_window_size));
    cartridge.write_control(0x4000 +
                            static_cast<int>(address & (Mbc6::rom_window_size - 1)), 0);
    check((cartridge.read_rom(0x4000) & 0x80) != 0,
          "programmation flash MBC6 ne publie pas le statut prêt");
    reset_flash(cartridge);
}

void erase_flash_sector(Cartridge& cartridge, std::size_t address) {
    begin_flash_command(cartridge, 0x80);
    write_followup_unlock(cartridge);
    cartridge.write_control(0x2000,
                            static_cast<int>(address / Mbc6::rom_window_size));
    cartridge.write_control(0x4000 +
                            static_cast<int>(address & (Mbc6::rom_window_size - 1)), 0x30);
    check((cartridge.read_rom(0x4000) & 0x80) != 0,
          "effacement secteur MBC6 n'atteint pas le statut prêt");
    reset_flash(cartridge);
}

void erase_flash_chip(Cartridge& cartridge) {
    begin_flash_command(cartridge, 0x80);
    write_followup_unlock(cartridge);
    cartridge.write_control(0x5555, 0x10);
    check((cartridge.read_rom(0x4000) & 0x80) != 0,
          "effacement global MBC6 n'atteint pas le statut prêt");
    reset_flash(cartridge);
}

void set_sector_zero_protection(Cartridge& cartridge, bool protect,
                                std::size_t final_address = 0x5555) {
    begin_flash_command(cartridge, 0x60);
    write_followup_unlock(cartridge);
    cartridge.write_control(0x2000,
                            static_cast<int>(final_address / Mbc6::rom_window_size));
    cartridge.write_control(0x4000 + static_cast<int>(
        final_address & (Mbc6::rom_window_size - 1)
    ), protect ? 0x20 : 0x40);
}

void program_hidden_byte(Cartridge& cartridge, int index, int value) {
    begin_flash_command(cartridge, 0x60);
    write_followup_unlock(cartridge);
    cartridge.write_control(0x5555, 0xe0);
    cartridge.write_control(0x2000, 0);
    cartridge.write_control(0x4000 + (index & 0x7f), value);
    cartridge.write_control(0x4000 + (index & 0xff), 0);
    check((cartridge.read_rom(0x4000) & 0x80) != 0,
          "programmation de la région cachée MBC6 sans statut prêt");
    reset_flash(cartridge);
}

int read_hidden_byte(Cartridge& cartridge, int index) {
    begin_flash_command(cartridge, 0x77);
    write_followup_unlock(cartridge);
    cartridge.write_control(0x5555, 0x77);
    cartridge.write_control(0x2000, 0);
    const int value = cartridge.read_rom(0x4000 + (index & 0xff));
    reset_flash(cartridge);
    return value;
}

void erase_hidden(Cartridge& cartridge) {
    begin_flash_command(cartridge, 0x60);
    write_followup_unlock(cartridge);
    cartridge.write_control(0x5555, 0x04);
}

void mbc6_header_and_banking_test() {
    auto cartridge = create_mbc6();
    const auto& header = cartridge->header();
    check(header.mbc == MbcType::mbc6 && header.cartridge_type == 0x20 &&
          header.has_ram && header.has_battery && header.has_flash &&
          header.ram_size == static_cast<int>(Mbc6::sram_size) &&
          header.uses_color && cartridge->has_persistent_data(),
          "capacités matérielles MBC6 non déduites du type $20");
    check(visible_bank(*cartridge, 0x0000) == 0 &&
          visible_bank(*cartridge, 0x2000) == 1 &&
          visible_bank(*cartridge, 0x4000) == 2 &&
          visible_bank(*cartridge, 0x6000) == 3,
          "mapping ROM MBC6 au power-on incorrect");

    cartridge->write_control(0x2000, 0x85);
    cartridge->write_control(0x3000, 0x86);
    check(visible_bank(*cartridge, 0x4000) == 5 &&
          visible_bank(*cartridge, 0x6000) == 6,
          "fenêtres ROM A/B MBC6 non indépendantes ou mal masquées");

    cartridge->write_control(0x2800, 0x08);
    check(cartridge->read_rom(0x4000) == 0xff,
          "flash MBC6 sélectionnée reste active avec /CE désactivé");
    cartridge->write_control(0x2800, 0x00);
    check(visible_bank(*cartridge, 0x4000) == 5,
          "retour de la fenêtre A MBC6 vers la ROM incorrect");
}

void mbc6_ram_windows_test() {
    auto cartridge = create_mbc6();
    cartridge->write_ram(0xa123, 0x11);
    check(cartridge->read_ram(0xa123) == 0xff,
          "SRAM MBC6 accessible avant son activation");

    cartridge->write_control(0x0000, 0x0a);
    cartridge->write_ram(0xa123, 0x21); // banque A 0
    cartridge->write_ram(0xb234, 0x31); // banque B 1
    cartridge->write_control(0x0400, 0xff); // masque -> banque 7
    cartridge->write_control(0x0800, 0xfb); // masque -> banque 3
    cartridge->write_ram(0xa123, 0x27);
    cartridge->write_ram(0xb234, 0x33);
    cartridge->write_control(0x0400, 0);
    cartridge->write_control(0x0800, 1);
    check(cartridge->read_ram(0xa123) == 0x21 &&
          cartridge->read_ram(0xb234) == 0x31,
          "fenêtres SRAM A/B MBC6 ne conservent pas leurs banques distinctes");
    cartridge->write_control(0x0400, 7);
    cartridge->write_control(0x0800, 3);
    check(cartridge->read_ram(0xa123) == 0x27 &&
          cartridge->read_ram(0xb234) == 0x33,
          "banques SRAM hautes MBC6 non relues correctement");
    cartridge->write_control(0x0000, 0);
    check(cartridge->read_ram(0xa123) == 0xff,
          "désactivation SRAM MBC6 n'expose pas l'open bus retenu");
}

void mbc6_flash_program_id_and_erase_test() {
    auto cartridge = create_mbc6();
    enable_flash(*cartridge);

    begin_flash_command(*cartridge, 0x90);
    cartridge->write_control(0x2000, 0);
    check(cartridge->read_rom(0x4000) == 0xc2 &&
          cartridge->read_rom(0x4001) == 0x81 &&
          cartridge->read_rom(0x4002) == 0xc2 &&
          cartridge->read_rom(0x4004) == 0xc2,
          "identifiants JEDEC du secteur 0 de la flash MBC6 incorrects");
    cartridge->write_control(0x2000, 16);
    check(cartridge->read_rom(0x4002) == 0x00 &&
          cartridge->read_rom(0x4003) == 0xff,
          "identifiants JEDEC hors secteur 0 de la flash MBC6 incorrects");
    reset_flash(*cartridge);

    // Le composant flash ne décode que A0-A14 pour les commandes.
    cartridge->write_control(0x2000, 18); // 25555
    cartridge->write_control(0x3000, 17); // 22AAA
    cartridge->write_control(0x5555, 0xaa);
    cartridge->write_control(0x6aaa, 0x55);
    cartridge->write_control(0x5555, 0x90);
    check(cartridge->read_rom(0x4000) == 0xc2,
          "lignes hautes de l'adresse flash MBC6 décodées dans une commande");
    reset_flash(*cartridge);

    constexpr std::size_t target = 2U * Mbc6Flash::sector_size + 0x123U;
    program_flash_byte(*cartridge, target, 0x0f);
    check(read_flash(*cartridge, target) == 0x0f,
          "buffer de programmation MBC6 non appliqué au bloc cible");
    program_flash_byte(*cartridge, target, 0xf3);
    check(read_flash(*cartridge, target) == 0x03,
          "programmation MBC6 a remis des bits à 1 sans effacement");

    erase_flash_sector(*cartridge, target);
    check(read_flash(*cartridge, target) == 0xff,
          "effacement du secteur MBC6 n'a pas restauré les bits à 1");

    cartridge->write_control(0x0c00, 0);
    begin_flash_command(*cartridge, 0x90); // /CE bas : commande ignorée
    cartridge->write_control(0x0c00, 1);
    cartridge->write_control(0x2000, 0);
    check(cartridge->read_rom(0x4000) == 0xff,
          "commande flash MBC6 acceptée alors que Flash Enable était nul");
}

void mbc6_flash_protection_and_chip_erase_test() {
    auto cartridge = create_mbc6();
    enable_flash(*cartridge);
    cartridge->write_control(0x1000, 1);
    constexpr std::size_t sector_zero_byte = 0x100;
    constexpr std::size_t sector_one_byte = Mbc6Flash::sector_size + 0x10;
    program_flash_byte(*cartridge, sector_zero_byte, 0x0f);
    program_flash_byte(*cartridge, sector_one_byte, 0x33);

    set_sector_zero_protection(*cartridge, true, Mbc6Flash::sector_size);
    begin_flash_command(*cartridge, 0xa0);
    check((cartridge->read_rom(0x4000) & 0x02) == 0,
          "protection secteur 0 MBC6 acceptée depuis un autre secteur");
    reset_flash(*cartridge);

    set_sector_zero_protection(*cartridge, true, 0x10000);
    check((cartridge->read_rom(0x4000) & 0x82) == 0x82,
          "protection MBC6 par une adresse quelconque du secteur 0 absente");
    reset_flash(*cartridge);

    cartridge->write_control(0x1000, 0);
    set_sector_zero_protection(*cartridge, false); // ignoré sans /WP levé
    begin_flash_command(*cartridge, 0xa0);
    check((cartridge->read_rom(0x4000) & 0x82) == 0x82,
          "protection secteur 0 MBC6 retirée malgré Flash Write Enable=0");
    reset_flash(*cartridge);

    erase_flash_chip(*cartridge);
    check(read_flash(*cartridge, sector_zero_byte) == 0x0f &&
          read_flash(*cartridge, sector_one_byte) == 0xff,
          "effacement global MBC6 n'a pas préservé uniquement le secteur 0 protégé");

    cartridge->write_control(0x1000, 1);
    set_sector_zero_protection(*cartridge, false);
    check((cartridge->read_rom(0x4000) & 0x82) == 0x80,
          "commande déprotection du secteur 0 MBC6 sans effet");
    reset_flash(*cartridge);
    erase_flash_chip(*cartridge);
    check(read_flash(*cartridge, sector_zero_byte) == 0xff,
          "secteur 0 MBC6 déproté non effacé par l'effacement global");
}

void mbc6_hidden_and_persistence_test() {
    auto source = create_mbc6();
    enable_flash(*source);
    source->write_control(0x1000, 1);
    constexpr int hidden_index = 0x83;
    constexpr std::size_t flash_target = 3U * Mbc6Flash::sector_size + 0x20U;
    program_hidden_byte(*source, hidden_index, 0x5a);
    check(read_hidden_byte(*source, hidden_index) == 0x5a,
          "région cachée MBC6 non lisible après programmation");

    source->write_control(0x1000, 0);
    erase_hidden(*source);
    check(read_hidden_byte(*source, hidden_index) == 0x5a,
          "région cachée MBC6 effacée avec /WP actif");
    source->write_control(0x1000, 1);
    program_flash_byte(*source, flash_target, 0x3c);
    set_sector_zero_protection(*source, true);
    reset_flash(*source);

    source->write_control(0x0000, 0x0a);
    source->write_control(0x0400, 7);
    source->write_control(0x0800, 3);
    source->write_ram(0xa123, 0x77);
    source->write_ram(0xb234, 0x88);

    const auto battery = source->export_battery();
    const std::size_t footer = Mbc6::battery_image_size - Mbc6::battery_footer_size;
    check(battery.has_value() && battery->size() == Mbc6::battery_image_size &&
          (*battery)[7U * Mbc6::ram_window_size + 0x123U] == 0x77 &&
          (*battery)[3U * Mbc6::ram_window_size + 0x234U] == 0x88 &&
          (*battery)[Mbc6::sram_size + flash_target] == 0x3c &&
          (*battery)[Mbc6::sram_size + Mbc6Flash::storage_size + hidden_index] == 0x5a &&
          (*battery)[footer] == 'R' && (*battery)[footer + 5] == 1,
          "conteneur persistant MBC6 incomplet ou mal structuré");

    auto restored = create_mbc6();
    restored->import_battery(*battery);
    restored->write_control(0x0000, 0x0a);
    restored->write_control(0x0400, 7);
    restored->write_control(0x0800, 3);
    enable_flash(*restored);
    check(restored->read_ram(0xa123) == 0x77 &&
          restored->read_ram(0xb234) == 0x88 &&
          read_flash(*restored, flash_target) == 0x3c &&
          read_hidden_byte(*restored, hidden_index) == 0x5a,
          "SRAM/flash/région cachée MBC6 non restaurées atomiquement");
    begin_flash_command(*restored, 0xa0);
    check((restored->read_rom(0x4000) & 0x02) != 0,
          "protection secteur 0 MBC6 non restaurée depuis la batterie");
    reset_flash(*restored);

    auto invalid = *battery;
    invalid[footer] ^= 1;
    auto rejected = create_mbc6();
    rejected->import_battery(invalid);
    rejected->write_control(0x0000, 0x0a);
    rejected->write_control(0x0400, 7);
    enable_flash(*rejected);
    check(rejected->read_ram(0xa123) == 0 &&
          read_flash(*rejected, flash_target) == 0xff,
          "conteneur batterie MBC6 invalide importé partiellement");

    restored->write_control(0x1000, 1);
    erase_hidden(*restored);
    check((restored->read_rom(0x4000) & 0x80) != 0,
          "effacement région cachée MBC6 sans statut prêt");
    reset_flash(*restored);
    check(read_hidden_byte(*restored, hidden_index) == 0xff,
          "effacement autorisé de la région cachée MBC6 incomplet");
}

void mbc6_save_state_mid_program_test() {
    auto source = create_mbc6();
    enable_flash(*source);
    source->write_control(0x1000, 1);
    source->write_control(0x0000, 0x0a);
    source->write_control(0x0400, 4);
    source->write_ram(0xa321, 0x64);
    begin_flash_command(*source, 0xa0);
    source->write_control(0x2000, 0);
    source->write_control(0x4005, 0x3c); // buffer non encore validé

    detail::BinaryWriter writer;
    source->save_state(writer);
    const auto state = std::move(writer).take();
    check(state.size() > Mbc6Flash::storage_size,
          "save state MBC6 ne contient pas la flash complète");

    auto restored = create_mbc6();
    detail::BinaryReader reader(state);
    restored->load_state(reader);
    constexpr std::size_t target = 6U * Mbc6Flash::sector_size + 5U;
    restored->write_control(0x2000,
                            static_cast<int>(target / Mbc6::rom_window_size));
    restored->write_control(0x4005, 0); // même offset : commit du buffer restauré
    check(reader.exhausted() && (restored->read_rom(0x4000) & 0x80) != 0,
          "phase intermédiaire du buffer flash MBC6 non restaurée");
    reset_flash(*restored);
    check(read_flash(*restored, target) == 0x3c &&
          restored->read_ram(0xa321) == 0x64,
          "save state MBC6 n'a pas restauré flash, registres et SRAM");

    auto incompatible = state;
    incompatible[3] = 2;
    expect_failure<ravenemu::SaveStateError>(
        [&] {
            auto invalid = create_mbc6();
            detail::BinaryReader invalid_reader(incompatible);
            invalid->load_state(invalid_reader);
        },
        "layout de save state MBC6 incompatible accepté"
    );

    auto invalid_mode = state;
    invalid_mode[13] = 0xff; // premier champ de l'état interne de la flash
    expect_failure<ravenemu::SaveStateError>(
        [&] {
            auto invalid = create_mbc6();
            detail::BinaryReader invalid_reader(invalid_mode);
            invalid->load_state(invalid_reader);
        },
        "mode interne flash MBC6 impossible accepté dans un save state"
    );

    auto orphaned_buffer = state;
    orphaned_buffer[16] = 0xff; // tampon rempli sans dernier offset observable
    expect_failure<ravenemu::SaveStateError>(
        [&] {
            auto invalid = create_mbc6();
            detail::BinaryReader invalid_reader(orphaned_buffer);
            invalid->load_state(invalid_reader);
        },
        "tampon flash MBC6 incohérent accepté dans un save state"
    );
}

} // namespace ravenemu::cgb::testing

int main() {
    ravenemu::cgb::testing::mbc6_header_and_banking_test();
    ravenemu::cgb::testing::mbc6_ram_windows_test();
    ravenemu::cgb::testing::mbc6_flash_program_id_and_erase_test();
    ravenemu::cgb::testing::mbc6_flash_protection_and_chip_erase_test();
    ravenemu::cgb::testing::mbc6_hidden_and_persistence_test();
    ravenemu::cgb::testing::mbc6_save_state_mid_program_test();
    return 0;
}
