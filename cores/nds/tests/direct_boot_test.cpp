#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/firmware_fixture.hpp"

#include <algorithm>
#include <array>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t header = 0x027ffe00, boot = 0x027ff800;

std::vector<std::uint8_t> image() {
    auto rom = cartridge_image();
    put32(rom,0,0x544f4f42); put32(rom,0xc,0x54535442);
    put16(rom,0x6c,0x55aa); put16(rom,0x15e,0xa55a); rom[0x16f] = 0x7e;
    return rom;
}

void ram_parameters() {
    for (const auto capacity : {0U,7U,12U}) {
        auto rom = image(); rom[0x14] = static_cast<std::uint8_t>(capacity);
        Machine machine; machine.load_cartridge(Cartridge{rom});
        auto& memory = machine.memory();
        for (unsigned i = 0; i < 0x170; ++i)
            check(memory.read8(arm9,header + i) == rom[i] && memory.read8(arm7,0x023ffe00 + i) == rom[i],
                  "en-tête NDS incomplet ou absent du miroir RAM partagé");
        const auto chip_id = capacity == 0 ? 0x7c2U : (capacity == 7 ? 0xfc2U : 0x8000fec2U);
        for (const auto base : {boot,0x027ffc00U}) {
            check(memory.read32(arm9,base) == chip_id && memory.read32(arm7,base + 4) == chip_id,
                  "identifiant cartouche absent des deux blocs de boot");
            check(memory.read16(arm9,base + 8) == 0xa55a && memory.read16(arm7,base + 0xa) == 0x55aa,
                  "CRC déclarés par l'en-tête non copiés");
        }
        check(memory.read16(arm9,boot + 0x10) == 0xffff && memory.read32(arm9,boot + 0x60) == 0x03800000 &&
              memory.read32(arm7,boot + 0x80) == 7 && memory.read32(arm9,boot + 0x84) == 6 &&
              memory.read16(arm7,0x027ffc40) == 1, "paramètres de fin de boot cartouche");
        check(memory.read32(arm9,boot + 0x64) == 1 && memory.read32(arm9,boot + 0x68) == 0 &&
              memory.read32(arm9,firmware_settings_address) == 0, "firmware absent présenté comme réglages valides");
        // Le même identifiant doit être observable par le port B8.
        memory.write32(arm9,0x040001a8,0xb8); memory.write32(arm9,0x040001ac,0);
        memory.write32(arm9,0x040001a4,0xa7000000); machine.advance_cycles(60);
        check(memory.read32(arm9,0x04100010) == chip_id && memory.gamecard_chip_id() == chip_id,
              "paramètres de boot et cartouche divergent");
    }
}

void firmware_metadata() {
    Machine machine; auto bytes = firmware(); put16(bytes,0x26,0x1234);
    settings(bytes,0x3ff00,11,9);
    machine.load_firmware(bytes); machine.load_cartridge(Cartridge{image()});
    auto& memory = machine.memory();
    check(memory.read32(arm7,boot + 0x64) == 0 && memory.read32(arm9,boot + 0x68) == 0x3fe00 &&
          memory.read16(arm9,boot + 0x74) == 0x1234 && memory.read8(arm7,firmware_settings_address + 2) == 9,
          "base firmware, CRC ou sélection de la seconde copie des réglages");
    for (unsigned i = 0; i < 0x70; ++i)
        check(memory.read8(arm9,firmware_settings_address + i) == bytes[0x3ff00 + i], "réglages post-firmware tronqués");
    bytes[0x3fe72] ^= 1; bytes[0x3ff72] ^= 1;
    machine.load_firmware(bytes); machine.load_cartridge(Cartridge{image()});
    check(memory.read32(arm9,boot + 0x64) == 1 && memory.read32(arm9,boot + 0x68) == 0x3fe00 &&
          memory.read32(arm9,firmware_settings_address) == 0, "réglages avec CRC invalides déclarés valides au boot");
}

void cpu_context() {
    for (const auto cpu : {arm9,arm7}) for (const bool thumb : {false,true}) {
        Cpu core{cpu}; constexpr std::uint32_t address = 0x02000000;
        core.reset_direct_boot(address,thumb);
        const auto& state = core.state();
        check(state.registers[15] == address && state.thumb == thumb && state.mode == CpuMode::system &&
              state.irq_disabled && state.fiq_disabled && state.instructions == 0 && !state.waiting_for_interrupt,
              "mode initial du démarrage direct CPU");
        check(state.registers[12] == (address | (thumb ? 1U : 0U)) && state.registers[14] == state.registers[12] &&
              state.registers[13] == (cpu == arm9 ? 0x03002f7cU : 0x0380fd80U), "r12/LR/SP post-firmware");
        for (unsigned r = 0; r < 12; ++r) check(state.registers[r] == 0, "r0..r11 post-firmware non nuls");
        auto banks = state;
        banks.set_cpsr(0xd2);
        check(banks.registers[13] == (cpu == arm9 ? 0x03003f80U : 0x0380ff80U) && banks.registers[14] == 0 && banks.spsr() == 0,
              "banque de pile IRQ de boot");
        banks.set_cpsr(0xd3);
        check(banks.registers[13] == (cpu == arm9 ? 0x03003fc0U : 0x0380ffc0U) && banks.registers[14] == 0 && banks.spsr() == 0,
              "banque de pile Supervisor de boot");
        banks.set_cpsr(state.cpsr());
        check(banks.registers == state.registers && banks.cpsr() == state.cpsr(), "initialisation des banques détruit System");
        const auto before = state;
        expect_failure<std::invalid_argument>([&] { core.reset_direct_boot(address + 1,thumb); }, "entrée directe non alignée acceptée");
        check(core.state() == before, "reset direct refusé modifie l'état CPU");
        core.reset(address,thumb);
        check(core.state().registers[13] == 0 && core.state().registers[12] == 0 && core.state().registers[14] == 0,
              "reset minimal de l'interpréteur simule un boot firmware");
    }
    Machine machine; machine.load_cartridge(Cartridge{image()});
    auto& memory = machine.memory();
    check(memory.cp15().read(9,1,0) == 0x0300000a && memory.cp15().read(1,0,0) == 0x12078 &&
          memory.cp15().read(9,1,1) == 0 && memory.wram_control() == 3, "DTCM de boot ou WRAM partagée");
    memory.write32(arm9,0x03002f78,0x11223344);
    memory.write32(arm7,0x03002f78,0xaabbccdd);
    check(memory.read32(arm9,0x03002f78) == 0x11223344 && memory.read32(arm7,0x03002f78) == 0xaabbccdd,
          "pile DTCM ARM9 partage le stockage WRAM ARM7");
}

void stack_and_exception_programs() {
    for (const auto cpu : {arm9,arm7}) {
        // PUSH/POP System sans que le programme initialise lui-même son SP.
        const std::vector<std::uint32_t> code{0xe3a0005a,0xe92d4001,0xe3a00000,0xe8bd4001,0xeafffffe};
        Machine machine;
        machine.load_cartridge(Cartridge{cpu == arm9 ? cartridge_image(code) : cartridge_image({0xeafffffe},code)});
        const auto initial = machine.cpu_state(cpu);
        machine.run_steps(cpu,4);
        check(machine.cpu_state(cpu).registers[0] == 0x5a && machine.cpu_state(cpu).registers[13] == initial.registers[13] &&
              machine.cpu_state(cpu).registers[14] == initial.registers[14], "pile System de boot inexploitable");

        std::vector<std::uint8_t> bios(cpu == arm9 ? 0x1000 : 0x4000);
        put32(bios,8,0xea00003c); put32(bios,0x100,0xe92d4001); put32(bios,0x104,0xe8fd8001);
        machine.memory().load_bios(cpu,bios);
        const std::vector<std::uint32_t> swi{0xef000000,0xe3a03007,0xeafffffe};
        machine.load_cartridge(Cartridge{cpu == arm9 ? cartridge_image(swi) : cartridge_image({0xeafffffe},swi)});
        machine.run_steps(cpu,3); // SWI, branche BIOS, PUSH de la banque Supervisor
        check(machine.cpu_state(cpu).mode == CpuMode::supervisor &&
              machine.cpu_state(cpu).registers[13] == (cpu == arm9 ? 0x03003fb8U : 0x0380ffb8U), "pile SVC au premier SWI");
        machine.run_steps(cpu,2);
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[3] == 7,
              "retour de SWI via pile SVC et SPSR");

        put32(bios,0x18,0xea000038); put32(bios,0x100,0xe24ee004); // IRQ : corriger LR avant PUSH
        put32(bios,0x104,0xe92d4001); put32(bios,0x108,0xe8fd8001);
        machine.memory().load_bios(cpu,bios);
        const std::vector<std::uint32_t> irq{0xe321f05f,0xe3a03009,0xeafffffe}; // autoriser IRQ
        machine.load_cartridge(Cartridge{cpu == arm9 ? cartridge_image(irq) : cartridge_image({0xeafffffe},irq)});
        machine.step(cpu); machine.set_interrupt_lines(cpu,true); machine.step(cpu); machine.set_interrupt_lines(cpu,false);
        machine.run_steps(cpu,3); // branche BIOS, SUB LR, PUSH
        check(machine.cpu_state(cpu).mode == CpuMode::irq &&
              machine.cpu_state(cpu).registers[13] == (cpu == arm9 ? 0x03003f78U : 0x0380ff78U), "pile IRQ au premier événement");
        machine.run_steps(cpu,2);
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[3] == 9,
              "retour IRQ via pile de boot");
    }
}

void thumb_and_header_programs() {
    for (const auto cpu : {arm9,arm7}) {
        const std::vector<std::uint32_t> thumb{0xb501205a,0xbc032000,0x46c0e7fe}; // MOV, PUSH {r0,lr}, MOV, POP {r0,r1}
        auto rom = cpu == arm9 ? cartridge_image(thumb) : cartridge_image({0xeafffffe},thumb);
        put32(rom,cpu == arm9 ? 0x24 : 0x34,cpu == arm9 ? 0x02000001 : 0x03800001);
        Machine machine; machine.load_cartridge(Cartridge{rom});
        const auto initial = machine.cpu_state(cpu);
        machine.run_steps(cpu,4);
        check(machine.cpu_state(cpu).thumb && machine.cpu_state(cpu).registers[0] == 0x5a &&
              machine.cpu_state(cpu).registers[1] == initial.registers[14] &&
              machine.cpu_state(cpu).registers[13] == initial.registers[13], "pile Thumb initiale ou bit d'interworking de LR");
    }
    const auto program = [](std::uint32_t destination) {
        Program code;
        code.load(0,header + 0xc); code.code.push_back(0xe5901000); code.store(1,destination);
        code.load(0,0x027ffc40); code.code.push_back(0xe1d020b0); code.store(2,destination + 4);
        code.load(0,0x027ffc00); code.code.push_back(0xe5903000); code.store(3,destination + 8);
        return code.finish();
    };
    auto rom = cartridge_image(program(0x02001000),program(0x02001010)); put32(rom,0xc,0x54535442);
    Machine machine; machine.load_cartridge(Cartridge{rom}); machine.run_cycles(100);
    for (const auto destination : {0x02001000U,0x02001010U})
        check(machine.memory().read32(arm9,destination) == 0x54535442 &&
              machine.memory().read32(arm9,destination + 4) == 1 && machine.memory().read32(arm9,destination + 8) == 0x7c2,
              "programme ARM9/ARM7 ne retrouve pas le contexte de boot en RAM");
}

void overlap_reload_and_failure() {
    Machine machine; const auto rom = image(); machine.load_cartridge(Cartridge{rom});
    machine.memory().write32(arm9,header,0); machine.memory().write32(arm9,0x03002f78,0x1234);
    const auto state = machine.cpu_state(arm9); const auto clock = machine.scheduler_state();
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(Cartridge{invalid}); }, "chargement invalide accepté");
    check(machine.memory().read32(arm9,header) == 0 && machine.memory().read32(arm9,0x03002f78) == 0x1234 &&
          machine.cpu_state(arm9) == state && machine.scheduler_state() == clock, "chargement refusé modifie le contexte direct");
    machine.load_cartridge(Cartridge{rom});
    check(machine.memory().read32(arm9,header) == 0x544f4f42 && machine.memory().read32(arm9,0x03002f78) == 0,
          "rechargement ne rétablit pas les paramètres et les piles vides");
    auto copy = machine; copy.memory().write32(arm9,header,0); copy.memory().write32(arm9,0x03002f78,0x5678);
    check(machine.memory().read32(arm9,header) == 0x544f4f42 && machine.memory().read32(arm9,0x03002f78) == 0,
          "copies partagent les paramètres ou piles");
    auto overlap = rom; put32(overlap,0x24,0x023ffe00); put32(overlap,0x28,0x023ffe00);
    machine.load_cartridge(Cartridge{overlap});
    check(machine.memory().read32(arm9,header) == 0xeafffffe, "paramètres de boot écrasent un segment explicitement chargé");
    machine.memory().clear_ram();
    check(machine.memory().read32(arm9,header) == 0 && machine.memory().cp15().read(1,0,0) == 0x2078,
          "clear_ram simule implicitement le boot de Machine");
}
}

int main() {
    ram_parameters(); firmware_metadata(); cpu_context(); stack_and_exception_programs(); thumb_and_header_programs();
    overlap_reload_and_failure();
}
