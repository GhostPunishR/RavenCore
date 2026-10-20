#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <vector>

using ravenemu::nds::Cpu;
using ravenemu::nds::CpuMode;
using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t ime = 0x04000208;
constexpr std::uint32_t ie = 0x04000210;
constexpr std::uint32_t flags = 0x04000214;
constexpr std::uint32_t timer0 = 1U << 3U;
constexpr std::uint32_t keypad = 1U << 12U;
constexpr std::uint32_t ipc_sync = 1U << 16U;
constexpr std::uint32_t base = 0x02000000;

namespace {

void register_test(Processor cpu) {
    Memory memory;
    check(memory.read32(cpu, ime) == 0 && memory.read32(cpu, ie) == 0 && memory.read32(cpu, flags) == 0,
          "registres IRQ initiaux");
    memory.request_interrupt(cpu, timer0 | keypad | ipc_sync);
    check(memory.read32(cpu, flags) == (timer0 | keypad | ipc_sync) && !memory.irq_pending(cpu),
          "IF doit retenir les sources avec IME et IE désactivés");
    memory.write32(cpu, ime, 1);
    check(!memory.irq_pending(cpu), "IME sans IE ne doit pas lever IRQ");
    memory.write32(cpu, ie, timer0);
    check(memory.irq_pending(cpu), "IME et IE/IF doivent lever IRQ");
    memory.write32(cpu, ie, 0);
    check(!memory.irq_pending(cpu) && memory.read32(cpu, flags) == (timer0 | keypad | ipc_sync),
          "désactiver IE ne doit pas perdre les demandes");
    memory.write32(cpu, ie, timer0 | keypad | ipc_sync);
    memory.write32(cpu, flags, 0);
    check(memory.irq_pending(cpu), "écrire zéro dans IF ne doit rien acquitter");
    memory.write8(cpu, flags, static_cast<std::uint8_t>(timer0));
    check(memory.read32(cpu, flags) == (keypad | ipc_sync) && memory.irq_pending(cpu), "acquittement IF par octet");
    memory.write16(cpu, flags, static_cast<std::uint16_t>(keypad));
    check(memory.read32(cpu, flags) == ipc_sync, "acquittement IF par demi-mot bas");
    memory.write16(cpu, flags + 2, 1);
    check(memory.read32(cpu, flags) == 0 && !memory.irq_pending(cpu), "acquittement IF par demi-mot haut");
    memory.request_interrupt(cpu, timer0);
    memory.write32(cpu, ime, 0xfffffffe);
    check(memory.read32(cpu, ime) == 0 && !memory.irq_pending(cpu), "IME n'implémente que le bit 0");
    memory.write32(cpu, ime, 0xffffffff);
    memory.write8(cpu, ime + 1, 0);
    memory.write16(cpu, ime + 2, 0xffff);
    check(memory.read32(cpu, ime) == 1 && memory.read16(cpu, ime + 2) == 0 && memory.irq_pending(cpu),
          "octets réservés IME ne doivent pas modifier bit 0");
    memory.clear_ram();
    check(memory.read32(cpu, ime) == 0 && memory.read32(cpu, ie) == 0 && memory.read32(cpu, flags) == 0 &&
          !memory.irq_pending(cpu), "reset du bus doit remettre IME/IE/IF à zéro");
}

void masks_and_lanes_test(Processor cpu) {
    Memory memory;
    const auto valid = cpu == arm9 ? 0x003f3f7fU : 0x01df3fffU;
    memory.write32(cpu, ie, 0xffffffff);
    memory.request_interrupt(cpu, 0xffffffff);
    check(memory.read32(cpu, ie) == valid && memory.read32(cpu, flags) == valid,
          "bits réservés ou sources propres à l'autre CPU visibles");
    memory.write8(cpu, ie, static_cast<std::uint8_t>(timer0));
    check(memory.read32(cpu, ie) == ((valid & ~0xffU) | timer0), "IE : écriture octet altère les autres voies");
    memory.write16(cpu, ie + 2, 1);
    memory.write16(cpu, ie, 0x1008);
    check(memory.read32(cpu, ie) == 0x00011008, "IE : écriture demi-mot altère les autres voies");
    memory.write32(cpu, flags, 0xffffffff);
    memory.request_interrupt(cpu, 1U << 21U); // FIFO géométrique ARM9
    check(memory.read32(cpu, flags) == (cpu == arm9 ? 1U << 21U : 0U), "source géométrique propre à ARM9");
    memory.write32(cpu, flags, 0xffffffff);
    memory.request_interrupt(cpu, 1U << 22U); // ouverture des écrans ARM7
    check(memory.read32(cpu, flags) == (cpu == arm7 ? 1U << 22U : 0U), "source capot propre à ARM7");
    memory.write32(cpu, flags, 0xffffffff);
    memory.request_interrupt(cpu, 1U << 24U); // Wi-Fi ARM7
    check(memory.read32(cpu, flags) == (cpu == arm7 ? 1U << 24U : 0U), "source Wi-Fi propre à ARM7");
    memory.write32(cpu, flags, 0xffffffff);
    memory.request_interrupt(cpu, 1U << 7U); // SIO/RCNT/RTC ARM7, absent sur ARM9
    check(memory.read32(cpu, flags) == (cpu == arm7 ? 1U << 7U : 0U), "source RTC propre à ARM7");
}

void isolation_test() {
    Memory memory;
    memory.write32(arm9, ie, timer0);
    memory.write32(arm9, ime, 1);
    memory.request_interrupt(arm9, timer0);
    check(memory.irq_pending(arm9) && !memory.irq_pending(arm7) && memory.read32(arm7, flags) == 0 &&
          memory.read32(arm7, ie) == 0 && memory.read32(arm7, ime) == 0, "registres IRQ partagés entre CPU");
    memory.write32(arm7, flags, 0xffffffff);
    check(memory.irq_pending(arm9), "acquitter ARM7 a effacé IF ARM9");
    auto copy = memory;
    copy.write32(arm9, flags, timer0);
    check(!copy.irq_pending(arm9) && memory.irq_pending(arm9), "copie du bus partage le contrôleur IRQ");
}

void wram_register_test() {
    Memory memory;
    memory.write8(arm9, 0x04000247, 0xff);
    check(memory.wram_control() == 3 && memory.read8(arm9, 0x04000247) == 3 &&
          memory.read8(arm7, 0x04000241) == 3, "WRAMCNT/WRAMSTAT ne désignent pas la même attribution");
    memory.write8(arm7, 0x04000241, 0); // lecture seule
    memory.write8(arm7, 0x03000000, 0x55);
    check(memory.read8(arm7, 0x03008000) == 0x55 && memory.wram_control() == 3, "écriture dans WRAMSTAT a changé WRAM");
    expect_failure<ravenemu::Error>([&] { memory.write8(arm9, 0x03000000, 1); }, "ARM9 accède à WRAM en mode 3");
    expect_failure<ravenemu::Error>([&] { memory.write8(arm7, 0x04000247, 0); }, "ARM7 peut écrire WRAMCNT");
    memory.write8(arm9, 0x04000247, 0);
    check(memory.read8(arm9, 0x03000000) == 0x55, "réattribution WRAM par MMIO a perdu les données");
}

void fault_test(Processor cpu) {
    Memory memory;
    memory.write32(cpu, ie, timer0);
    memory.write32(cpu, ime, 1);
    memory.request_interrupt(cpu, timer0);
    const std::array<std::uint32_t, 2> words{timer0, 0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(cpu, flags, words); },
                                   "transfert vers IF puis registre non implémenté accepté");
    check(memory.read32(cpu, flags) == timer0 && memory.irq_pending(cpu), "écriture refusée a acquitté IF partiellement");
    expect_failure<ravenemu::Error>([&] { memory.write16(cpu, flags + 1, 0xffff); }, "IF non aligné accepté");
    expect_failure<ravenemu::Error>([&] { memory.write32(cpu, 0x0400020c, 1); }, "trou MMIO accepté");
    check(memory.read32(cpu, flags) == timer0 && memory.read32(cpu, ie) == timer0 && memory.read32(cpu, ime) == 1,
          "erreur de bus a modifié les IRQ");
    // Un transfert multiple peut écrire IE puis acquitter IF, sans IRQ fantôme.
    const std::array<std::uint32_t, 2> valid{timer0, timer0};
    memory.write_words(cpu, ie, valid);
    check(!memory.irq_pending(cpu) && memory.read32(cpu, flags) == 0, "STM vers IE/IF n'a pas acquitté la demande");
}

void standalone_cpu_test(Processor processor) {
    Memory memory;
    Cpu cpu{processor};
    cpu.reset(base, false);
    memory.write32(processor, base, 0xe321f01f); // MSR cpsr_c,#0x1f
    memory.write32(processor, ie, timer0);
    memory.write32(processor, ime, 1);
    memory.request_interrupt(processor, timer0);
    cpu.step(memory); // I est encore masqué au début de ce pas
    check(cpu.state().mode == CpuMode::system, "CPSR.I doit masquer l'IRQ du contrôleur");
    cpu.step(memory);
    check(cpu.state().mode == CpuMode::irq && cpu.state().instructions == 1,
          "Cpu autonome ne voit pas la ligne IRQ du bus");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

std::vector<std::uint8_t> synthetic_bios(Processor cpu) {
    std::vector<std::uint8_t> bytes(cpu == arm9 ? 0x1000 : 0x4000);
    put32(bytes, 0x18, 0xea000008); // B +0x40
    put32(bytes, 0x40, 0xe59f0010); // LDR r0,[pc,#16] -> +0x58
    put32(bytes, 0x44, 0xe3a01008); // MOV r1,#8
    put32(bytes, 0x48, 0xe5801000); // STR r1,[r0] : acquitter timer0 dans IF
    put32(bytes, 0x4c, 0xe2833001); // ADD r3,r3,#1 : compteur du gestionnaire
    put32(bytes, 0x50, 0xe25ef004); // SUBS pc,lr,#4
    put32(bytes, 0x58, flags);
    return bytes;
}

void machine_program_test() {
    std::vector<std::uint8_t> rom(0x274);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, base); put32(rom, 0x28, base); put32(rom, 0x2c, 0x34);
    put32(rom, 0x30, 0x240); put32(rom, 0x34, 0x03800000); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 0x34);
    for (const auto offset : {0x200U, 0x240U}) {
        put32(rom, offset, 0xe59f0028); // LDR r0,[pc,#40] -> littéral +0x30
        put32(rom, offset + 4, 0xe3a01008); // MOV r1,#8 : timer0
        put32(rom, offset + 8, 0xe5801008); // STR r1,[r0,#8] : IE
        put32(rom, offset + 12, 0xe3a02001); // MOV r2,#1
        put32(rom, offset + 16, 0xe5802000); // STR r2,[r0] : IME
        put32(rom, offset + 20, 0xe321f01f); // MSR cpsr_c,#0x1f : démasquer I
        put32(rom, offset + 24, 0xe2844001); // ADD r4,r4,#1 : reprise du programme
        put32(rom, offset + 28, 0xeafffffe); // B .
        put32(rom, offset + 0x30, ime);
    }
    ravenemu::nds::Machine machine;
    machine.memory().load_bios(arm9, synthetic_bios(arm9));
    machine.memory().load_bios(arm7, synthetic_bios(arm7));
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7}) {
        machine.memory().request_interrupt(cpu, timer0); // IF retient la demande avant configuration
        machine.run_steps(cpu, 6);
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[4] == 0 &&
              machine.memory().irq_pending(cpu), "programme ARM n'a pas configuré IME/IE/CPSR.I");
    }
    for (const auto cpu : {arm9, arm7}) {
        const auto entry = cpu == arm9 ? base : 0x03800000U;
        machine.step(cpu);
        check(machine.cpu_state(cpu).mode == CpuMode::irq && machine.cpu_state(cpu).instructions == 6,
              "entrée IRQ issue de MMIO");
        machine.run_steps(cpu, 6); // branche, LDR, MOV, STR IF, ADD, retour
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[15] == entry + 24 &&
              machine.cpu_state(cpu).registers[3] == 1 && machine.cpu_state(cpu).registers[4] == 0 &&
              machine.memory().read32(cpu, flags) == 0 && !machine.memory().irq_pending(cpu),
              "gestionnaire ARM : acquittement IF ou retour d'exception");
        machine.step(cpu);
        check(machine.cpu_state(cpu).registers[4] == 1 && machine.cpu_state(cpu).instructions == 13,
              "IRQ réapparue après acquittement ou reprise du programme absente");
        if (cpu == arm9) check(machine.memory().irq_pending(arm7), "acquitter ARM9 a perdu la demande ARM7");
    }
    // Un chargement refusé préserve les IRQ, un chargement réussi les réinitialise.
    machine.memory().request_interrupt(arm9, timer0);
    auto invalid = rom;
    put32(invalid, 0x38, 0x04000000); put32(invalid, 0x34, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(ravenemu::nds::Cartridge{invalid}); },
                                         "chargement invalide accepté");
    check(machine.memory().irq_pending(arm9), "chargement refusé a effacé les IRQ");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    check(!machine.memory().irq_pending(arm9) && !machine.memory().irq_pending(arm7) &&
          machine.memory().read32(arm9, flags) == 0 && machine.memory().read32(arm7, ie) == 0,
          "chargement réussi conserve l'ancien contrôleur IRQ");
}

} // namespace

int main() {
    for (const auto cpu : {arm9, arm7}) {
        register_test(cpu);
        masks_and_lanes_test(cpu);
        fault_test(cpu);
        standalone_cpu_test(cpu);
    }
    isolation_test();
    wram_register_test();
    machine_program_test();
}
