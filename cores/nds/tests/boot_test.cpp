#include <ravenemu/nds/machine.hpp>
#include <ravenemu/core.hpp>
#include "check.hpp"

#include <array>
#include <vector>

using ravenemu::nds::Cartridge;
using ravenemu::nds::Machine;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;

namespace {

void put32(std::vector<std::uint8_t>& rom, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) rom[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

std::vector<std::uint8_t> image(std::uint32_t arm9_address = 0x02000000,
                                std::uint32_t arm7_address = 0x03800000) {
    std::vector<std::uint8_t> rom(0x208);
    put32(rom, 0x20, 0x200);
    put32(rom, 0x24, arm9_address);
    put32(rom, 0x28, arm9_address);
    put32(rom, 0x2c, 4);
    put32(rom, 0x30, 0x204);
    put32(rom, 0x34, arm7_address | 1U);
    put32(rom, 0x38, arm7_address);
    put32(rom, 0x3c, 4);
    put32(rom, 0x200, 0xeafffffe); // ARM b .
    put32(rom, 0x204, 0x46c0e7fe); // Thumb b . / nop
    return rom;
}

void load_test() {
    Machine machine;
    check(!machine.entries(), "machine initiale déjà chargée");
    machine.load_cartridge(Cartridge{image()});
    const auto& ram = machine.memory();
    check(ram.peek32(arm9, 0x02000000) == 0xeafffffe, "copie ARM9");
    check(ram.peek32(arm7, 0x02400000) == 0xeafffffe, "programme ARM9 visible par ARM7");
    check(ram.peek16(arm7, 0x03800000) == 0xe7fe, "copie ARM7");
    check(machine.entries()->arm9.address == 0x02000000 && !machine.entries()->arm9.thumb, "entrée ARM9");
    check(machine.entries()->arm7.address == 0x03800000 && machine.entries()->arm7.thumb, "entrée ARM7");
    check(ram.wram_control() == 3, "répartition WRAM au chargement");
    machine.memory().write8(arm9, 0x02000100, 0x55);
    machine.memory().write8(arm7, 0x03800100, 0x55);
    machine.load_cartridge(Cartridge{image(0x023ffffc, 0x03007ffc)});
    check(ram.peek32(arm9, 0x023ffffc) == 0xeafffffe, "dernier mot RAM principale");
    check(ram.peek16(arm7, 0x03007ffc) == 0xe7fe, "chargement en WRAM partagée");
    check(ram.peek8(arm9, 0x02000100) == 0 && ram.peek8(arm7, 0x03800100) == 0, "RAM remise à zéro");
    machine.load_cartridge(Cartridge{image(0x02000000, 0x02000004)});
    check(ram.peek32(arm9, 0x02000004) == 0x46c0e7fe, "ARM7 en RAM principale, plages adjacentes");
    machine.load_cartridge(Cartridge{image(0x02000000, 0x0380fffc)});
    check(ram.peek32(arm7, 0x0380fffc) == 0x46c0e7fe, "dernier mot RAM privée ARM7");
    auto rom = image();
    put32(rom, 0x24, 0x02000003); // entrée Thumb à +2
    put32(rom, 0x34, 0x03800000); // entrée ARM
    machine.load_cartridge(Cartridge{rom});
    check(machine.entries()->arm9.address == 0x02000002 && machine.entries()->arm9.thumb,
          "entrée Thumb ARM9 à l'intérieur du programme");
    check(machine.entries()->arm7.address == 0x03800000 && !machine.entries()->arm7.thumb,
          "entrée ARM ARM7");
}

void rejected_load_test() {
    Machine machine;
    machine.load_cartridge(Cartridge{image()});
    machine.memory().write32(arm9, 0x02001000, 0x12345678);
    machine.memory().set_wram_control(1);
    const std::array<std::array<std::uint32_t, 2>, 10> invalid{{
        {0x02400000, 0x03800000}, // alias exclu du chargeur initial
        {0x03000000, 0x03800000}, // WRAM non allouée à ARM9
        {0x04000000, 0x03800000}, // MMIO
        {0x023ffffe, 0x03800000}, // franchissement RAM
        {0x02000000, 0x0380fffe}, // franchissement WRAM privée
        {0x02000000, 0x03007ffe}, // franchissement WRAM partagée
        {0x02000000, 0x02000000}, // chevauchement
        {0x02000000, 0x02000002}, // chevauchement partiel
        {0x02000002, 0x03800000}, // entrée ARM non alignée
        {0x02000000, 0x04000000}, // ARM7 invalide après un ARM9 valide
    }};
    for (const auto addresses : invalid) {
        expect_failure<ravenemu::RomLoadError>([&] {
            machine.load_cartridge(Cartridge{image(addresses[0], addresses[1])});
        }, "destination invalide acceptée");
        check(machine.memory().read32(arm9, 0x02001000) == 0x12345678, "échec a effacé la mémoire");
        check(machine.memory().wram_control() == 1, "échec a modifié WRAMCNT");
        check(machine.entries()->arm9.address == 0x02000000, "échec a modifié l'entrée ARM9");
        check(machine.entries()->arm7.address == 0x03800000, "échec a modifié l'entrée ARM7");
    }
    auto rom = image();
    put32(rom, 0x34, 0x03800003); // dernière demi-instruction Thumb
    put32(rom, 0x3c, 3);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(Cartridge{rom}); },
                                         "instruction Thumb tronquée acceptée");
    Machine empty;
    expect_failure<ravenemu::RomLoadError>([&] {
        empty.load_cartridge(Cartridge{image(0x04000000)});
    }, "chargement initial invalide accepté");
    check(!empty.entries() && empty.memory().read32(arm9, 0x02000000) == 0, "échec initial non atomique");
}

} // namespace

int main() {
    load_test();
    rejected_load_test();
}
