#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <limits>
#include <vector>

using ravenemu::nds::CpuMode;
using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t registers = 0x04000100;
constexpr std::uint32_t flags = 0x04000214;

namespace {

void registers_test(Processor cpu) {
    for (unsigned i = 0; i < 4; ++i) {
        Memory memory;
        const auto address = registers + 4 * i;
        check(memory.read32(cpu, address) == 0, "timer non nul au reset");
        memory.write16(cpu, address, 0x1234);
        memory.write8(cpu, address, 0xab);
        memory.write8(cpu, address + 1, 0xcd);
        check(memory.read16(cpu, address) == 0, "écrire reload doit laisser le compteur arrêté intact");
        memory.write16(cpu, address + 2, 0xff80);
        check(memory.read32(cpu, address) == 0x0080cdab, "démarrage, voies reload ou bits réservés");
        memory.write8(cpu, address, 0x34);
        memory.write8(cpu, address + 1, 0x12);
        check(memory.read16(cpu, address) == 0xcdab, "écriture de reload a modifié le compteur actif");
        memory.advance_cycles(0x10000U - 0xcdabU);
        check(memory.read16(cpu, address) == 0x1234 && memory.read32(cpu, flags) == 0,
              "débordement ne charge pas reload ou génère une IRQ désactivée");
        memory.write8(cpu, address + 2, 0);
        memory.advance_cycles(100000);
        memory.write16(cpu, address, 0x5678);
        check(memory.read16(cpu, address) == 0x1234, "arrêt ne fige pas le compteur");
        memory.write8(cpu, address + 2, 0x80);
        memory.advance_cycles(2);
        memory.write8(cpu, address + 2, 0x80);
        check(memory.read16(cpu, address) == 0x567a, "réécrire start actif recharge le compteur");
        memory.write8(cpu, address + 2, 0);
        memory.write32(cpu, address, 0xfffffffE);
        check(memory.read32(cpu, address) == (i == 0 ? 0x00c3fffeU : 0x00c7fffeU),
              "écriture mot : nouveau reload au démarrage ou masque du contrôle");
    }
}

void prescalers_test(Processor cpu) {
    constexpr std::array<unsigned, 4> divisors{1, 64, 256, 1024};
    for (unsigned channel = 0; channel < 4; ++channel) {
        const auto address = registers + channel * 4;
        const auto source = 1U << (channel + 3U);
        for (unsigned selector = 0; selector < divisors.size(); ++selector) {
            Memory memory;
            const auto divisor = divisors[selector];
            memory.write32(cpu, address, ((0xc0U | selector) << 16U) | 0xfffdU);
            memory.advance_cycles(divisor - 1);
            check(memory.read16(cpu, address) == 0xfffd && memory.read32(cpu, flags) == 0, "préscaler avance trop tôt");
            memory.advance_cycles(1);
            check(memory.read16(cpu, address) == 0xfffe, "phase fractionnée du préscaler perdue");
            memory.advance_cycles(divisor);
            check(memory.read16(cpu, address) == 0xffff && memory.read32(cpu, flags) == 0, "IRQ avant débordement");
            memory.advance_cycles(divisor);
            check(memory.read16(cpu, address) == 0xfffd && memory.read32(cpu, flags) == source && !memory.irq_pending(cpu),
                  "reload/IF de débordement, indépendamment d'IME/IE");
            memory.write32(cpu, 0x04000210, source);
            memory.write32(cpu, 0x04000208, 1);
            check(memory.irq_pending(cpu), "débordement mémorisé ne lève pas IRQ après activation d'IME/IE");
            memory.write32(cpu, flags, source);
            memory.advance_cycles(0);
            check(memory.read32(cpu, flags) == 0, "zéro cycle a créé un débordement");
            memory.advance_cycles(divisor * 6);
            check(memory.read16(cpu, address) == 0xfffd && memory.read32(cpu, flags) == source,
                  "débordements multiples sur un seul avancement");
        }
    }
}

void phase_test(Processor cpu) {
    Memory memory;
    memory.write32(cpu, registers, 0x0081fffe); // /64
    memory.advance_cycles(63);
    memory.write8(cpu, registers + 2, 0xc1); // seule IRQ change
    memory.write8(cpu, registers + 3, 0xff); // réservé
    memory.advance_cycles(1);
    check(memory.read16(cpu, registers) == 0xffff, "changer IRQ ou l'octet réservé a perdu la phase");
    memory.advance_cycles(63);
    memory.write8(cpu, registers + 2, 0xc1); // contrôle inchangé
    memory.advance_cycles(1);
    check(memory.read16(cpu, registers) == 0xfffe && memory.read32(cpu, flags) == 8,
          "réécrire contrôle actif a perdu la phase");
    memory.advance_cycles(63);
    memory.write8(cpu, registers + 2, 0x41); // arrêter
    memory.advance_cycles(1024);
    memory.write8(cpu, registers + 2, 0xc1); // redémarrer à phase zéro
    memory.advance_cycles(1);
    check(memory.read16(cpu, registers) == 0xfffe, "redémarrage conserve une ancienne phase");
    memory.write8(cpu, registers + 2, 0xc0); // changer diviseur sans recharger
    memory.advance_cycles(1);
    check(memory.read16(cpu, registers) == 0xffff, "nouveau diviseur ne prend pas effet");
    memory.write8(cpu, registers + 2, 0xc4); // cascade interdite sur timer0
    memory.advance_cycles(1);
    check(memory.read16(cpu, registers) == 0xfffe && memory.read8(cpu, registers + 2) == 0xc0,
          "timer0 doit ignorer count-up");
}

void cascade_test(Processor cpu) {
    Memory memory;
    for (unsigned i = 0; i < 4; ++i)
        memory.write32(cpu, registers + 4 * i, ((i == 0 ? 0xc0U : 0xc7U) << 16U) | 0xfffeU);
    memory.advance_cycles(15);
    for (unsigned i = 0; i < 4; ++i)
        check(memory.read16(cpu, registers + 4 * i) == 0xffff, "cascade de quatre timers avant retenue finale");
    check(memory.read32(cpu, flags) == 0x38, "IRQ timer3 avant son premier débordement");
    memory.advance_cycles(1);
    for (unsigned i = 0; i < 4; ++i)
        check(memory.read16(cpu, registers + 4 * i) == 0xfffe, "propagation de débordement sur quatre timers");
    check(memory.read32(cpu, flags) == 0x78, "IRQ des quatre timers en cascade");
    memory.write8(cpu, registers + 2, 0); // timer0 arrêté coupe la chaîne
    memory.advance_cycles(1000000);
    check(memory.read16(cpu, registers + 12) == 0xfffe, "cascade avance sans débordement du précédent");
    memory.write8(cpu, registers + 10, 0);
    memory.write32(cpu, registers + 8, 0x00800000); // redémarrer timer2 à zéro sur l'horloge, IRQ désactivée
    memory.advance_cycles(0x10000);
    check(memory.read16(cpu, registers + 8) == 0 && memory.read16(cpu, registers + 12) == 0xffff,
          "cascade doit compter un débordement même si l'IRQ amont est désactivée");
    memory.write8(cpu, registers + 14, 0x80); // timer3 repasse à l'horloge sans recharger
    memory.advance_cycles(1);
    check(memory.read16(cpu, registers + 12) == 0xfffe, "changement de source recharge ou bloque timer3");
}

void large_intervals_test(Processor cpu) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    Memory memory;
    for (unsigned i = 0; i < 4; ++i)
        memory.write32(cpu, registers + 4 * i, ((i == 0 ? 0xc0U : 0xc4U) << 16U) | 0xffffU);
    memory.advance_cycles(maximum);
    for (unsigned i = 0; i < 4; ++i)
        check(memory.read16(cpu, registers + 4 * i) == 0xffff, "cascade avec UINT64_MAX débordements");
    check(memory.read32(cpu, flags) == 0x78, "IRQ après grand intervalle");
    memory.clear_ram();
    memory.write32(cpu, registers, 0x00c30000); // /1024
    memory.advance_cycles(maximum);
    check(memory.read16(cpu, registers) == 0xffff, "division du grand intervalle");
    memory.advance_cycles(maximum);
    check(memory.read16(cpu, registers) == 0xffff, "addition phase + grand intervalle déborde");
    memory.advance_cycles(2);
    check(memory.read16(cpu, registers) == 0, "reste du grand intervalle non conservé");
}

void batching_test() {
    Memory batched;
    for (const auto cpu : {arm9, arm7}) {
        const std::array<unsigned, 4> controls = cpu == arm9 ?
            std::array<unsigned, 4>{0xc0, 0xc5, 0xc1, 0xc4} : std::array<unsigned, 4>{0xc2, 0xc3, 0xc4, 0xc6};
        for (unsigned i = 0; i < 4; ++i)
            batched.write32(cpu, registers + i * 4, (controls[i] << 16U) | (0xfff1U + i));
    }
    auto individual = batched;
    for (const auto cycles : {1U, 63U, 1025U, 65117U, 11903U}) {
        batched.advance_cycles(cycles);
        for (unsigned i = 0; i < cycles; ++i) individual.advance_cycles(1);
        for (const auto cpu : {arm9, arm7}) {
            for (unsigned i = 0; i < 4; ++i)
                check(batched.read32(cpu, registers + i * 4) == individual.read32(cpu, registers + i * 4),
                      "avancement groupé diffère des cycles individuels");
            check(batched.read32(cpu, flags) == individual.read32(cpu, flags), "IF dépend du découpage temporel");
        }
    }
}

void reset_copy_fault_test() {
    Memory memory;
    memory.write32(arm9, registers, 0x00c0fffe);
    memory.write32(arm7, registers, 0x00c1fffe);
    memory.advance_cycles(2);
    check(memory.read32(arm9, flags) == 8 && memory.read32(arm7, flags) == 0,
          "les deux CPU doivent partager l'horloge mais pas les compteurs ou IRQ");
    auto copy = memory;
    copy.write32(arm9, flags, 8);
    copy.advance_cycles(62);
    check(copy.read16(arm7, registers) == 0xffff && memory.read16(arm7, registers) == 0xfffe &&
          memory.read32(arm9, flags) == 8, "copie du bus partage timers, phases ou IRQ");
    const std::array<std::uint32_t, 2> words{0x00c0ffff, 0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm9, registers + 12, words); },
                                   "transfert au-delà des timers accepté");
    check(memory.read32(arm9, registers + 12) == 0, "transfert refusé a démarré timer3");
    expect_failure<ravenemu::Error>([&] { memory.write16(arm9, registers + 1, 0xffff); }, "timer non aligné accepté");
    memory.clear_ram();
    memory.advance_cycles(1000000);
    for (const auto cpu : {arm9, arm7}) {
        for (unsigned i = 0; i < 4; ++i) check(memory.read32(cpu, registers + 4 * i) == 0, "reset conserve un timer");
        check(memory.read32(cpu, flags) == 0, "reset timer conserve une IRQ");
    }
    memory.write8(arm9, registers + 2, 0x80);
    check(memory.read16(arm9, registers) == 0, "reset conserve reload");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

void machine_program_test() {
    // Même programme ARM sur les deux CPU : configurer timer0, IME/IE et CPSR.I.
    std::vector<std::uint8_t> rom(0x2a0);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, 0x02000000); put32(rom, 0x28, 0x02000000); put32(rom, 0x2c, 0x50);
    put32(rom, 0x30, 0x250); put32(rom, 0x34, 0x03800000); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 0x50);
    for (const auto offset : {0x200U, 0x250U}) {
        put32(rom, offset,      0xe59f0038); // LDR r0,[pc,#56] -> +0x40 (timers)
        put32(rom, offset + 4,  0xe59f1038); // LDR r1,[pc,#56] -> +0x44 (reload/contrôle)
        put32(rom, offset + 8,  0xe5801000); // STR r1,[r0] : reload=fffc, IRQ et start
        put32(rom, offset + 12, 0xe59f0034); // LDR r0,[pc,#52] -> +0x48 (IME)
        put32(rom, offset + 16, 0xe3a01008); // MOV r1,#8
        put32(rom, offset + 20, 0xe5801008); // STR r1,[r0,#8] : IE
        put32(rom, offset + 24, 0xe3a01001); // MOV r1,#1
        put32(rom, offset + 28, 0xe5801000); // STR r1,[r0] : IME
        put32(rom, offset + 32, 0xe321f01f); // MSR cpsr_c,#0x1f
        put32(rom, offset + 36, 0xe2844001); // ADD r4,r4,#1 après retour
        put32(rom, offset + 40, 0xeafffffe); // B .
        put32(rom, offset + 0x40, registers);
        put32(rom, offset + 0x44, 0x00c0fffc);
        put32(rom, offset + 0x48, 0x04000208);
    }
    ravenemu::nds::Machine machine;
    for (const auto cpu : {arm9, arm7}) {
        std::vector<std::uint8_t> bios(cpu == arm9 ? 0x1000 : 0x4000);
        put32(bios, 0x18, 0xea000008); // B +0x40
        put32(bios, 0x40, 0xe59f0010); // LDR r0,[pc,#16] -> +0x58 (IF)
        put32(bios, 0x44, 0xe3a01008); // MOV r1,#8
        put32(bios, 0x48, 0xe5801000); // STR r1,[r0] : acquitter
        put32(bios, 0x4c, 0xe2833001); // ADD r3,r3,#1
        put32(bios, 0x50, 0xe25ef004); // SUBS pc,lr,#4
        put32(bios, 0x58, flags);
        machine.memory().load_bios(cpu, bios);
    }
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7}) {
        machine.run_steps(cpu, 9);
        check(machine.memory().read16(cpu, registers) == 0xfffc && !machine.memory().irq_pending(cpu),
              "pas CPU ne doivent pas avancer implicitement l'horloge des timers");
    }
    machine.advance_cycles(3);
    check(!machine.memory().irq_pending(arm9) && !machine.memory().irq_pending(arm7), "IRQ timer trop tôt");
    machine.advance_cycles(1);
    for (const auto cpu : {arm9, arm7}) {
        check(machine.memory().irq_pending(cpu), "timer n'a pas généré sa demande réelle");
        machine.step(cpu);
        check(machine.cpu_state(cpu).mode == CpuMode::irq && machine.cpu_state(cpu).instructions == 9, "entrée IRQ timer");
        machine.run_steps(cpu, 6);
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[3] == 1 &&
              !machine.memory().irq_pending(cpu), "acquittement/retour depuis le gestionnaire ARM de timer");
        machine.step(cpu);
        check(machine.cpu_state(cpu).registers[4] == 1, "programme ne reprend pas après IRQ timer");
    }
    auto copy = machine;
    copy.advance_cycles(4);
    check(copy.memory().irq_pending(arm9) && !machine.memory().irq_pending(arm9), "copie de machine partage timers");
    auto invalid = rom;
    put32(invalid, 0x38, 0x04000000); put32(invalid, 0x34, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(ravenemu::nds::Cartridge{invalid}); },
                                         "chargement invalide accepté");
    check(machine.memory().read32(arm9, registers) == 0x00c0fffc, "chargement refusé a effacé les timers");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    machine.advance_cycles(100000);
    check(machine.memory().read32(arm9, registers) == 0 && machine.memory().read32(arm7, registers) == 0 &&
          !machine.memory().irq_pending(arm9) && !machine.memory().irq_pending(arm7), "chargement conserve les timers");
}

} // namespace

int main() {
    for (const auto cpu : {arm9, arm7}) {
        registers_test(cpu);
        prescalers_test(cpu);
        phase_test(cpu);
        cascade_test(cpu);
        large_intervals_test(cpu);
    }
    batching_test();
    reset_copy_fault_test();
    machine_program_test();
}
