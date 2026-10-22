#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/program_fixture.hpp"

#include <array>
#include <limits>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
constexpr auto arm7 = Processor::arm7, arm9 = Processor::arm9;
constexpr std::uint32_t control = 0x040001c0, data = control + 2, flags = 0x04000214;
constexpr std::uint32_t spi_irq = 1U << 23U;

std::uint8_t byte(Memory& memory, std::uint8_t output, bool last = false, unsigned baud = 2) {
    memory.write16(arm7,control,static_cast<std::uint16_t>((last ? 0x8000U : 0x8800U) | baud));
    memory.write8(arm7,data,output);
    memory.advance_cycles(64U << baud);
    check(!(memory.read16(arm7,control) & 0x80U), "octet Powerman toujours occupé");
    return memory.read8(arm7,data);
}

std::uint8_t read_register(Memory& memory, std::uint8_t index) {
    check(byte(memory,static_cast<std::uint8_t>(index | 0x80U)) == 0, "réponse avant l'octet de lecture");
    return byte(memory,0,true);
}

void write_register(Memory& memory, std::uint8_t index, std::uint8_t value) {
    static_cast<void>(byte(memory,index));
    check(byte(memory,value,true) == 0, "écriture Powerman produit une donnée");
}

void registers_and_mirrors() {
    Memory memory;
    check(read_register(memory,0) == 0x0d && read_register(memory,1) == 0 &&
          read_register(memory,2) == 0 && read_register(memory,3) == 1, "état initial de démarrage direct");
    // Tous les indices de la DS originale, y compris ceux qui identifient la Lite.
    for (unsigned index = 0; index < 128; ++index) {
        switch (index & 3U) {
        case 0: write_register(memory,static_cast<std::uint8_t>(index),0xbf); break;
        case 1: write_register(memory,static_cast<std::uint8_t>(index),0xff); break;
        case 2: write_register(memory,static_cast<std::uint8_t>(index),0xfe); break;
        case 3: write_register(memory,static_cast<std::uint8_t>(index),0xfe); break;
        }
        const std::array<std::uint8_t,4> expected{0x3f,0,0,2};
        check(read_register(memory,static_cast<std::uint8_t>(index)) == expected[index & 3U],
              "miroir, masque ou registre batterie modifiable");
    }
    check(memory.power_management().control() == 0x3f && memory.power_management().microphone_gain() == 2 &&
          memory.power_management().upper_backlight_enabled() && memory.power_management().lower_backlight_enabled(),
          "état Powerman hôte incohérent");
    write_register(memory,2,0xff);
    write_register(memory,0,0x81);
    check(memory.power_management().microphone_control() == 1 && read_register(memory,0) == 1 &&
          !memory.power_management().upper_backlight_enabled() && !memory.power_management().lower_backlight_enabled(),
          "amplificateurs ou rétroéclairages non modifiables");
    memory.set_battery_low(true);
    check(read_register(memory,1) == 1 && read_register(memory,0x7d) == 1, "entrée batterie basse ignorée");
    write_register(memory,1,0); check(read_register(memory,1) == 1, "statut batterie effacé par le logiciel");
    memory.set_battery_low(false); check(read_register(memory,1) == 0, "retour batterie bonne ignoré");
    // Powerman ne modifie ni POWCNT1, ni les pixels produits par les moteurs 2D.
    memory.write16(arm9,0x04000304,0x8203);
    memory.write32(arm9,0x04000000,0x10000);
    memory.write16(arm9,0x05000000,0x1234);
    const auto frame = memory.render_frame();
    write_register(memory,0,0x0c);
    check(memory.read16(arm9,0x04000304) == 0x8203 && memory.render_frame().upper == frame.upper &&
          memory.render_frame().lower == frame.lower, "rétroéclairage altère le rendu ou POWCNT1");
}

void timing_selection_and_irq() {
    Memory memory;
    for (unsigned baud = 0; baud < 4; ++baud) {
        memory.clear_ram();
        const auto duration = 64U << baud;
        // Mot combiné : sélectionner Powerman et transmettre son index.
        memory.write32(arm7,control,0x0000c800U | baud);
        memory.advance_cycles(duration);
        check(memory.read32(arm7,flags) == spi_irq && memory.read32(arm9,flags) == 0, "IRQ index Powerman");
        memory.write32(arm7,flags,spi_irq);
        memory.write8(arm7,data + 1,0x40); // voie haute : pas de strobe
        check(!(memory.read16(arm7,control) & 0x80U), "voie haute déclenche Powerman");
        memory.write16(arm7,data,0xff38);
        memory.write8(arm7,data,0x40); // busy : doit être ignoré
        memory.advance_cycles(duration - 1);
        check(memory.power_management().control() == 0x0d && memory.read32(arm7,flags) == 0,
              "registre ou IRQ appliqué avant la fin de l'octet");
        memory.set_battery_low(true); // une copie différée ne doit pas écraser l'entrée hôte
        memory.advance_cycles(1);
        check(memory.power_management().control() == 0x38 && memory.power_management().battery_low() &&
              memory.read32(arm7,flags) == spi_irq && !memory.powered_off(), "fin Powerman, busy ou entrée hôte");
        memory.write16(arm7,control,0); // CS seul ne réinitialise pas les registres
        memory.write32(arm7,flags,spi_irq);
        memory.advance_cycles(10000);
        check(read_register(memory,0) == 0x38 && memory.read32(arm7,flags) == 0, "désélection ou IRQ répétée");
    }
    memory.clear_ram();
    static_cast<void>(byte(memory,0,true)); // CS non maintenu : la donnée suivante est un nouvel index
    static_cast<void>(byte(memory,0x40,true));
    check(!memory.powered_off() && read_register(memory,0) == 0x0d, "index survit à CS haut");
    static_cast<void>(byte(memory,0));
    memory.write16(arm7,control,0x8a02); // détour par le tactile sans strobe
    memory.write16(arm7,control,0x8802);
    static_cast<void>(byte(memory,0x40,true));
    check(!memory.powered_off(), "sélection d'un autre périphérique conserve l'index");
    static_cast<void>(byte(memory,0));
    memory.write16(arm7,control,0xc802); memory.write8(arm7,data,0x40);
    memory.advance_cycles(255); memory.write16(arm7,control,0); memory.advance_cycles(10000);
    check(!memory.powered_off() && memory.power_management().control() == 0x0d && memory.read32(arm7,flags) == 0,
          "extinction ou IRQ malgré l'abandon d'un octet incomplet");
    // La validation d'un STM doit précéder toute sélection/strobe.
    const std::array<std::uint32_t,2> invalid{0x00008802,0};
    const auto before = memory.read32(arm7,control);
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm7,control,invalid); }, "STM vers trou SPI accepté");
    check(memory.read32(arm7,control) == before, "STM refusé a démarré un index");
}

void copies_reset_and_shutdown_boundary() {
    Memory memory; memory.set_battery_low(true);
    static_cast<void>(byte(memory,0));
    memory.write8(arm7,data,0x40); memory.advance_cycles(17);
    auto copy = memory;
    // Le timer ne doit avancer que jusqu'au dernier bit de l'extinction.
    copy.write32(arm7,0x04000100,0x00800000);
    copy.advance_cycles(std::numeric_limits<std::uint64_t>::max());
    check(copy.powered_off() && !memory.powered_off() && copy.read16(arm7,0x04000100) == 239 &&
          !copy.power_management().upper_backlight_enabled() && !copy.power_management().lower_backlight_enabled(),
          "copie partagée ou grande avance dépasse l'extinction");
    const auto line = copy.read16(arm7,0x04000006);
    copy.advance_cycles(100000);
    check(copy.read16(arm7,0x04000100) == 239 && copy.read16(arm7,0x04000006) == line,
          "horloges avancent après extinction");
    // Un CPU séparé, son reset et une IRQ ne rétablissent pas l'alimentation.
    copy.write32(arm9,0x02000000,0xe3a0002a);
    Cpu cpu{arm9}; cpu.reset(0x02000000,false); cpu.set_interrupt_lines(true,true);
    const auto cpu_before = cpu.state(); cpu.step(copy);
    check(cpu.state() == cpu_before, "CPU ou interruption exécuté après extinction");
    copy.write32(arm9,0x040000b0,0x02000000); copy.write32(arm9,0x040000b4,0x02001000);
    copy.write32(arm9,0x040000b8,0x84000001);
    check(!copy.step_dma(arm9) && copy.read32(arm9,0x02001000) == 0, "DMA exécuté hors tension");
    copy.write16(arm7,control,0); copy.write32(arm7,control,0x00008002);
    check(copy.powered_off(), "écriture SPI remet sous tension");
    copy.clear_ram();
    check(!copy.powered_off() && copy.power_management().battery_low() && read_register(copy,0) == 0x0d,
          "reset conserve extinction ou perd l'entrée batterie");
    memory.clear_ram(); memory.advance_cycles(1000);
    check(!memory.powered_off() && memory.read32(arm7,control) == 0, "reset laisse une extinction différée");
}

std::vector<std::uint8_t> program() {
    Program p; p.load(0,control);
    const auto select = [&](bool last) { p.load(2,last ? 0xc002 : 0xc802); p.code.push_back(0xe1c020b0); };
    select(false); p.spi_byte(0); select(true); p.spi_byte(0x39);
    select(false); p.spi_byte(0x80); select(true); p.spi_byte(0,3); p.store(3,0x02001000);
    select(false); p.spi_byte(0x81); select(true); p.spi_byte(0,3); p.store(3,0x02001004);
    select(false); p.spi_byte(0); select(true); p.spi_byte(0x40);
    p.load(3,0xdeadbeef); p.store(3,0x02001008); // ne doit jamais s'exécuter
    return cartridge_image({0xe2800001,0xeafffffd},p.finish());
}

void executable_reload_and_persistence() {
    const auto rom = program();
    Machine machine; machine.set_battery_low(true); machine.load_cartridge(Cartridge{rom});
    machine.load_backup(BackupType::fram_8k);
    const auto save_byte = [&](std::uint8_t output, bool last) {
        machine.memory().write16(arm9,0x040001a0,last ? 0xa000 : 0xa040);
        machine.memory().write8(arm9,0x040001a2,output); machine.advance_cycles(64);
    };
    save_byte(6,true); save_byte(2,false); save_byte(0,false); save_byte(0x12,false); save_byte(0xa5,true);
    const auto saved = machine.snapshot_backup();
    machine.memory().write32(arm7,0x04000100,0x00800000);
    machine.run_cycles(10000);
    check(machine.powered_off() && machine.scheduler_state().cycles < 10000 && machine.scheduler_state().cycles > 2000 &&
          machine.memory().read32(arm9,0x02001000) == 0x39 && machine.memory().read32(arm9,0x02001004) == 1 &&
          machine.memory().read32(arm9,0x02001008) == 0 && machine.cpu_state(arm9).registers[0] != 0,
          "programme ARM7 ne dialogue pas avec Powerman ou continue après extinction");
    check(machine.memory().read16(arm7,0x04000100) == machine.scheduler_state().cycles,
          "timer diverge de l'ordonnanceur lors de l'extinction");
    const auto a9 = machine.cpu_state(arm9), a7 = machine.cpu_state(arm7);
    const auto scheduler = machine.scheduler_state();
    machine.set_interrupt_lines(arm7,true,true); machine.set_key(Key::a,true);
    machine.run_cycles(std::numeric_limits<std::uint64_t>::max()); machine.run_frame();
    machine.run_steps(arm7,100); machine.step(arm9); machine.advance_cycles(10000);
    check(machine.cpu_state(arm9) == a9 && machine.cpu_state(arm7) == a7 && machine.scheduler_state() == scheduler,
          "machine éteinte continue ou se réveille comme HALT");
    check(machine.backup_dirty() && machine.snapshot_backup().data == saved.data && saved.data[0x12] == 0xa5,
          "extinction perd la sauvegarde ou son état dirty");
    machine.acknowledge_backup_saved(saved.generation); check(!machine.backup_dirty(), "sauvegarde après extinction");
    auto copy = machine;
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { copy.load_cartridge(Cartridge{invalid}); }, "ROM invalide acceptée");
    check(copy.powered_off() && copy.scheduler_state() == scheduler, "échec de chargement rallume la console");
    // Charger le firmware ne doit pas agir comme un bouton d'alimentation.
    copy.load_firmware(std::vector<std::uint8_t>(0x40000,0xff));
    check(copy.powered_off(), "chargement du firmware rallume Powerman");
    copy.load_cartridge(Cartridge{rom});
    check(!copy.powered_off() && copy.power_management().battery_low() && copy.scheduler_state().cycles == 0 &&
          machine.powered_off(), "redémarrage direct ou indépendance des copies");
    copy.run_cycles(10000); check(copy.powered_off(), "programme non reproductible après redémarrage");
}
} // namespace

int main() {
    registers_and_mirrors();
    timing_selection_and_irq();
    copies_reset_and_shutdown_boundary();
    executable_reload_and_persistence();
}
