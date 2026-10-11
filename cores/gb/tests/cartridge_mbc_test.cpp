#include "cartridge/cartridge_factory.hpp"
#include "check.hpp"

#include <array>
#include <memory>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

void finalize_header_checksum(std::vector<std::uint8_t>& rom, std::size_t base) {
    std::uint8_t checksum{};
    for (std::size_t offset = 0x134; offset <= 0x14c; ++offset) {
        checksum = static_cast<std::uint8_t>(checksum - rom[base + offset] - 1U);
    }
    rom[base + 0x14d] = checksum;
}

std::vector<std::uint8_t> make_one_megabyte_mbc1(bool multicart) {
    std::vector<std::uint8_t> rom(0x100000);
    for (int bank = 0; bank < 64; ++bank) {
        std::fill_n(rom.begin() + static_cast<std::ptrdiff_t>(bank * Cartridge::rom_bank_size),
                    Cartridge::rom_bank_size, static_cast<std::uint8_t>(bank));
    }
    rom[0x147] = 0x03; // MBC1 + RAM + batterie
    rom[0x149] = 0x02; // 8 Kio
    finalize_header_checksum(rom, 0);

    constexpr std::size_t second_header = 0x40000; // banque $10
    rom[second_header + 0x147] = 0x00;
    finalize_header_checksum(rom, second_header);
    if (!multicart) rom[second_header + 0x14d] ^= 1;
    return rom;
}

std::unique_ptr<Cartridge> create(std::vector<std::uint8_t> rom) {
    auto image = std::make_shared<const std::vector<std::uint8_t>>(std::move(rom));
    return Cartridge::create(std::move(image), [] { return std::int64_t{0}; });
}

std::vector<std::uint8_t> make_mmm01(int type = 0x0d,
                                     std::size_t size = 8U * 1024U * 1024U,
                                     int ram_size_code = 0x04,
                                     int cgb_flag = 0x00) {
    std::vector<std::uint8_t> rom(size);
    const int banks = static_cast<int>(size / Cartridge::rom_bank_size);
    for (int bank = 0; bank < banks; ++bank) {
        const auto base = static_cast<std::size_t>(bank * Cartridge::rom_bank_size);
        rom[base] = static_cast<std::uint8_t>(bank);
        rom[base + 1] = static_cast<std::uint8_t>(bank >> 8);
    }

    // L'en-tête initial appartient volontairement à une sous-ROM MBC1 : le
    // vrai en-tête MMM01 visible au reset se trouve dans les 32 Kio finaux.
    rom[0x147] = 0x01;
    finalize_header_checksum(rom, 0);
    const auto menu_base = size - CartridgeHeader::min_rom_size;
    rom[menu_base + 0x143] = static_cast<std::uint8_t>(cgb_flag);
    rom[menu_base + 0x147] = static_cast<std::uint8_t>(type);
    rom[menu_base + 0x149] = static_cast<std::uint8_t>(ram_size_code);
    finalize_header_checksum(rom, menu_base);
    return rom;
}

int visible_rom_bank(Cartridge& cartridge, int address) {
    return cartridge.read_rom(address) | (cartridge.read_rom(address + 1) << 8);
}

std::vector<std::uint8_t> make_mbc3_rtc() {
    std::vector<std::uint8_t> rom(0x8000);
    rom[0x147] = 0x10; // MBC3 + timer + RAM + batterie
    rom[0x149] = 0x03;
    return rom;
}

std::unique_ptr<Cartridge> create_rtc(std::int64_t& epoch) {
    auto image = std::make_shared<const std::vector<std::uint8_t>>(make_mbc3_rtc());
    return Cartridge::create(std::move(image), [&epoch] { return epoch; });
}

void rtc_write(Cartridge& cartridge, int reg, int value) {
    cartridge.write_control(0x0000, 0x0a);
    cartridge.write_control(0x4000, reg);
    cartridge.write_ram(0xa000, value);
}

void rtc_latch(Cartridge& cartridge) {
    cartridge.write_control(0x0000, 0x0a);
    cartridge.write_control(0x6000, 0);
    cartridge.write_control(0x6000, 1);
}

int rtc_read(Cartridge& cartridge, int reg) {
    cartridge.write_control(0x4000, reg);
    return cartridge.read_ram(0xa000);
}

void mbc1m_detection_and_banking_test() {
    auto multicart = create(make_one_megabyte_mbc1(true));
    check(multicart->header().mbc1_multicart, "MBC1M synthétique non détecté");
    check(multicart->read_rom(0x4000) == 1, "banque initiale MBC1M incorrecte");

    multicart->write_control(0x4000, 1);
    check(multicart->read_rom(0x4000) == 17,
          "registre secondaire MBC1M non câblé sur les bits 4-5");
    multicart->write_control(0x6000, 1);
    check(multicart->read_rom(0x0000) == 16,
          "mode 1 MBC1M ne remappe pas la fenêtre ROM basse");
    multicart->write_control(0x2000, 0x10);
    check(multicart->read_rom(0x4000) == 16,
          "bit 4 MBC1M ignoré sans préserver la règle spéciale banque zéro");

    auto regular = create(make_one_megabyte_mbc1(false));
    check(!regular->header().mbc1_multicart, "MBC1 standard détecté à tort comme MBC1M");
    regular->write_control(0x4000, 1);
    check(regular->read_rom(0x4000) == 33,
          "MBC1 standard 1 Mio a perdu son câblage sur les bits 5-6");
}

void mbc1m_state_and_persistence_test() {
    auto source = create(make_one_megabyte_mbc1(true));
    source->write_control(0x0000, 0x0a);
    source->write_control(0x4000, 2);
    source->write_control(0x2000, 3);
    source->write_control(0x6000, 1);
    source->write_ram(0xa000, 0x5a);

    detail::BinaryWriter writer;
    source->save_state(writer);
    const auto state = std::move(writer).take();
    auto restored = create(make_one_megabyte_mbc1(true));
    detail::BinaryReader reader(state);
    restored->load_state(reader);
    check(reader.exhausted(), "save state MBC1M laisse des données excédentaires");
    check(restored->read_rom(0x0000) == 32 && restored->read_rom(0x4000) == 35,
          "banques MBC1M non restaurées");
    check(restored->read_ram(0xa000) == 0x5a, "RAM MBC1M non restaurée");

    const auto battery = source->export_battery();
    check(battery.has_value(), "persistance batterie MBC1M absente");
    auto persisted = create(make_one_megabyte_mbc1(true));
    persisted->import_battery(*battery);
    persisted->write_control(0x0000, 0x0a);
    check(persisted->read_ram(0xa000) == 0x5a, "RAM batterie MBC1M non réimportée");
}

void mmm01_header_and_startup_mapping_test() {
    auto image = make_mmm01(0x0d, 512U * 1024U, 0x03, 0x80);
    auto cartridge = create(image);
    check(cartridge->header().mbc == MbcType::mmm01 &&
          cartridge->header().cartridge_type == 0x0d &&
          cartridge->header().has_ram && cartridge->header().has_battery &&
          cartridge->header().ram_size == 32 * 1024 &&
          cartridge->header().uses_color,
          "en-tête MMM01 final non utilisé pour les capacités de la cartouche");
    check(visible_rom_bank(*cartridge, 0x0000) == 30 &&
          visible_rom_bank(*cartridge, 0x4000) == 31,
          "MMM01 ne démarre pas sur les deux dernières banques du menu");

    const std::array variants{
        std::array{0x0b, 0, 0},
        std::array{0x0c, 1, 0},
        std::array{0x0d, 1, 1},
    };
    for (const auto& variant : variants) {
        auto parsed = create(make_mmm01(variant[0], 512U * 1024U, 0x02));
        check(parsed->header().has_ram == (variant[1] != 0) &&
              parsed->header().has_battery == (variant[2] != 0) &&
              parsed->has_persistent_data() == (variant[2] != 0),
              "capacités RAM/batterie incorrectes pour une variante MMM01");
    }

    auto false_positive = make_one_megabyte_mbc1(false);
    const auto tail = false_positive.size() - CartridgeHeader::min_rom_size;
    false_positive[tail + 0x147] = 0x0d; // checksum final volontairement invalide
    auto regular = create(std::move(false_positive));
    check(regular->header().mbc == MbcType::mbc1,
          "octet MMM01 fortuit sans en-tête valide détecté dans la dernière sous-ROM");
}

void mmm01_non_multiplexed_banking_and_locks_test() {
    auto cartridge = create(make_mmm01());
    cartridge->write_control(0x2000, 0x30); // ROM low=$10, mid=1
    cartridge->write_control(0x6000, 0x38); // masque ROM=$1C
    cartridge->write_control(0x2000, 0x21); // bit $10 verrouillé, low devient $11
    cartridge->write_control(0x4000, 0x20); // ROM high=2
    cartridge->write_control(0x0000, 0x40); // mapping irréversible

    check(visible_rom_bank(*cartridge, 0x0000) == 304 &&
          visible_rom_bank(*cartridge, 0x4000) == 305,
          "composition des bits ROM MMM01 non multiplexés incorrecte");

    cartridge->write_control(0x2000, 0x62); // mid=3 ignoré, bits masqués conservés
    cartridge->write_control(0x4000, 0x00); // ROM high verrouillé après mapping
    cartridge->write_control(0x6000, 0x40); // multiplex ignoré après mapping
    check(visible_rom_bank(*cartridge, 0x0000) == 304 &&
          visible_rom_bank(*cartridge, 0x4000) == 306,
          "bits étendus MMM01 modifiés après leur verrouillage");

    cartridge->write_control(0x0000, 0x00); // désactive RAM, pas le mapping
    check(visible_rom_bank(*cartridge, 0x0000) == 304,
          "écriture Mapping Enable=0 a ramené MMM01 en mode menu");
}

void mmm01_ram_banking_persistence_and_open_bus_test() {
    auto source = create(make_mmm01());
    source->write_control(0x4000, 0x0b); // RAM high=2, low=3
    source->write_control(0x0000, 0x2a); // masque RAM=2, RAM active
    source->write_ram(0xa000, 0x33);
    check(source->read_ram(0xa000) == 0x33,
          "chemin RAM MMM01 documenté avant mapping non déterministe");
    source->write_control(0x0000, 0x6a); // mapping avec le même masque

    source->write_ram(0xa000, 0xaa); // mode 0 : banque 10
    source->write_control(0x6000, 0x01);
    source->write_ram(0xa000, 0xbb); // mode 1 : banque 11
    source->write_control(0x6000, 0x00);
    check(source->read_ram(0xa000) == 0xaa,
          "mode 0 MMM01 n'a pas masqué les bits RAM réservés au jeu");
    source->write_control(0x6000, 0x01);
    check(source->read_ram(0xa000) == 0xbb,
          "mode 1 MMM01 n'a pas sélectionné la banque RAM complète");

    source->write_control(0x4000, 0x08); // bit 1 verrouillé, bit 0 devient zéro
    check(source->read_ram(0xa000) == 0xaa,
          "masque RAM MMM01 n'a pas empêché l'écriture du bit verrouillé");
    source->write_control(0x0000, 0x00);
    check(source->read_ram(0xa000) == 0xff,
          "RAM MMM01 désactivée ne renvoie pas la valeur open-bus retenue");
    source->write_ram(0xa000, 0x11);

    const auto battery = source->export_battery();
    check(battery.has_value() && battery->size() == 128U * 1024U &&
          (*battery)[10U * Cartridge::ram_bank_size] == 0xaa &&
          (*battery)[11U * Cartridge::ram_bank_size] == 0xbb,
          "persistance batterie MMM01 ne contient pas toutes les banques RAM");

    auto restored = create(make_mmm01());
    restored->import_battery(*battery);
    restored->write_control(0x4000, 0x0b);
    restored->write_control(0x0000, 0x2a);
    restored->write_control(0x0000, 0x6a);
    restored->write_control(0x6000, 0x01);
    check(restored->read_ram(0xa000) == 0xbb,
          "banque RAM MMM01 non restaurée depuis la sauvegarde batterie");
}

void mmm01_multiplex_and_mode_lock_test() {
    auto multiplexed = create(make_mmm01());
    multiplexed->write_control(0x2000, 0x41); // ROM mid=2, low=1
    multiplexed->write_control(0x6000, 0x40); // multiplex, mode 0
    multiplexed->write_control(0x4000, 0x1f); // ROM high=1, RAM high=3, low=3
    multiplexed->write_control(0x0000, 0x2a);
    multiplexed->write_control(0x0000, 0x6a);
    check(visible_rom_bank(*multiplexed, 0x0000) == 192 &&
          visible_rom_bank(*multiplexed, 0x4000) == 225,
          "mode multiplex MMM01 n'a pas échangé ROM Mid et RAM Low");
    multiplexed->write_ram(0xa000, 0x5a); // RAM high=3, ROM mid=2 => banque 14
    const auto battery = multiplexed->export_battery();
    check(battery.has_value() &&
          (*battery)[14U * Cartridge::ram_bank_size] == 0x5a,
          "multiplex MMM01 n'a pas utilisé ROM Mid pour la banque RAM");

    multiplexed->write_control(0x6000, 0x01);
    check(visible_rom_bank(*multiplexed, 0x0000) == 224,
          "mode avancé multiplex MMM01 ne commute pas la fenêtre ROM basse");
    multiplexed->write_control(0x2000, 0x62);
    check(visible_rom_bank(*multiplexed, 0x4000) == 226,
          "ROM Mid MMM01 modifié après mapping ou ROM Low non mis à jour");

    auto locked = create(make_mmm01());
    locked->write_control(0x2000, 0x01);
    locked->write_control(0x4000, 0x43); // RAM low=3 + verrou du mode
    locked->write_control(0x6000, 0x40); // multiplex, mode 0 conservé
    locked->write_control(0x0000, 0x40);
    locked->write_control(0x6000, 0x01); // tentative mode 1
    check(visible_rom_bank(*locked, 0x0000) == 0,
          "verrou d'écriture du mode MMM01 n'a pas bloqué le mode avancé");
}

void mmm01_save_state_validation_and_unsupported_test() {
    auto source = create(make_mmm01());
    source->write_control(0x2000, 0x41);
    source->write_control(0x6000, 0x40);
    source->write_control(0x4000, 0x1f);
    source->write_control(0x0000, 0x2a);
    source->write_control(0x0000, 0x6a);
    source->write_ram(0xa123, 0x7c);

    detail::BinaryWriter writer;
    source->save_state(writer);
    const auto state = std::move(writer).take();
    auto restored = create(make_mmm01());
    detail::BinaryReader reader(state);
    restored->load_state(reader);
    check(reader.exhausted() &&
          visible_rom_bank(*restored, 0x0000) == visible_rom_bank(*source, 0x0000) &&
          visible_rom_bank(*restored, 0x4000) == visible_rom_bank(*source, 0x4000) &&
          restored->read_ram(0xa123) == 0x7c,
          "état MMM01 n'a pas restauré mapping, verrous et RAM");

    auto incompatible = state;
    incompatible[3] = 2;
    expect_failure<ravenemu::SaveStateError>(
        [&] {
            auto invalid = create(make_mmm01());
            detail::BinaryReader invalid_reader(incompatible);
            invalid->load_state(invalid_reader);
        },
        "layout d'état MMM01 incompatible accepté silencieusement");

    auto invalid_mask = state;
    invalid_mask[11] = 1; // le bit 0 du masque ROM est physiquement forcé à zéro
    expect_failure<ravenemu::SaveStateError>(
        [&] {
            auto invalid = create(make_mmm01());
            detail::BinaryReader invalid_reader(invalid_mask);
            invalid->load_state(invalid_reader);
        },
        "masque ROM MMM01 impossible accepté dans un save state");

    std::vector<std::uint8_t> tama5(CartridgeHeader::min_rom_size);
    tama5[0x147] = 0xfd;
    expect_failure<ravenemu::RomLoadError>(
        [&] { static_cast<void>(create(std::move(tama5))); },
        "TAMA5 annoncé comme supporté avant son implémentation réelle");
}

void mbc3_rtc_progress_halt_and_carry_test() {
    std::int64_t epoch = 1'000;
    auto rtc = create_rtc(epoch);
    rtc_write(*rtc, 0x08, 58);
    rtc_write(*rtc, 0x09, 59);
    rtc_write(*rtc, 0x0a, 23);
    rtc_write(*rtc, 0x0b, 0);
    rtc_write(*rtc, 0x0c, 0);
    epoch += 3;
    rtc_latch(*rtc);
    check(rtc_read(*rtc, 0x08) == 1 && rtc_read(*rtc, 0x09) == 0 &&
          rtc_read(*rtc, 0x0a) == 0 && rtc_read(*rtc, 0x0b) == 1,
          "RTC MBC3 ne propage pas correctement seconde/minute/heure/jour");

    rtc_write(*rtc, 0x0c, 0x40); // halt
    epoch += 100'000;
    rtc_latch(*rtc);
    check(rtc_read(*rtc, 0x08) == 1 && (rtc_read(*rtc, 0x0c) & 0x40) != 0,
          "RTC MBC3 avance malgré le bit halt");
    rtc_write(*rtc, 0x0c, 0x00); // reprise sans rattraper la période suspendue
    epoch += 2;
    rtc_latch(*rtc);
    check(rtc_read(*rtc, 0x08) == 3,
          "RTC MBC3 rattrape le temps suspendu ou ne reprend pas après halt");

    rtc_write(*rtc, 0x08, 0); rtc_write(*rtc, 0x09, 0); rtc_write(*rtc, 0x0a, 0);
    rtc_write(*rtc, 0x0b, 0xff); rtc_write(*rtc, 0x0c, 0x01); // jour 511
    epoch += 86'400;
    rtc_latch(*rtc);
    check(rtc_read(*rtc, 0x0b) == 0 && (rtc_read(*rtc, 0x0c) & 0x81) == 0x80,
          "RTC MBC3 ne reboucle pas le jour 511 avec carry");
    rtc_write(*rtc, 0x0c, 0x00);
    rtc_latch(*rtc);
    check((rtc_read(*rtc, 0x0c) & 0x80) == 0, "carry RTC MBC3 impossible à effacer");
}

void mbc3_rtc_battery_and_state_test() {
    std::int64_t epoch = 10'000;
    auto source = create_rtc(epoch);
    rtc_write(*source, 0x08, 7);
    rtc_write(*source, 0x09, 6);
    rtc_write(*source, 0x0a, 5);
    rtc_latch(*source);

    const auto battery = source->export_battery();
    check(battery.has_value() && battery->size() == 32U * 1024U + 48U,
          "footer batterie RTC MBC3 absent ou de taille incorrecte");
    epoch += 60;
    auto imported = create_rtc(epoch);
    imported->import_battery(*battery);
    rtc_latch(*imported);
    const int imported_seconds = rtc_read(*imported, 0x08);
    const int imported_minutes = rtc_read(*imported, 0x09);
    const int imported_hours = rtc_read(*imported, 0x0a);
    if (imported_seconds != 7 || imported_minutes != 7 || imported_hours != 5) {
        throw std::runtime_error("RTC MBC3 persisté incorrect après arrêt : " +
            std::to_string(imported_hours) + ":" + std::to_string(imported_minutes) +
            ":" + std::to_string(imported_seconds));
    }

    detail::BinaryWriter writer;
    imported->save_state(writer);
    const auto state = std::move(writer).take();
    auto restored = create_rtc(epoch);
    detail::BinaryReader reader(state);
    restored->load_state(reader);
    rtc_latch(*restored);
    check(reader.exhausted() && rtc_read(*restored, 0x09) == 7,
          "save state RTC MBC3 non déterministe à horloge figée");

    epoch += 86'400LL * 1'025LL;
    rtc_latch(*restored);
    check(rtc_read(*restored, 0x0b) == 1 && (rtc_read(*restored, 0x0c) & 0x80) != 0,
          "RTC MBC3 échoue après une période supérieure à deux tours de compteur");
}

} // namespace ravenemu::cgb::testing

int main() {
    ravenemu::cgb::testing::mbc1m_detection_and_banking_test();
    ravenemu::cgb::testing::mbc1m_state_and_persistence_test();
    ravenemu::cgb::testing::mmm01_header_and_startup_mapping_test();
    ravenemu::cgb::testing::mmm01_non_multiplexed_banking_and_locks_test();
    ravenemu::cgb::testing::mmm01_ram_banking_persistence_and_open_bus_test();
    ravenemu::cgb::testing::mmm01_multiplex_and_mode_lock_test();
    ravenemu::cgb::testing::mmm01_save_state_validation_and_unsupported_test();
    ravenemu::cgb::testing::mbc3_rtc_progress_halt_and_carry_test();
    ravenemu::cgb::testing::mbc3_rtc_battery_and_state_test();
    return 0;
}
