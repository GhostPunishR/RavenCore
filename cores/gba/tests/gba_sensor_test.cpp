#include "machine/machine.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Capteurs de cartouche GBA, pilotés par le bus comme le font les jeux. Les
 * routines reprennent les exemples de GBATEK (« GBA Cart Solar Sensor »,
 * « Gyro Sensor » et « Tilt Sensor »), si bien que les valeurs attendues sont
 * celles qu'un jeu lirait.
 */
namespace ravenemu::gba::testing {

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {

constexpr std::int32_t data_register = 0x0800'00c4;
constexpr std::int32_t direction_register = 0x0800'00c6;
constexpr std::int32_t control_register = 0x0800'00c8;
constexpr std::int32_t tilt_base = 0x0e00'8000;

/** ROM GBA minimale au code de jeu donné. */
std::vector<std::uint8_t> rom_with_code(std::string_view code) {
    auto rom = ravenemu::testing::minimal_gba_rom();
    std::copy(code.begin(), code.end(), rom.begin() + 0xac);
    return rom;
}

class Fixture {
public:
    explicit Fixture(std::string_view code)
        : image_(std::make_shared<const std::vector<std::uint8_t>>(rom_with_code(code))),
          machine_(image_, RomTraits::detect(*image_), std::nullopt, std::nullopt, [] { return std::int64_t{0}; }) {}

    Bus& bus() noexcept { return machine_.bus; }
    Cartridge& cartridge() noexcept { return machine_.cartridge; }
    void input(SensorInput value) { machine_.cartridge.set_sensor_input(value); }

private:
    RomImage image_;
    Machine machine_;
};

/** Mesure du capteur solaire, comme la routine d'exemple de GBATEK. */
int read_solar(Bus& bus) {
    bus.write16(control_register, 1);
    bus.write16(direction_register, 7);
    bus.write16(data_register, 2); // remise à zéro du compteur
    bus.write16(data_register, 0);
    int level = 0;
    while (true) {
        bus.write16(data_register, 1); // impulsion d'horloge
        bus.write16(data_register, 0);
        if ((bus.read16(data_register) & 8) != 0 || ++level == 0x100) return level;
    }
}

/** Lecture du gyroscope, comme `read_gyro` dans GBATEK : 16 bits, MSB d'abord. */
int read_gyro(Bus& bus) {
    bus.write16(control_register, 1);
    bus.write16(direction_register, 0x0b);
    auto pins = bus.read16(data_register) | 3; // garde la broche 3 (vibreur)
    bus.write16(data_register, pins);           // lance la conversion
    pins &= ~1;
    bus.write16(data_register, pins);
    pins &= ~2;
    int stream = 0;
    for (int bit = 0; bit < 16; ++bit) {
        const auto data = (bus.read16(data_register) >> 2) & 1;
        bus.write16(data_register, pins);     // horloge basse
        stream = stream << 1 | data;
        bus.write16(data_register, pins | 2); // horloge haute
    }
    check((stream & 0xf000) == 0, "gyroscope : les 4 premiers bits doivent être nuls");
    return stream & 0x0fff;
}

struct Tilt { int x; int y; bool ready; };
Tilt read_tilt(Bus& bus) {
    const auto byte = [&](int offset) { return bus.read8(tilt_base + offset); };
    return {(byte(0x300) & 0x0f) << 8 | byte(0x200), (byte(0x500) & 0x0f) << 8 | byte(0x400),
            (byte(0x300) & 0x80) != 0};
}
void start_tilt(Bus& bus) {
    bus.write8(tilt_base, 0x55);
    bus.write8(tilt_base + 0x100, 0xaa);
}

void detection_test() {
    struct Case { std::string_view code; bool solar; bool rotation; bool tilt; };
    for (const auto& item : std::array<Case, 8>{{
             {"U3IE", true, false, false}, {"U32J", true, false, false}, {"U33J", true, false, false},
             {"RZWE", false, true, false}, {"KYGE", false, false, true}, {"KYGP", false, false, true},
             {"KHPJ", false, false, true}, {"AAAE", false, false, false}}}) {
        auto core = ravenemu::make_gba_core(std::nullopt);
        core->load_rom(rom_with_code(item.code), {});
        const auto sensors = core->cartridge_sensors();
        check(sensors.solar == item.solar && sensors.rotation == item.rotation && sensors.tilt == item.tilt,
              "capteurs mal reconnus pour " + std::string{item.code});
    }
    check(!ravenemu::make_gba_core(std::nullopt)->cartridge_sensors().tilt, "capteur annoncé sans jeu chargé");
}

void solar_sensor_test() {
    Fixture boktai("U3IE");
    // Valeurs relevées par GBATEK : E8h dans l'obscurité, 50h au maximum de la jauge.
    boktai.input({.light = 0});
    check(read_solar(boktai.bus()) == SolarSensor::darkness, "obscurité mal mesurée");
    boktai.input({.light = 255});
    check(read_solar(boktai.bus()) == SolarSensor::full_sun, "plein soleil mal mesuré");
    boktai.input({.light = 128});
    const auto half = read_solar(boktai.bus());
    check(half > SolarSensor::full_sun && half < SolarSensor::darkness, "lumière intermédiaire hors plage");

    // La remise à zéro, active à l'état haut, bloque le compteur.
    auto& bus = boktai.bus();
    bus.write16(data_register, 2);
    for (int pulse = 0; pulse < 0xff; ++pulse) { bus.write16(data_register, 3); bus.write16(data_register, 2); }
    check((bus.read16(data_register) & 8) == 0, "compteur avancé pendant la remise à zéro");
    // Compteur au-delà de la photodiode, puis broche 3 passée en sortie : le
    // jeu relit ce qu'il écrit, pas le comparateur.
    bus.write16(data_register, 0);
    for (int pulse = 0; pulse < 0xff; ++pulse) { bus.write16(data_register, 1); bus.write16(data_register, 0); }
    check((bus.read16(data_register) & 8) != 0, "comparateur resté bas");
    bus.write16(direction_register, 0x0f);
    check((bus.read16(data_register) & 8) == 0, "comparateur visible sur une broche en sortie");
    bus.write16(direction_register, 7);
    check((bus.read16(data_register) & 8) != 0, "comparateur perdu au retour en entrée");

    Fixture plain("AAAE");
    check(plain.cartridge().gpio() == nullptr, "port GPIO créé sans composant");
}

void solar_with_clock_test() {
    // Boktai porte aussi l'horloge Seiko sur les mêmes broches : les deux répondent.
    auto image = std::make_shared<const std::vector<std::uint8_t>>(rom_with_code("U3IE"));
    Machine machine(image, RomTraits::detect(*image), std::nullopt, true, [] { return std::int64_t{0}; });
    check(machine.cartridge.gpio()->has_rtc(), "horloge absente malgré le forçage");
    machine.cartridge.set_sensor_input({.light = 255});
    check(read_solar(machine.bus) == SolarSensor::full_sun, "capteur solaire muet à côté de l'horloge");
}

void gyro_sensor_test() {
    Fixture wario("RZWE");
    // Valeurs relevées par GBATEK : 6C0h au repos, 9E3h et 354h en rotation brutale.
    check(read_gyro(wario.bus()) == GyroSensor::at_rest, "gyroscope au repos mal lu");
    wario.input({.rotation = 32767});
    check(read_gyro(wario.bus()) == GyroSensor::fastest_clockwise, "rotation horaire mal lue");
    wario.input({.rotation = -32768});
    check(read_gyro(wario.bus()) == GyroSensor::fastest_anticlockwise, "rotation antihoraire mal lue");
    wario.input({.rotation = 1000});
    const auto slow = read_gyro(wario.bus());
    check(slow > GyroSensor::at_rest && slow < GyroSensor::fastest_clockwise, "rotation lente hors plage");

    // La mesure est figée au lancement de la conversion.
    auto& bus = wario.bus();
    bus.write16(data_register, 3);
    bus.write16(data_register, 2);
    wario.input({.rotation = -32768});
    int stream = 0;
    for (int bit = 0; bit < 16; ++bit) {
        stream = stream << 1 | ((bus.read16(data_register) >> 2) & 1);
        bus.write16(data_register, 0);
        bus.write16(data_register, 2);
    }
    check(stream == slow, "mesure changée pendant la lecture");

    // Le vibreur de la broche 3 reste commandé pendant la lecture.
    bus.write16(data_register, 8);
    check(wario.cartridge().gpio()->rumble_active(), "vibreur éteint");
    static_cast<void>(read_gyro(bus));
    check(wario.cartridge().gpio()->rumble_active(), "lecture du gyroscope : vibreur coupé");
}

void tilt_sensor_test() {
    Fixture yoshi("KYGE");
    auto& bus = yoshi.bus();
    auto tilt = read_tilt(bus);
    check(tilt.x == TiltSensor::center_x && tilt.y == TiltSensor::center_y && tilt.ready,
          "capteur d'inclinaison au repos mal lu");

    yoshi.input({.tilt_x = 32767, .tilt_y = -32768});
    tilt = read_tilt(bus);
    check(tilt.x == TiltSensor::center_x, "mesure prise sans conversion");
    start_tilt(bus);
    tilt = read_tilt(bus);
    check(tilt.x == TiltSensor::center_x + 0xdf && tilt.y == TiltSensor::center_y - TiltSensor::units_per_g,
          "inclinaison maximale mal lue");

    // Sans 55h d'abord, AAh ne lance rien.
    yoshi.input({});
    bus.write8(tilt_base + 0x100, 0xaa);
    check(read_tilt(bus).x == TiltSensor::center_x + 0xdf, "conversion lancée sans 55h");
    start_tilt(bus);
    check(read_tilt(bus).x == TiltSensor::center_x, "conversion suivante ignorée");

    // Sur une autre cartouche, ces adresses restent de la SRAM.
    Fixture plain("AAAE");
    plain.bus().write8(tilt_base + 0x200, 0x12);
    check(plain.bus().read8(tilt_base + 0x200) == 0x12, "SRAM détournée sans capteur");
}

void sensor_state_validation_test() {
    Fixture wario("RZWE");
    auto valid = wario.cartridge().export_sensor_state();
    wario.cartridge().import_sensor_state(valid);
    for (const auto index : {0, 3, 4, 7, 8, 9}) {
        auto corrupt = valid;
        corrupt[static_cast<std::size_t>(index)] = 0x2000;
        expect_failure<SaveStateError>([&] { wario.cartridge().import_sensor_state(corrupt); },
                                       "état de capteur hors limites accepté");
    }
}

/**
 * Programme qui lance une mesure d'inclinaison, puis recopie sans fin l'octet
 * bas de X en IWRAM : après rechargement d'un état, ce qui est recopié vient
 * du capteur restauré, sans nouvelle conversion.
 */
std::vector<std::uint8_t> tilt_program_rom() {
    auto rom = rom_with_code("KYGE");
    const std::array<std::uint32_t, 11> program{
        0xe59f001cU, // ldr  r0, =0x0E008000
        0xe3a01055U, // mov  r1, #0x55
        0xe5c01000U, // strb r1, [r0]
        0xe3a010aaU, // mov  r1, #0xAA
        0xe5c01100U, // strb r1, [r0, #0x100]
        0xe59f200cU, // ldr  r2, =0x03000000
        0xe5d01200U, // ldrb r1, [r0, #0x200]
        0xe5c21000U, // strb r1, [r2]
        0xeafffffcU, // b    (ldrb)
        0x0e008000U,
        0x03000000U,
    };
    for (std::size_t index = 0; index < program.size(); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[index * 4U + byte] = static_cast<std::uint8_t>(program[index] >> (byte * 8U));
    return rom;
}

void sensor_save_state_test() {
    const auto rom = tilt_program_rom();
    std::vector<std::int32_t> frame(240U * 160U);
    const auto copied_x = [](const ravenemu::Core& core) {
        std::array<std::uint8_t, 1> out{};
        check(core.read_achievement_memory(0, out) == 1, "IWRAM illisible");
        return out[0];
    };

    auto first = ravenemu::make_gba_core(std::nullopt);
    first->set_gba_tilt(32767, 0); // fourni avant le chargement : gardé par le cœur
    first->load_rom(rom, {});
    first->run_frame(frame, false);
    constexpr auto tilted_low = (TiltSensor::center_x + 0xdf) & 0xff;
    check(copied_x(*first) == tilted_low, "inclinaison donnée avant le chargement perdue");
    const auto state = first->save_state();

    auto second = ravenemu::make_gba_core(std::nullopt);
    second->load_rom(rom, {});
    second->load_state(state);
    second->run_frame(frame, false);
    check(copied_x(*second) == tilted_low, "mesure du capteur perdue par l'état");

    // Format 14 : le bloc du préchargement et des timers suit celui des
    // capteurs (tête, compte, progression, activité, quatre délais), puis
    // l'état du câble link. Format 15 : quatre recharges de timer en attente,
    // l'âge de la ligne d'IRQ et le temps restant d'une fonction du BIOS.
    // Format 16 : retour d'un Halt du BIOS, puis SIOMULTI0-3.
    constexpr std::size_t prefetch_bytes = 4U + 4U + 4U + 1U + 4U * 4U + 4U * Serial::link_state_words;
    constexpr std::size_t irq_bytes = 4U * 4U + 4U + 2U * 4U;
    constexpr std::size_t format16_bytes = 4U + 4U * 4U;
    constexpr std::size_t format17_bytes = 1U + 32U;  // type de BIOS et empreinte
    check(state[4] == 0 && state[5] == 17, "version d'état GBA inattendue");

    // État au format 16 : sans le bloc du format 17, donc pris avec le BIOS réécrit.
    std::vector<std::uint8_t> legacy(state.begin(), state.end() - static_cast<std::ptrdiff_t>(format17_bytes));
    legacy[5] = 16;
    second->load_state(legacy);
    second->run_frame(frame, false);
    check(copied_x(*second) == tilted_low, "état 16 : mesure du capteur perdue");

    // État au format 15 : sans le bloc du format 16 non plus.
    legacy.resize(legacy.size() - format16_bytes);
    legacy[5] = 15;
    second->load_state(legacy);
    second->run_frame(frame, false);
    check(copied_x(*second) == tilted_low, "état 15 : mesure du capteur perdue");

    // État au format 14 : sans le bloc du format 15 non plus.
    legacy.resize(legacy.size() - irq_bytes);
    legacy[5] = 14;
    second->load_state(legacy);
    second->run_frame(frame, false);
    check(copied_x(*second) == tilted_low, "état 14 : mesure du capteur perdue");

    // État au format 13 : sans le bloc du format 14 non plus.
    legacy.resize(legacy.size() - prefetch_bytes);
    legacy[5] = 13;
    second->load_state(legacy);
    second->run_frame(frame, false);
    check(copied_x(*second) == tilted_low, "état 13 : mesure du capteur perdue");

    // État au format 12 : sans le bloc des capteurs non plus. Il se recharge,
    // capteur à son état initial.
    constexpr std::size_t sensor_bytes = Cartridge::sensor_state_words * 4U;
    legacy.resize(legacy.size() - sensor_bytes);
    legacy[5] = 12;
    second->load_state(legacy);
    second->run_frame(frame, false);
    check(copied_x(*second) == (TiltSensor::center_x & 0xff), "état 12 : capteur non réinitialisé");

    legacy[5] = 17;
    expect_failure<SaveStateError>([&] { second->load_state(legacy); }, "version d'état future acceptée");
}

} // namespace
} // namespace ravenemu::gba::testing

int main() {
    using namespace ravenemu::gba::testing;
    detection_test();
    solar_sensor_test();
    solar_with_clock_test();
    gyro_sensor_test();
    tilt_sensor_test();
    sensor_state_validation_test();
    sensor_save_state_test();
    return 0;
}
