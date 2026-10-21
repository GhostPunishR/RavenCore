#include <ravenemu/nds/firmware.hpp>
#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/firmware_fixture.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t control = 0x040001c0, data = control + 2, flags = 0x04000214;
constexpr std::uint32_t spi_irq = 1U << 23U;

namespace {
void image_and_settings_test() {
    const Firmware absent;
    check(absent.bytes().empty() && !absent.settings(), "firmware inventé par défaut");
    for (const auto size : {0U,1U,0x1000U,0x20000U,0x3ffffU,0x40001U,0x100000U}) {
        const std::vector<std::uint8_t> bytes(size);
        expect_failure<ravenemu::RomLoadError>([&] { static_cast<void>(Firmware{bytes}); }, "taille firmware invalide acceptée");
    }
    auto bytes = firmware();
    Firmware owned{bytes}; bytes[0] = 0; bytes[0x3fe02] = 0;
    check(owned.bytes().size() == 0x40000 && owned.bytes()[0] == 0x5a && owned.settings() &&
          owned.settings()->offset == 0x3fe00 && owned.settings()->data[2] == 3 && owned.settings()->counter == 10 &&
          owned.settings()->touch_calibration, "propriété de l'image ou sélection des réglages");

    for (const auto counters : {std::array<unsigned,3>{10,11,1},{127,0,1},{0,127,0},{10,10,0},{10,17,0}}) {
        bytes = firmware(); put16(bytes,0x3fe70,static_cast<std::uint16_t>(counters[0]));
        put16(bytes,0x3ff70,static_cast<std::uint16_t>(counters[1]));
        const Firmware selected{bytes};
        check(selected.settings() && selected.settings()->offset == 0x3fe00U + counters[2] * 0x100U,
              "compteur de réglages modulo 128");
    }
    bytes = firmware(); bytes[0x3fe02] ^= 1;
    check(Firmware{bytes}.settings()->offset == 0x3ff00, "CRC invalide de la première copie non détecté");
    bytes[0x3ff72] ^= 1;
    check(!Firmware{bytes}.settings(), "deux CRC invalides acceptés");
    bytes = firmware(); bytes[0x3ff73] ^= 1;
    check(Firmware{bytes}.settings()->offset == 0x3fe00, "copie valide écartée pour un mauvais CRC voisin");
    put16(bytes,0x3fe70,0x80);
    check(!Firmware{bytes}.settings(), "compteur hors de 0..127 accepté");
    bytes = firmware(); put16(bytes,0x3fe00,4); seal(bytes,0x3fe00);
    check(Firmware{bytes}.settings()->offset == 0x3ff00, "version de réglages inconnue acceptée");
    bytes = firmware(0x80000,0x7fe00);
    check(Firmware{bytes}.settings()->offset == 0x7fe00, "réglages du firmware 512 Kio tronqués");
    bytes = firmware(0x80000,0x50000);
    check(Firmware{bytes}.settings()->offset == 0x50000, "offset d'en-tête ignoré au profit de la fin d'image");
    for (const auto offset : {0U,0x20U,0x3ff08U,0x7fff8U}) {
        bytes = firmware(); put16(bytes,0x20,static_cast<std::uint16_t>(offset / 8));
        check(!Firmware{bytes}.settings(), "réglages hors image ou dans l'en-tête acceptés");
    }
    // Une image peut être lue sans réglages utilisables, sans inventer d'étalonnage.
    bytes = firmware(); put16(bytes,0x3fe58,0xffff); seal(bytes,0x3fe00);
    const Firmware invalid_calibration{bytes};
    check(invalid_calibration.settings() && !invalid_calibration.settings()->touch_calibration &&
          invalid_calibration.settings()->data[2] == 3, "mauvais étalonnage efface tous les réglages ou retombe sur l'ancienne copie");
}

void calibration_test() {
    const auto bytes = firmware(); const Firmware image{bytes};
    const auto calibration = *image.settings()->touch_calibration;
    check(calibration.valid() && calibration.sample_at(32,24) == TouchSample{0x200,0x300} &&
          calibration.sample_at(224,168) == TouchSample{0xe00,0xd00} &&
          calibration.sample_at(128,96) == TouchSample{0x800,0x800} &&
          calibration.sample_at(0,0) == TouchSample{0,0x155} &&
          calibration.sample_at(255,191) == TouchSample{0xff0,0xe99}, "origine, interpolation, extrapolation ou arrondi tactile");
    auto reversed = calibration; std::swap(reversed.first,reversed.second);
    check(reversed.valid() && reversed.sample_at(128,96) == TouchSample{0x800,0x800} &&
          reversed.sample_at(0,0) == calibration.sample_at(0,0), "ordre inverse des points d'étalonnage");
    const TouchCalibration saturated{{0,0,100,50},{4095,4095,110,60}};
    check(saturated.sample_at(0,0) == TouchSample{0,0} && saturated.sample_at(255,191) == TouchSample{4095,4095},
          "extrapolation n'est pas saturée sur 12 bits");
    for (unsigned defect = 0; defect < 8; ++defect) {
        auto invalid = calibration;
        switch (defect) {
        case 0: invalid.first.screen_x = 0; break;
        case 1: invalid.first.screen_y = 193; break;
        case 2: invalid.second.screen_y = 0; break;
        case 3: invalid.first.adc_x = 0x1000; break;
        case 4: invalid.second.adc_y = 0xffff; break;
        case 5: invalid.second.screen_x = invalid.first.screen_x; break;
        case 6: invalid.second.adc_x = invalid.first.adc_x; break;
        case 7: invalid.second.adc_y = invalid.first.adc_y; break;
        }
        check(!invalid.valid(), "étalonnage dégénéré accepté");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(invalid.sample_at(1,1)); }, "conversion d'un étalonnage invalide");
    }
    expect_failure<std::invalid_argument>([&] { static_cast<void>(calibration.sample_at(256,0)); }, "pixel X hors écran accepté");
    expect_failure<std::invalid_argument>([&] { static_cast<void>(calibration.sample_at(0,192)); }, "pixel Y hors écran accepté");

    Memory memory;
    expect_failure<ravenemu::Error>([&] { memory.set_touch_position(128,96); }, "coordonnées acceptées sans étalonnage");
    check(memory.input_state() == InputState{}, "conversion refusée modifie les entrées");
    memory.load_firmware(bytes);
    memory.set_input({0x801,false,true,true,{1,2,0x567,0x9ab}});
    memory.set_touch_position(128,96);
    check(memory.input_state() == InputState{0x801,true,true,true,{0x800,0x800,0x567,0x9ab}} &&
          memory.read16(arm7,0x04000136) == 0xb5 && memory.peek32(arm7,flags) == 0 && memory.peek32(arm9,flags) == 0,
          "API pixel efface boutons, capot, pression ou invente une IRQ");
    const auto before = memory.input_state();
    expect_failure<std::invalid_argument>([&] { memory.set_touch_position(65535,1); }, "coordonnées hôte invalides acceptées");
    check(memory.input_state() == before, "pixel invalide a changé le contact ou les ADC");
    memory.set_touch_position(65535,65535,false);
    check(!memory.input_state().pen_down && memory.input_state().touch == before.touch, "relâchement modifie les mesures mémorisées");
    auto damaged = bytes; damaged[0x3fe72] ^= 1; damaged[0x3ff72] ^= 1; memory.load_firmware(damaged);
    expect_failure<ravenemu::Error>([&] { memory.set_touch_position(10,10); }, "étalonnage inventé après deux CRC invalides");
    memory.set_input(before); memory.set_touch_position(0,0,false);
    check(!memory.input_state().pen_down, "relâchement impossible sans étalonnage");
}

std::uint8_t exchange(Memory& memory, std::uint8_t output) {
    memory.write8(arm7,data,output); memory.advance_cycles(64);
    check(!(memory.peek16(arm7,control) & 0x80U), "busy firmware persiste après 64 cycles");
    return memory.read8(arm7,data);
}
void select(Memory& memory, bool hold = true) {
    memory.write16(arm7,control,0); memory.write16(arm7,control,hold ? 0xc900 : 0xc100);
}
void address(Memory& memory, std::uint32_t offset, bool fast = false) {
    select(memory); static_cast<void>(exchange(memory,fast ? 0x0b : 3));
    for (const auto shift : {16U,8U,0U}) static_cast<void>(exchange(memory,static_cast<std::uint8_t>(offset >> shift)));
    if (fast) check(exchange(memory,0xa5) == 0xff, "FAST n'a pas de cycle muet");
}
std::uint8_t status(Memory& memory) {
    select(memory); static_cast<void>(exchange(memory,5)); return exchange(memory,0);
}

void serial_read_test() {
    Memory memory;
    expect_failure<ravenemu::Error>([&] { memory.write32(arm7,control,0x0003c900); }, "SPI accepte un firmware absent");
    check(memory.read32(arm7,control) == 0, "échec firmware absent a changé SPICNT");
    auto bytes = firmware(); bytes[0x12345] = 0x37; bytes[0x12346] = 0x82;
    memory.load_firmware(bytes);
    address(memory,0x12345);
    check(exchange(memory,0x9f) == 0x37 && exchange(memory,3) == 0x82, "READ adresse 24 bits ou flux indépendant des données sortantes");
    memory.write16(arm7,control,0xc100);
    check(exchange(memory,0) == bytes[0x12347], "dernier octet avec hold retiré");
    address(memory,0x12345,true);
    check(exchange(memory,0) == 0x37 && exchange(memory,0) == 0x82, "FAST lecture décale le résultat");
    for (const auto offset : {0x3ffffU,0xffffffU}) {
        address(memory,offset);
        check(exchange(memory,0) == 0xa6 && exchange(memory,0) == 0x5a, "bouclage mémoire ou compteur 24 bits");
    }
    for (const auto size : {0x40000U,0x80000U}) {
        bytes = firmware(size,size - 0x200); memory.load_firmware(bytes);
        select(memory); static_cast<void>(exchange(memory,0x9f));
        check(exchange(memory,0) == 0x20 && exchange(memory,0) == (size == 0x40000 ? 0x40 : 0x80) &&
              exchange(memory,0) == (size == 0x40000 ? 0x12 : 0x13) && exchange(memory,0) == 0xff, "JEDEC du modèle de flash");
        address(memory,size - 1);
        check(exchange(memory,0) == 0xa6 && exchange(memory,0) == 0x5a, "bouclage selon la capacité réelle");
    }
    check(memory.peek32(arm7,flags) == spi_irq && memory.peek32(arm9,flags) == 0, "IRQ firmware pas limitée à ARM7");
    memory.write32(arm7,flags,spi_irq); memory.advance_cycles(std::numeric_limits<std::uint64_t>::max());
    check(memory.peek32(arm7,flags) == 0, "IRQ firmware répétée après grande avance");
}

void status_power_and_errors_test() {
    Memory memory; const auto bytes = firmware(); memory.load_firmware(bytes);
    check(status(memory) == 0 && exchange(memory,0xff) == 0, "RDSR initial ou répétition");
    select(memory); static_cast<void>(exchange(memory,6));
    // CS est encore bas : changer de périphérique termine WREN.
    memory.write16(arm7,control,0x8a00);
    check(status(memory) == 2, "WREN perdu à la désélection ou WIP inventé");
    select(memory,false); static_cast<void>(exchange(memory,4));
    check(status(memory) == 0, "WRDI ne remet pas WEL à zéro");
    select(memory,false); static_cast<void>(exchange(memory,0xb9));
    select(memory); static_cast<void>(exchange(memory,0x9f));
    check(exchange(memory,0) == 0xff && exchange(memory,0) == 0xff, "flash répond pendant Deep Power-down");
    // Même une commande inconnue est ignorée pendant le sommeil.
    select(memory,false); check(exchange(memory,0x02) == 0xff, "flash endormie ne doit pas programmer");
    select(memory,false); static_cast<void>(exchange(memory,0xab));
    check(status(memory) == 0, "RDP ne réveille pas la flash");
    for (const auto command : {0x00U,0x01U,0x20U,0x60U,0xc7U,0xffU}) {
        memory.write16(arm7,control,0);
        const auto before = memory.read32(arm7,control); const auto requested = memory.peek32(arm7,flags);
        expect_failure<ravenemu::Error>([&] { memory.write32(arm7,control,(command << 16U) | 0xc900U); },
                                       "commande firmware non implémentée ignorée silencieusement");
        check(memory.read32(arm7,control) == before && memory.peek32(arm7,flags) == requested &&
              std::equal(bytes.begin(),bytes.end(),memory.firmware().bytes().begin()), "commande refusée a muté le bus ou l'image");
    }
    const std::array<std::uint32_t,2> words{0x0003c900,0};
    const auto before = memory.read32(arm7,control);
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm7,control,words); }, "transfert multiple dépasse SPI");
    check(memory.read32(arm7,control) == before, "validation multiple a lancé une lecture firmware");
}

void copy_reset_and_load_test() {
    Memory memory; auto bytes = firmware(); bytes[0x123] = 0x61; bytes[0x124] = 0x92;
    memory.load_firmware(bytes); bytes[0x123] = 0;
    address(memory,0x123); memory.write8(arm7,data,0); memory.advance_cycles(17);
    auto copy = memory;
    memory.advance_cycles(47);
    check(memory.read8(arm7,data) == 0x61 && (copy.read16(arm7,control) & 0x80U), "copie partage progression SPI ou image de l'hôte");
    copy.advance_cycles(47);
    check(copy.read8(arm7,data) == 0x61 && exchange(memory,0) == 0x92 && exchange(copy,0) == 0x92,
          "session firmware copiée ne conserve pas son curseur");
    memory.write8(arm7,data,0); memory.advance_cycles(19);
    const auto before = memory.read32(arm7,control);
    expect_failure<ravenemu::RomLoadError>([&] { memory.load_firmware(std::span<const std::uint8_t>{}); }, "firmware vide accepté");
    check(memory.read32(arm7,control) == before && memory.firmware().bytes()[0x123] == 0x61, "chargement firmware refusé annule le transfert");
    memory.advance_cycles(45);
    check(memory.read8(arm7,data) == bytes[0x125], "chargement refusé change le compteur différé");
    select(memory,false); static_cast<void>(exchange(memory,6));
    select(memory,false); static_cast<void>(exchange(memory,0xb9));
    memory.clear_ram();
    check(memory.firmware().bytes().size() == 0x40000 && memory.read32(arm7,control) == 0 &&
          memory.peek32(arm7,flags) == 0 && status(memory) == 0, "reset efface l'image ou conserve sommeil/WEL");
    memory.write32(arm7,flags,spi_irq); memory.advance_cycles(1000);
    check(memory.peek32(arm7,flags) == 0, "reset laisse une IRQ firmware différée");
    bytes[0x123] = 0xee; copy.load_firmware(bytes);
    address(copy,0x123); check(exchange(copy,0) == 0xee, "nouvelle image firmware non appliquée");
    address(memory,0x123); check(exchange(memory,0) == 0x61, "remplacement firmware de la copie modifie l'original");
    memory.set_input({1,true,true,false,{0x800,0x600,0x444,0xaaa}});
    memory.write16(arm7,control,0x8a00); static_cast<void>(exchange(memory,0xd3));
    check(memory.read16(arm7,0x04000136) & 0x40U, "commande TSC n'a pas masqué PENIRQ");
    address(memory,0x123); memory.write32(arm7,flags,spi_irq); memory.write8(arm7,data,0); memory.advance_cycles(20);
    const auto physical = memory.input_state();
    memory.load_firmware(bytes); memory.advance_cycles(1000);
    check(memory.read32(arm7,control) == 0 && memory.peek32(arm7,flags) == 0 && memory.input_state() == physical &&
          !(memory.read16(arm7,0x04000136) & 0x40U), "remplacement firmware ne réinitialise pas le bus ou efface les entrées physiques");
}

void executable_and_boot_test() {
    Program arm9_program;
    arm9_program.load(0,firmware_settings_address + 0x58);
    arm9_program.code.push_back(0xe5901000); // première paire ADC depuis les réglages de boot
    arm9_program.store(1,0x02001000);
    Program arm7_program;
    arm7_program.load(0,control); arm7_program.load(1,0xc900);
    arm7_program.code.push_back(0xe1c010b0); // firmware à 4 MHz nominal
    for (const auto output : {3,3,0xfe,0x58}) arm7_program.spi_byte(static_cast<std::uint8_t>(output));
    arm7_program.spi_byte(0,2); arm7_program.spi_byte(0,3);
    arm7_program.code.insert(arm7_program.code.end(),{0xe1a03403,0xe1822003});
    arm7_program.store(2,0x02001004); // X1 lu dans la flash
    arm7_program.load(1,0xca01); arm7_program.code.push_back(0xe1c010b0); // tactile à 2 MHz
    arm7_program.spi_byte(0xd0); arm7_program.spi_byte(0,2); arm7_program.spi_byte(0,3);
    arm7_program.code.insert(arm7_program.code.end(),{0xe1a02402,0xe1822003,0xe1a021a2});
    arm7_program.store(2,0x02001008); // X mesuré après conversion des pixels hôte
    const auto rom = cartridge_image(arm9_program.finish(),arm7_program.finish());
    const auto bytes = firmware();
    Machine machine; machine.load_firmware(bytes); machine.load_cartridge(Cartridge{rom});
    check(machine.memory().peek8(arm9,firmware_settings_address + 2) == 3 &&
          machine.memory().peek16(arm7,firmware_settings_address + 0x70) == 0,
          "boot ne copie pas exactement les 0x70 octets de la copie récente");
    machine.set_touch_position(128,96); machine.run_cycles(5000);
    check(machine.memory().peek32(arm9,0x02001000) == 0x03000200 &&
          machine.memory().peek32(arm9,0x02001004) == 0x200 && machine.memory().peek32(arm9,0x02001008) == 0x800 &&
          machine.memory().peek32(arm7,flags) == spi_irq && machine.memory().peek32(arm9,flags) == 0,
          "ARM9/ARM7 n'observent pas les réglages de boot, la flash SPI et le tactile étalonné");
    address(machine.memory(),0x3fe02); machine.memory().write8(arm7,data,0); machine.advance_cycles(19);
    auto copy = machine; auto invalid = rom; put32(invalid,0x28,0x04000000);
    const auto clock = copy.scheduler_state(); const auto physical = copy.input_state();
    expect_failure<ravenemu::RomLoadError>([&] { copy.load_cartridge(Cartridge{invalid}); }, "destination MMIO acceptée au boot");
    check(copy.scheduler_state() == clock && copy.input_state() == physical &&
          (copy.memory().peek16(arm7,control) & 0x80U) && copy.memory().firmware().settings()->offset == 0x3fe00,
          "chargement refusé efface firmware, entrées ou transfert en attente");
    copy.advance_cycles(45);
    check(copy.memory().read8(arm7,data) == 3 && (machine.memory().read16(arm7,control) & 0x80U),
          "copie de machine partage la session flash");
    machine.load_cartridge(Cartridge{rom});
    check(machine.memory().firmware().bytes().size() == bytes.size() && machine.memory().peek32(arm7,control) == 0 &&
          machine.memory().peek32(arm7,flags) == 0 && machine.input_state() == InputState{} &&
          machine.memory().peek8(arm9,firmware_settings_address + 2) == 3, "rechargement ne conserve pas le firmware ou ne réinitialise pas le bus");
    machine.run_cycles(5000);
    check(machine.memory().peek32(arm9,0x02001008) == 0, "stylet reste appuyé après rechargement");

    auto newer = bytes; put16(newer,0x3ff70,11);
    machine.load_firmware(newer); machine.load_cartridge(Cartridge{rom});
    check(machine.memory().peek8(arm7,firmware_settings_address + 2) == 7, "boot ignore la seconde copie plus récente");
    newer[0x3fe72] ^= 1; newer[0x3ff72] ^= 1;
    machine.load_firmware(newer); machine.load_cartridge(Cartridge{rom});
    check(machine.memory().peek32(arm7,firmware_settings_address) == 0, "boot invente des réglages après CRC invalides");

    // Le chargement des réglages ne doit pas écraser un exécutable valide.
    auto overlap = cartridge_image(); put32(overlap,0x24,0x023ffc80); put32(overlap,0x28,0x023ffc80);
    machine.load_firmware(bytes); machine.load_cartridge(Cartridge{overlap});
    check(machine.memory().peek32(arm9,firmware_settings_address) == 0xeafffffe, "copie de réglages écrase le programme chargé");
    machine.memory().clear_ram();
    check(machine.memory().firmware().settings() && machine.memory().peek32(arm7,firmware_settings_address) == 0,
          "clear_ram perd l'image ou simule implicitement un boot");
}
} // namespace

int main() {
    image_and_settings_test();
    calibration_test();
    serial_read_test();
    status_power_and_errors_test();
    copy_reset_and_load_test();
    executable_and_boot_test();
}
