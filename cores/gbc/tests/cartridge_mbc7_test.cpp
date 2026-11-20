#include "cartridge/cartridge_factory.hpp"
#include "check.hpp"

#include <memory>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

std::unique_ptr<Cartridge> create_mbc7() {
    std::vector<std::uint8_t> rom(2U * 1024U * 1024U);
    for (int bank = 0; bank < 128; ++bank) {
        rom[static_cast<std::size_t>(bank * Cartridge::rom_bank_size)] =
            static_cast<std::uint8_t>(bank);
    }
    rom[0x143] = 0xc0; rom[0x147] = 0x22;
    auto image = std::make_shared<const std::vector<std::uint8_t>>(std::move(rom));
    return Cartridge::create(std::move(image), [] { return std::int64_t{}; });
}

void enable_registers(Cartridge& cartridge, int second = 0x40) {
    cartridge.write_control(0x0000, 0x0a);
    cartridge.write_control(0x4000, second);
}

void pins(Cartridge& cartridge, int value) { cartridge.write_ram(0xa080, value); }

void clock_bit(Cartridge& cartridge, bool bit) {
    const int base = 0x80 | (bit ? 0x02 : 0);
    pins(cartridge, base); pins(cartridge, base | 0x40); pins(cartridge, base);
}

void begin_command(Cartridge& cartridge, int command) {
    pins(cartridge, 0x00); pins(cartridge, 0x80);
    clock_bit(cartridge, true); // start
    for (int bit = 9; bit >= 0; --bit) clock_bit(cartridge, ((command >> bit) & 1) != 0);
}

void send_data(Cartridge& cartridge, int value, int bits = 16) {
    for (int bit = bits - 1; bit >= 0; --bit) clock_bit(cartridge, ((value >> bit) & 1) != 0);
}

int read_word(Cartridge& cartridge, int address) {
    begin_command(cartridge, 0x200 | (address & 0x7f));
    int result{};
    for (int bit = 0; bit < 16; ++bit) {
        pins(cartridge, 0x80); pins(cartridge, 0xc0);
        result = (result << 1) | (cartridge.read_ram(0xa080) & 1);
        pins(cartridge, 0x80);
    }
    pins(cartridge, 0x00);
    return result;
}

std::uint32_t read_two_words(Cartridge& cartridge, int address) {
    begin_command(cartridge, 0x200 | (address & 0x7f));
    std::uint32_t result{};
    for (int bit = 0; bit < 32; ++bit) {
        pins(cartridge, 0x80); pins(cartridge, 0xc0);
        result = (result << 1U) |
            static_cast<std::uint32_t>(cartridge.read_ram(0xa080) & 1);
        pins(cartridge, 0x80);
    }
    pins(cartridge, 0x00);
    return result;
}

void finish_busy(Cartridge& cartridge) {
    check((cartridge.read_ram(0xa080) & 1) == 0,
          "EEPROM MBC7 ne signale pas son cycle d'écriture occupé");
    cartridge.tick(Mbc7Eeprom::write_busy_dots - 1);
    check((cartridge.read_ram(0xa080) & 1) == 0,
          "EEPROM MBC7 termine son écriture trop tôt");
    cartridge.tick(1);
    check((cartridge.read_ram(0xa080) & 1) == 1,
          "EEPROM MBC7 ne publie pas RDY après le délai d'écriture");
    pins(cartridge, 0x00);
}

void mapping_and_sensor_test() {
    auto cartridge = create_mbc7();
    check(cartridge->header().mbc == MbcType::mbc7 &&
          cartridge->header().has_ram && cartridge->header().has_battery &&
          cartridge->header().ram_size == 256 && cartridge->has_persistent_data(),
          "capacités MBC7 du type $22 incorrectes");
    cartridge->write_control(0x2000, 0x85);
    check(cartridge->read_rom(0x4000) == 5, "masque de banque ROM MBC7 incorrect");
    check(cartridge->read_ram(0xa020) == 0xff, "registres MBC7 actifs au reset");
    enable_registers(*cartridge, 0x41);
    check(cartridge->read_ram(0xa020) == 0xff,
          "seconde porte MBC7 acceptée avec une valeur autre que $40");
    enable_registers(*cartridge);
    check(cartridge->read_ram(0xa000) == 0xff && cartridge->read_ram(0xa010) == 0xff,
          "registres write-only du latch MBC7 ne lisent pas $FF");
    check(cartridge->read_ram(0xa020) == 0x00 && cartridge->read_ram(0xa030) == 0x80,
          "valeur accéléromètre MBC7 avant latch incorrecte");
    pins(*cartridge, 0xc2);
    check(cartridge->read_ram(0xa080) == 0xc3,
          "broches CS/CLK/DI/DO ou bits inutilisés MBC7 mal relus");
    pins(*cartridge, 0x00);
    cartridge->set_acceleration(0x70, -0x70);
    cartridge->write_ram(0xaf0f, 0x55);
    cartridge->write_ram(0xa11f, 0xaa);
    check(cartridge->read_ram(0xa02a) == 0x40 && cartridge->read_ram(0xa03b) == 0x82 &&
          cartridge->read_ram(0xa04c) == 0x60 && cartridge->read_ram(0xa05d) == 0x81,
          "latch ou miroirs des axes MBC7 incorrects");
    cartridge->set_acceleration(0, 0);
    cartridge->write_ram(0xa010, 0xaa);
    check(cartridge->read_ram(0xa020) == 0x40,
          "accéléromètre MBC7 relatché sans séquence $55/$AA");
    check(cartridge->read_ram(0xa060) == 0x00 && cartridge->read_ram(0xa070) == 0xff &&
          cartridge->read_ram(0xa090) == 0xff && cartridge->read_ram(0xb080) == 0xff,
          "valeurs réservées MBC7 incorrectes");
    cartridge->write_control(0x0000, 0x00);
    check(cartridge->read_ram(0xa020) == 0xff,
          "première porte MBC7 n'a pas désactivé les registres");
}

void eeprom_commands_and_persistence_test() {
    auto cartridge = create_mbc7(); enable_registers(*cartridge);
    constexpr int address = 0x25;
    begin_command(*cartridge, 0x100 | address); send_data(*cartridge, 0x1234);
    check(read_word(*cartridge, address) == 0xffff,
          "WRITE EEPROM MBC7 accepté avant EWEN");

    begin_command(*cartridge, 0x0c0); pins(*cartridge, 0x00); // EWEN
    begin_command(*cartridge, 0x100 | address); send_data(*cartridge, 0x1234);
    finish_busy(*cartridge);
    check(read_word(*cartridge, address) == 0x1234,
          "WRITE/READ EEPROM MBC7 n'est pas MSB-first");

    begin_command(*cartridge, 0x300 | address); finish_busy(*cartridge);
    check(read_word(*cartridge, address) == 0xffff, "ERASE EEPROM MBC7 incomplet");
    begin_command(*cartridge, 0x040); send_data(*cartridge, 0x55aa); finish_busy(*cartridge);
    check(read_word(*cartridge, 0) == 0x55aa && read_word(*cartridge, 0x7f) == 0x55aa,
          "WRAL EEPROM MBC7 incomplet");
    begin_command(*cartridge, 0x080); finish_busy(*cartridge);
    check(read_word(*cartridge, 0) == 0xffff && read_word(*cartridge, 0x7f) == 0xffff,
          "ERAL EEPROM MBC7 incomplet");

    begin_command(*cartridge, 0x100 | 3); send_data(*cartridge, 0xabcd); finish_busy(*cartridge);
    begin_command(*cartridge, 0x100 | 4); send_data(*cartridge, 0x5678); finish_busy(*cartridge);
    check(read_two_words(*cartridge, 3) == 0xabcd5678U,
          "lecture séquentielle EEPROM MBC7 ne passe pas au mot suivant");
    begin_command(*cartridge, 0x000); pins(*cartridge, 0x00); // EWDS
    begin_command(*cartridge, 0x100 | 5); send_data(*cartridge, 0x1111);
    check(read_word(*cartridge, 5) == 0xffff,
          "EWDS EEPROM MBC7 n'a pas reverrouillé les écritures");
    const auto battery = cartridge->export_battery();
    check(battery && battery->size() == 256 && (*battery)[6] == 0xab && (*battery)[7] == 0xcd,
          "persistance EEPROM MBC7 incorrecte");
    cartridge->import_battery(std::vector<std::uint8_t>(255, 0));
    check(cartridge->export_battery() == battery && cartridge->dirty(),
          "import EEPROM MBC7 tronqué accepté ou saleté acquittée");
    auto restored = create_mbc7(); restored->import_battery(*battery); enable_registers(*restored);
    check(!restored->dirty() && read_word(*restored, 3) == 0xabcd,
          "EEPROM MBC7 non restaurée ou marquée sale à l'import");
}

void save_state_mid_command_test() {
    auto source = create_mbc7(); enable_registers(*source);
    begin_command(*source, 0x0c0); pins(*source, 0x00);
    begin_command(*source, 0x100 | 9); send_data(*source, 0xbe, 8);
    detail::BinaryWriter writer; source->save_state(writer);
    const auto state = std::move(writer).take();
    auto restored = create_mbc7(); detail::BinaryReader reader(state); restored->load_state(reader);
    send_data(*restored, 0xef, 8);
    restored->tick(1'234);
    detail::BinaryWriter busy_writer; restored->save_state(busy_writer);
    const auto busy_state = std::move(busy_writer).take();
    auto busy_restored = create_mbc7();
    detail::BinaryReader busy_reader(busy_state); busy_restored->load_state(busy_reader);
    check((busy_restored->read_ram(0xa080) & 1) == 0,
          "save state MBC7 a perdu le signal EEPROM occupé");
    busy_restored->tick(Mbc7Eeprom::write_busy_dots - 1'235);
    check((busy_restored->read_ram(0xa080) & 1) == 0,
          "save state MBC7 a raccourci le délai EEPROM");
    busy_restored->tick(1);
    check((busy_restored->read_ram(0xa080) & 1) == 1,
          "save state MBC7 n'a pas repris le délai EEPROM");
    pins(*busy_restored, 0x00);
    check(reader.exhausted() && busy_reader.exhausted() &&
          read_word(*busy_restored, 9) == 0xbeef,
          "save state MBC7 n'a pas restauré la commande EEPROM partielle");

    auto invalid = state; invalid[3] = 2;
    expect_failure<SaveStateError>([&] {
        auto rejected = create_mbc7(); detail::BinaryReader input(invalid); rejected->load_state(input);
    }, "layout MBC7 incompatible accepté");
}

/**
 * Une cartouche MBC7 n'a pas de RAM d'octets.
 *
 * Son en-tête en annonce pourtant, et la classe de base alloue ce tampon pour
 * tous les mappeurs. Le MBC7 ne l'écrit jamais : ses sauvegardes vivent dans
 * une EEPROM série, et son espace 0xA000 répond à des registres, pas à de la
 * mémoire. Un tampon de zéros servi comme s'il était de la mémoire de
 * cartouche est pire qu'une absence : le lecteur qui s'en sert ne peut plus
 * distinguer une région qu'il ne sait pas lire d'une région pleine de zéros,
 * et une condition « cet octet vaut zéro » y est vraie en permanence.
 *
 * La fenêtre des succès doit donc être vide ici, pour que la lecture s'arrête
 * et se déclare incomplète.
 */
void achievement_memory_is_not_invented_test() {
    auto cartridge = create_mbc7();
    check(cartridge->achievement_ram().empty(),
          "MBC7 expose une RAM de cartouche que le mappeur n'écrit jamais");

    // Le contraste : le MBC6 porte réellement 32 Kio de SRAM dans ce même
    // tampon, et la restreindre pour tout le monde le priverait de la sienne.
    std::vector<std::uint8_t> rom(2U * 1024U * 1024U);
    rom[0x143] = 0xc0; rom[0x147] = 0x20;
    auto mbc6 = Cartridge::create(
        std::make_shared<const std::vector<std::uint8_t>>(std::move(rom)), [] { return std::int64_t{}; });
    check(mbc6->achievement_ram().size() == 32U * 1024U, "MBC6 a perdu la SRAM qu'il porte vraiment");
}

} // namespace ravenemu::cgb::testing

int main() {
    ravenemu::cgb::testing::mapping_and_sensor_test();
    ravenemu::cgb::testing::eeprom_commands_and_persistence_test();
    ravenemu::cgb::testing::save_state_mid_command_test();
    ravenemu::cgb::testing::achievement_memory_is_not_invented_test();
}
