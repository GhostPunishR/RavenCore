#include "cartridge/cartridge_factory.hpp"
#include "machine/machine.hpp"
#include "check.hpp"

#include <memory>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

std::vector<std::uint8_t> make_huc1_rom() {
    std::vector<std::uint8_t> rom(1U * 1024U * 1024U);
    for (int bank = 0; bank < 64; ++bank) {
        const auto offset = static_cast<std::size_t>(bank * Cartridge::rom_bank_size);
        rom[offset] = static_cast<std::uint8_t>(bank);
        rom[offset + 1U] = static_cast<std::uint8_t>(bank ^ 0x5a);
    }
    rom[0x143] = 0x80;
    rom[0x147] = 0xff;
    rom[0x148] = 0x05;
    rom[0x149] = 0x03;
    return rom;
}

RomImage make_huc1_image() {
    return std::make_shared<const std::vector<std::uint8_t>>(make_huc1_rom());
}

std::unique_ptr<Cartridge> create_huc1() {
    return Cartridge::create(make_huc1_image(), [] { return std::int64_t{}; });
}

class ProbeInfraredPort final : public InfraredPortEndpoint {
public:
    [[nodiscard]] bool infrared_led_on() const noexcept override { return output; }
    void set_infrared_light(bool detected) noexcept override { input = detected; }

    bool output{};
    bool input{};
};

void banking_and_always_enabled_ram_test() {
    auto cartridge = create_huc1();
    check(cartridge->header().mbc == MbcType::huc1 &&
          cartridge->header().has_ram && cartridge->header().has_battery &&
          cartridge->header().ram_size == 32 * 1024 &&
          cartridge->has_persistent_data(),
          "capacités d'en-tête HuC1 $FF incorrectes");
    check(cartridge->read_rom(0x0000) == 0 && cartridge->read_rom(0x4000) == 1,
          "banques HuC1 de mise sous tension incorrectes");

    cartridge->write_control(0x2000, 0x3f);
    check(cartridge->read_rom(0x4000) == 0x3f,
          "sixième bit de banque ROM HuC1 ignoré");
    cartridge->write_control(0x2000, 0x7f);
    check(cartridge->read_rom(0x4000) == 0x3f,
          "bits ROM HuC1 non documentés non masqués");
    cartridge->write_control(0x2000, 0x00);
    check(cartridge->read_rom(0x4000) == 0,
          "banque ROM zéro HuC1 non exposée par le registre direct");

    for (int bank = 0; bank < 4; ++bank) {
        cartridge->write_control(0x4000, bank);
        cartridge->write_ram(0xa123, 0x40 + bank);
    }
    for (int bank = 0; bank < 4; ++bank) {
        cartridge->write_control(0x4000, bank);
        check(cartridge->read_ram(0xa123) == 0x40 + bank,
              "banking RAM HuC1 incorrect sans registre d'activation");
    }

    cartridge->write_control(0x2000, 7);
    cartridge->write_control(0x4000, 2);
    cartridge->write_control(0x6000, 0xff);
    check(cartridge->read_rom(0x4000) == 7 && cartridge->read_ram(0xa123) == 0x42,
          "écriture $6000-$7FFF HuC1 a modifié le mapping");

    cartridge->write_control(0x0000, 0x0a);
    check(cartridge->read_ram(0xa123) == 0x42,
          "écriture MBC1 $0A a désactivé la RAM toujours accessible du HuC1");
    cartridge->write_control(0x0000, 0x1e);
    check(cartridge->read_ram(0xa123) == 0x42,
          "valeur autre que $0E entrée par erreur en mode IR HuC1");
}

void infrared_register_and_endpoint_test() {
    LocalInfraredEndpoint local;
    ProbeInfraredPort remote;
    check(local.attach(remote), "sonde IR HuC1 non attachée");
    auto cartridge = create_huc1();
    check(cartridge->connect_infrared_endpoint(&local),
          "transceiver de cartouche HuC1 non attaché");

    cartridge->write_control(0x0000, 0x0e);
    check(cartridge->read_ram(0xa000) == 0xc0 &&
          cartridge->read_ram(0xbfff) == 0xc0,
          "registre IR HuC1 sans lumière ou mirroring incorrect");

    remote.output = true;
    local.output_changed(remote);
    check(cartridge->read_ram(0xa555) == 0xc1,
          "récepteur HuC1 n'a pas vu la lumière externe");

    cartridge->write_ram(0xbfff, 0x01);
    check(remote.input, "LED HuC1 non propagée au backend IR");
    cartridge->write_ram(0xa000, 0x00);
    check(!remote.input, "extinction de la LED HuC1 non propagée");
    cartridge->write_ram(0xa000, 0xff);
    check(remote.input, "bits inutilisés de l'émetteur HuC1 ont masqué le bit 0");
    cartridge->write_ram(0xa000, 0xfe);
    check(!remote.input, "bits inutilisés de l'émetteur HuC1 ont forcé la LED");

    cartridge->disconnect_infrared_endpoint();
    check(!remote.input && cartridge->read_ram(0xa000) == 0xc0,
          "déconnexion HuC1 a conservé une lumière fantôme");
}

void persistence_and_save_state_test() {
    auto source = create_huc1();
    for (int bank = 0; bank < 4; ++bank) {
        source->write_control(0x4000, bank);
        source->write_ram(0xa010, 0x80 + bank);
    }
    const auto battery = source->export_battery();
    check(battery && battery->size() == 32U * 1024U &&
          (*battery)[0x0010] == 0x80 && (*battery)[0x6010] == 0x83,
          "persistance RAM HuC1 incorrecte");

    auto battery_restored = create_huc1();
    battery_restored->import_battery(*battery);
    battery_restored->write_control(0x4000, 3);
    check(!battery_restored->dirty() && battery_restored->read_ram(0xa010) == 0x83,
          "restauration batterie HuC1 incorrecte ou marquée sale");

    LocalInfraredEndpoint local;
    ProbeInfraredPort remote;
    remote.output = true;
    check(local.attach(remote) && source->connect_infrared_endpoint(&local),
          "précondition endpoint HuC1 du save state absente");
    source->write_control(0x2000, 0x25);
    source->write_control(0x4000, 2);
    source->write_control(0x0000, 0x0e);
    source->write_ram(0xa000, 1);
    check(source->read_ram(0xa000) == 0xc1 && remote.input,
          "précondition état IR HuC1 actif absente");

    detail::BinaryWriter writer;
    source->save_state(writer);
    const auto state = std::move(writer).take();

    LocalInfraredEndpoint restored_local;
    ProbeInfraredPort restored_remote;
    restored_remote.output = true;
    check(restored_local.attach(restored_remote), "sonde IR restaurée non attachée");
    auto restored = create_huc1();
    check(restored->connect_infrared_endpoint(&restored_local),
          "endpoint HuC1 restauré non attaché");
    detail::BinaryReader reader(state);
    restored->load_state(reader);
    check(reader.exhausted() && restored->read_rom(0x4000) == 0x25 &&
          restored->read_ram(0xa000) == 0xc1 && restored_remote.input,
          "save state HuC1 n'a pas restauré mapping, IR et RAM");

    auto invalid_layout = state;
    invalid_layout[3] = 2;
    expect_failure<SaveStateError>([&] {
        auto rejected = create_huc1();
        detail::BinaryReader input(invalid_layout);
        rejected->load_state(input);
    }, "layout HuC1 incompatible accepté");

    auto invalid_bool = state;
    invalid_bool[4] = 2;
    expect_failure<SaveStateError>([&] {
        auto rejected = create_huc1();
        detail::BinaryReader input(invalid_bool);
        rejected->load_state(input);
    }, "booléen HuC1 corrompu accepté");

    auto invalid_bank = state;
    invalid_bank[8] = 0x40;
    expect_failure<SaveStateError>([&] {
        auto rejected = create_huc1();
        detail::BinaryReader input(invalid_bank);
        rejected->load_state(input);
    }, "banque ROM HuC1 hors plage acceptée");

    // Le transport appartient à l'appelant et doit vivre plus longtemps que
    // les ports qui lui sont attachés. Le source a été créé avant `local`.
    source->disconnect_infrared_endpoint();
}

void machine_ir_router_test() {
    // Comme dans l'API publique, le backend externe englobe la durée de vie
    // des machines connectées.
    LocalInfraredEndpoint local;
    Machine first(make_huc1_image(), [] { return std::int64_t{}; },
                  gb::HardwareMode::dmg);
    Machine second(make_huc1_image(), [] { return std::int64_t{}; },
                   gb::HardwareMode::cgb_native);
    check(first.infrared_router.connect(&local) && second.infrared_router.connect(&local),
          "machines HuC1 DMG/CGB ne partagent pas le backend IR");
    first.bus.write(0x0000, 0x0e);
    second.bus.write(0x0000, 0x0e);
    check(first.bus.read(0xff56) == 0xff,
          "RP interne actif par erreur sur le matériel DMG HuC1");

    second.bus.write(0xff56, 0xc0); // récepteur CGB actif, LED éteinte
    first.bus.write(0xa000, 1);     // LED de cartouche HuC1
    check((second.bus.read(0xff56) & 0x02) == 0 &&
          second.bus.read(0xa000) == 0xc1,
          "LED HuC1 non distribuée aux ports CGB et cartouche distants");
    check(first.bus.read(0xa000) == 0xc0,
          "multiplexeur IR a renvoyé la LED HuC1 vers sa propre machine");

    first.bus.write(0xa000, 0);
    check((second.bus.read(0xff56) & 0x02) != 0,
          "extinction HuC1 non visible par le port CGB distant");
    second.bus.write(0xff56, 0xc1); // LED CGB
    check(first.bus.read(0xa000) == 0xc1,
          "LED CGB non distribuée au récepteur HuC1 distant");
    second.bus.write(0xff56, 0xc0);
    check(first.bus.read(0xa000) == 0xc0,
          "extinction CGB non visible par le récepteur HuC1");

    LocalInfraredEndpoint compatibility_local;
    ProbeInfraredPort remote;
    check(compatibility_local.attach(remote),
          "sonde IR de compatibilité CGB non attachée");
    Machine compatibility(make_huc1_image(), [] { return std::int64_t{}; },
                          gb::HardwareMode::cgb_compatibility);
    check(compatibility.infrared_router.connect(&compatibility_local),
          "HuC1 en compatibilité CGB non attaché au backend IR");
    compatibility.bus.write(0x0000, 0x0e);
    check(compatibility.bus.read(0xff56) == 0xff,
          "RP accessible par erreur au logiciel en compatibilité CGB");
    remote.output = true;
    compatibility_local.output_changed(remote);
    check(compatibility.bus.read(0xa000) == 0xc1,
          "récepteur HuC1 inactif en compatibilité CGB");
    compatibility.bus.write(0xa000, 1);
    check(remote.input,
          "LED HuC1 inactive en compatibilité CGB");
}

} // namespace ravenemu::cgb::testing

int main() {
    ravenemu::cgb::testing::banking_and_always_enabled_ram_test();
    ravenemu::cgb::testing::infrared_register_and_endpoint_test();
    ravenemu::cgb::testing::persistence_and_save_state_test();
    ravenemu::cgb::testing::machine_ir_router_test();
}
