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
constexpr std::uint32_t sync_register = 0x04000180;
constexpr std::uint32_t control = 0x04000184;
constexpr std::uint32_t send = 0x04000188;
constexpr std::uint32_t receive = 0x04100000;
constexpr std::uint32_t flags = 0x04000214;
constexpr std::uint32_t sync_irq = 1U << 16U;
constexpr std::uint32_t empty_irq = 1U << 17U;
constexpr std::uint32_t receive_irq = 1U << 18U;

namespace {

constexpr Processor other(Processor cpu) { return cpu == arm9 ? arm7 : arm9; }

void enable(Memory& memory) {
    for (const auto cpu : {arm9, arm7}) memory.write16(cpu, control, 0x8000);
}

void sync_test(Processor sender) {
    const auto receiver = other(sender);
    Memory memory;
    check(memory.read32(sender, sync_register) == 0 && memory.read32(receiver, sync_register) == 0, "IPCSYNC initial");
    memory.write16(sender, sync_register, 0xffff);
    check(memory.read32(sender, sync_register) == 0x4f00 && memory.read32(receiver, sync_register) == 15 &&
          memory.read32(receiver, flags) == 0, "IPCSYNC : masques, données ou IRQ réceptrice désactivée");
    memory.write16(receiver, sync_register, 0x4500);
    check(memory.read16(sender, sync_register) == 0x4f05 && memory.read16(receiver, sync_register) == 0x450f, "nibbles IPCSYNC croisés");
    memory.write8(sender, sync_register, 0xff);
    memory.write16(sender, sync_register + 2, 0xffff);
    check(memory.read32(sender, sync_register) == 0x4f05, "voies réservées IPCSYNC modifient les données");
    memory.write8(sender, sync_register + 1, 0x2a); // pulse et nouvelle sortie, sans autorisation locale
    check(memory.read32(sender, sync_register) == 0x0a05 && memory.read16(receiver, sync_register) == 0x450a &&
          memory.read32(receiver, flags) == sync_irq && memory.read32(sender, flags) == 0,
          "pulse IPCSYNC doit demander l'IRQ uniquement au CPU distant autorisé");
    memory.write32(receiver, flags, sync_irq);
    memory.write8(sender, sync_register + 1, 0x0a);
    check(memory.read32(receiver, flags) == 0, "données seules ne doivent pas générer d'IRQ IPCSYNC");
    memory.write8(sender, sync_register + 1, 0x2a);
    check(memory.read32(receiver, flags) == sync_irq, "nouveau pulse IPCSYNC perdu");
    memory.write32(receiver, flags, sync_irq);
    memory.write16(receiver, sync_register, 0);
    memory.write8(sender, sync_register + 1, 0x20);
    memory.write16(receiver, sync_register, 0x4000);
    check(memory.read32(receiver, flags) == 0, "autoriser après un pulse ne doit pas le rejouer");
}

void fifo_order_test(Processor sender) {
    const auto receiver = other(sender);
    Memory memory;
    enable(memory);
    check(memory.read32(sender, control) == 0x8101 && memory.read32(receiver, control) == 0x8101, "FIFO vide initiale");
    for (unsigned i = 0; i < 16; ++i) memory.write32(sender, send, 0xabcdef00U + i);
    check(memory.read16(sender, control) == 0x8102 && memory.read16(receiver, control) == 0x8201,
          "statuts plein/vide vus par les deux CPU");
    memory.write32(sender, send, 0xdeadbeef);
    check(memory.read16(sender, control) == 0xc102 && memory.read16(receiver, control) == 0x8201,
          "envoyer dans une FIFO pleine doit lever seulement l'erreur locale");
    for (unsigned i = 0; i < 8; ++i)
        check(memory.read32(receiver, receive) == 0xabcdef00U + i, "ordre des premiers mots FIFO");
    for (unsigned i = 16; i < 24; ++i) memory.write32(sender, send, 0xabcdef00U + i);
    for (unsigned i = 8; i < 24; ++i)
        check(memory.read32(receiver, receive) == 0xabcdef00U + i, "ordre FIFO après bouclage de l'anneau");
    check(memory.read32(receiver, receive) == 0xabcdef17 && memory.read16(receiver, control) == 0xc101,
          "lecture vide doit répéter le dernier mot et lever l'erreur du lecteur");
    memory.write8(sender, control, 0xff); // bit erreur dans l'autre voie
    check((memory.read16(sender, control) & 0x4000U) != 0, "voie basse acquitte l'erreur haute");
    memory.write8(sender, control + 1, 0xc0);
    check((memory.read16(sender, control) & 0x4000U) == 0 && (memory.read16(receiver, control) & 0x4000U) != 0,
          "acquittement erreur doit être local et W1C");
    memory.write16(receiver, control, 0x8000);
    check((memory.read16(receiver, control) & 0x4000U) != 0, "écrire zéro acquitte l'erreur FIFO");
    memory.write16(receiver, control, 0xc000);
    check(memory.read32(receiver, control) == 0x8101, "acquittement erreur FIFO");
}

void disabled_and_clear_test(Processor sender) {
    const auto receiver = other(sender);
    Memory memory;
    memory.write32(sender, send, 1);
    check(memory.peek32(receiver, receive) == 0 && memory.read16(sender, control) == 0x0101,
          "envoyer avec FIFO désactivée ne doit stocker ni lever d'erreur");
    memory.write16(sender, control, 0x8000);
    memory.write32(sender, send, 0x12345678);
    check(memory.read32(receiver, receive) == 0x12345678 && memory.read32(receiver, receive) == 0x12345678 &&
          memory.read16(sender, control) == 0x8100 && memory.read16(receiver, control) == 1,
          "lecture désactivée doit regarder le premier mot sans le retirer");
    memory.write16(receiver, control, 0x8000);
    check(memory.read32(receiver, receive) == 0x12345678 && memory.read16(sender, control) == 0x8101,
          "réactivation FIFO ne conserve pas le mot non retiré");
    memory.write16(receiver, control, 0);
    check(memory.read32(receiver, receive) == 0x12345678 && (memory.read16(receiver, control) & 0x4000U),
          "lecture vide désactivée doit aussi lever l'erreur");
    enable(memory);
    memory.write32(receiver, send, 0xfedcba98); // direction indépendante
    memory.write32(sender, send, 0x11111111);
    memory.write16(sender, control, 0x8008); // vider seulement la file sortante
    check(memory.peek32(receiver, receive) == 0 && memory.read32(sender, receive) == 0xfedcba98 &&
          (memory.read16(receiver, control) & 0x4000U), "clear doit préserver l'autre direction et l'erreur distante");
    check(memory.read32(receiver, receive) == 0, "clear doit effacer aussi le dernier mot mémorisé");
    memory.write32(sender, send, 0x76543210);
    memory.write16(sender, control, 0); // arrêter ne vide pas
    check(memory.read32(receiver, receive) == 0x76543210, "désactiver l'émetteur efface sa file");
}

void irq_edges_test(Processor sender) {
    const auto receiver = other(sender);
    Memory memory;
    memory.write16(sender, control, 4); // même désactivée, condition IRQ vide devient vraie
    check(memory.read32(sender, flags) == empty_irq && !memory.irq_pending(sender), "autorisation IRQ vide sur file vide");
    memory.write32(sender, flags, empty_irq);
    memory.write16(sender, control, 4);
    check(memory.read32(sender, flags) == 0, "IRQ FIFO vide doit être un front, pas un niveau");
    memory.write16(sender, control, 0x8004);
    memory.write16(receiver, control, 0x8400);
    for (const auto cpu : {sender, receiver}) {
        memory.write32(cpu, 0x04000210, empty_irq | receive_irq);
        memory.write32(cpu, 0x04000208, 1);
    }
    memory.write32(sender, send, 11);
    check(memory.read32(receiver, flags) == receive_irq && memory.irq_pending(receiver) && !memory.irq_pending(sender),
          "premier mot doit demander l'IRQ distante de réception");
    memory.write32(receiver, flags, receive_irq);
    memory.write32(sender, send, 12);
    check(memory.read32(receiver, flags) == 0 && !memory.irq_pending(receiver), "second mot dans une file non vide recrée IRQ");
    check(memory.read32(receiver, receive) == 11 && memory.read32(sender, flags) == 0, "IRQ émetteur avant dernier retrait");
    check(memory.read32(receiver, receive) == 12 && memory.read32(sender, flags) == empty_irq && memory.irq_pending(sender),
          "dernier retrait doit demander l'IRQ distante d'émission vide");
    memory.write32(sender, flags, empty_irq);
    memory.write16(sender, control, 0x8004);
    memory.write16(sender, control, 0x800c); // vider une file déjà vide
    check(!memory.irq_pending(sender), "condition restée vraie réaffirme l'IRQ après acquittement");
    memory.write16(sender, control, 0x8000);
    memory.write16(sender, control, 0x8004);
    check(memory.read32(sender, flags) == empty_irq, "réautoriser IRQ vide ne recrée pas de front");
    memory.write32(sender, flags, empty_irq);
    memory.write16(receiver, control, 0x8000); // réception IRQ désactivée
    memory.write32(sender, send, 13);
    check(memory.read32(receiver, flags) == 0, "IRQ réception masquée");
    memory.write16(receiver, control, 0x8400);
    check(memory.read32(receiver, flags) == receive_irq, "autoriser réception sur file non vide doit créer un front");
    memory.write32(receiver, flags, receive_irq);
    memory.write16(receiver, control, 0x0400); // FIFO désactivée, condition IRQ conservée
    check(memory.read32(receiver, receive) == 13 && memory.read32(sender, flags) == 0, "lecture désactivée a vidé la file");
    memory.write16(sender, control, 0x800c);
    check(memory.read32(sender, flags) == empty_irq && memory.read32(receiver, flags) == 0,
          "clear doit produire le front vide local sans front de réception distant");
}

void inspection_and_fault_test(Processor sender) {
    const auto receiver = other(sender);
    Memory memory;
    enable(memory);
    memory.write16(sender, control, 0x8004);
    memory.write32(sender, flags, empty_irq);
    memory.write32(sender, send, 0x12345678);
    const Memory& view = memory;
    check(view.peek32(receiver, receive) == 0x12345678 && view.peek32(receiver, receive) == 0x12345678 &&
          view.peek8(sender, control) == 4 && view.peek16(receiver, control) == 0x8001,
          "inspection FIFO doit être stable et sans retrait");
    for (unsigned offset = 0; offset < 4; ++offset) {
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read8(receiver, receive + offset)); }, "FIFO octet acceptée");
        expect_failure<ravenemu::Error>([&] { memory.write8(sender, send + offset, 0); }, "envoi octet accepté");
    }
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(receiver, receive)); }, "FIFO demi-mot acceptée");
    expect_failure<ravenemu::Error>([&] { memory.write16(sender, send, 0); }, "envoi demi-mot accepté");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(sender, send)); }, "lecture FIFO émission acceptée");
    expect_failure<ravenemu::Error>([&] { memory.write32(receiver, receive, 0); }, "écriture FIFO réception acceptée");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(receiver, receive + 1)); }, "FIFO non alignée acceptée");
    const std::array<std::uint32_t, 2> outgoing{0xaabbccdd, 0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(sender, send, outgoing); }, "transfert sortant tronqué accepté");
    std::array<std::uint32_t, 2> incoming{0xaaaaaaaa, 0xbbbbbbbb};
    expect_failure<ravenemu::Error>([&] { memory.read_words(receiver, receive, incoming); }, "transfert entrant tronqué accepté");
    check(incoming[0] == 0xaaaaaaaa && incoming[1] == 0xbbbbbbbb && memory.read32(sender, flags) == 0 &&
          memory.read16(receiver, control) == 0x8001, "transfert refusé a retiré un mot, levé une erreur ou modifié le résultat");
    memory.read_words(receiver, receive, std::span<std::uint32_t>{incoming}.first(1));
    check(incoming[0] == 0x12345678 && memory.read32(sender, flags) == empty_irq, "lecture multiple valide ne retire pas exactement un mot");
    check(view.peek32(receiver, receive) == 0x12345678 && !(view.peek16(receiver, control) & 0x4000U),
          "inspection vide doit conserver la dernière valeur sans erreur");
    const std::array<std::uint32_t, 4> invalid{0x2000, 0xffff, 99, 0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(sender, sync_register, invalid); }, "écriture IPC puis trou acceptée");
    check(memory.read32(sender, sync_register) == 0 && memory.read16(sender, control) == 0x8105,
          "transfert refusé modifie contrôle ou synchronisation");
    const std::array<std::uint32_t, 2> valid{0x8004, 0xfedcba98};
    memory.write_words(sender, control, valid);
    check(memory.read32(receiver, receive) == 0xfedcba98, "transfert contrôle et FIFO valide");
}

void cpu_transfer_test(Processor receiver) {
    Memory memory;
    enable(memory);
    memory.write32(other(receiver), send, 0xabcdef12);
    Cpu cpu{receiver};
    cpu.reset(0x02000000, false);
    cpu.set_register(4, receive);
    memory.write32(receiver, 0x02000000, 0xe8b40003); // LDMIA r4!,{r0,r1}, deuxième mot non mappé
    const auto before = cpu.state();
    expect_failure<ravenemu::nds::CpuExecutionError>([&] { cpu.step(memory); }, "LDM IPC avec seconde adresse absente accepté");
    check(cpu.state() == before && memory.peek32(receiver, receive) == 0xabcdef12 &&
          memory.read16(receiver, control) == 0x8001, "LDM refusé a modifié les registres ou vidé la FIFO");
    memory.write32(receiver, 0x02000000, 0xe8b40001); // LDMIA r4!,{r0}
    cpu.step(memory);
    check(cpu.state().registers[0] == 0xabcdef12 && cpu.state().registers[4] == receive + 4 &&
          memory.read16(receiver, control) == 0x8101, "LDM IPC valide sans retrait ou writeback");
    cpu.reset(0x02000000, true);
    cpu.set_register(0, receive);
    memory.write32(other(receiver), send, 0x76543210);
    memory.write16(receiver, 0x02000000, 0x6801); // LDR r1,[r0]
    cpu.step(memory);
    check(cpu.state().registers[1] == 0x76543210 && memory.read16(receiver, control) == 0x8101,
          "LDR Thumb ne retire pas un mot IPC");
}

void reset_and_copy_test() {
    Memory memory;
    enable(memory);
    memory.write16(arm9, sync_register, 0x4a00);
    memory.write32(arm9, send, 0x11223344);
    auto copy = memory;
    check(copy.read32(arm7, receive) == 0x11223344 && memory.read16(arm7, control) == 0x8001,
          "copie de bus partage sa FIFO IPC");
    copy.write16(arm9, sync_register, 0);
    check(memory.read16(arm7, sync_register) == 10, "copie de bus partage IPCSYNC");
    memory.clear_ram();
    for (const auto cpu : {arm9, arm7})
        check(memory.peek32(cpu, receive) == 0 && memory.read32(cpu, sync_register) == 0 &&
              memory.read32(cpu, control) == 0x0101 && memory.read32(cpu, flags) == 0, "reset IPC incomplet");
    memory.write16(arm9, control, 0xffff);
    memory.write16(arm9, control + 2, 0xffff);
    check(memory.read32(arm9, control) == 0x8505, "masque des bits FIFO réservés ou W1C");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

void machine_exchange_test() {
    std::vector<std::uint8_t> rom(0x300);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, 0x02000000); put32(rom, 0x28, 0x02000000); put32(rom, 0x2c, 0x80);
    put32(rom, 0x30, 0x280); put32(rom, 0x34, 0x03800000); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 0x80);
    for (const auto offset : {0x200U, 0x280U}) {
        put32(rom, offset,      0xe59f0058); // LDR r0,[pc,#88] -> +0x60, FIFO contrôle
        put32(rom, offset + 4,  0xe59f1058); // LDR r1,[pc,#88] -> +0x64, 8400
        put32(rom, offset + 8,  0xe5801000); // STR r1,[r0]
        put32(rom, offset + 12, 0xe59f0054); // LDR r0,[pc,#84] -> +0x68, IME
        put32(rom, offset + 16, 0xe59f1054); // LDR r1,[pc,#84] -> +0x6c, IRQ réception
        put32(rom, offset + 20, 0xe5801008); // STR r1,[r0,#8] : IE
        put32(rom, offset + 24, 0xe3a01001); // MOV r1,#1
        put32(rom, offset + 28, 0xe5801000); // STR r1,[r0] : IME
        put32(rom, offset + 32, 0xe321f01f); // MSR cpsr_c,#0x1f
        put32(rom, offset + 36, 0xe59f0044); // LDR r0,[pc,#68] -> +0x70, SEND
        put32(rom, offset + 40, 0xe59f1044); // LDR r1,[pc,#68] -> +0x74, message
        put32(rom, offset + 44, 0xe5801000); // STR r1,[r0]
        put32(rom, offset + 48, 0xe2844001); // ADD r4,r4,#1 après retour
        put32(rom, offset + 52, 0xeafffffe); // B .
        put32(rom, offset + 0x60, control); put32(rom, offset + 0x64, 0x8400);
        put32(rom, offset + 0x68, 0x04000208); put32(rom, offset + 0x6c, receive_irq);
        put32(rom, offset + 0x70, send); put32(rom, offset + 0x74, 0x12345678);
    }
    // ARM7 attend le message au lieu d'en envoyer un spontanément.
    put32(rom, 0x280 + 36, 0xe2844001); // ADD r4,r4,#1 après retour
    put32(rom, 0x280 + 40, 0xeafffffe);
    ravenemu::nds::Machine machine;
    for (const auto cpu : {arm9, arm7}) {
        std::vector<std::uint8_t> bios(cpu == arm9 ? 0x1000 : 0x4000);
        put32(bios, 0x18, 0xea000008); // B +0x40
        put32(bios, 0x40, 0xe59f0038); // LDR r0,[pc,#56] -> +0x80, RECV
        put32(bios, 0x44, 0xe5902000); // LDR r2,[r0] : retirer le message
        if (cpu == arm7) {
            put32(bios, 0x48, 0xe2822001); // ADD r2,r2,#1
            put32(bios, 0x4c, 0xe59f0030); // LDR r0,[pc,#48] -> +0x84, SEND
            put32(bios, 0x50, 0xe5802000); // STR r2,[r0] : réponse à ARM9
            put32(bios, 0x54, 0xe59f002c); // LDR r0,[pc,#44] -> +0x88, IF
            put32(bios, 0x58, 0xe59f102c); // LDR r1,[pc,#44] -> +0x8c, IRQ réception
            put32(bios, 0x5c, 0xe5801000); // STR r1,[r0] : acquitter
            put32(bios, 0x60, 0xe2833001); // ADD r3,r3,#1
            put32(bios, 0x64, 0xe25ef004); // SUBS pc,lr,#4
        } else {
            put32(bios, 0x48, 0xe59f0038); // LDR r0,[pc,#56] -> +0x88, IF
            put32(bios, 0x4c, 0xe59f1038); // LDR r1,[pc,#56] -> +0x8c
            put32(bios, 0x50, 0xe5801000); // STR r1,[r0] : acquitter
            put32(bios, 0x54, 0xe2833001); // ADD r3,r3,#1
            put32(bios, 0x58, 0xe25ef004); // SUBS pc,lr,#4
        }
        put32(bios, 0x80, receive); put32(bios, 0x84, send);
        put32(bios, 0x88, flags); put32(bios, 0x8c, receive_irq);
        machine.memory().load_bios(cpu, bios);
    }
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7}) machine.run_steps(cpu, 9);
    machine.run_steps(arm9, 3); // envoi depuis le programme, aucun événement injecté
    check(machine.memory().irq_pending(arm7) && !machine.memory().irq_pending(arm9), "message ARM9 ne déclenche pas ARM7");
    machine.step(arm7);
    check(machine.cpu_state(arm7).mode == CpuMode::irq, "entrée IRQ IPC ARM7");
    machine.run_steps(arm7, 11);
    check(machine.cpu_state(arm7).mode == CpuMode::system && machine.cpu_state(arm7).registers[2] == 0x12345679 &&
          !machine.memory().irq_pending(arm7) && machine.memory().irq_pending(arm9), "réponse ARM7 ou retour IRQ");
    machine.step(arm9);
    machine.run_steps(arm9, 8);
    for (const auto cpu : {arm9, arm7}) {
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[2] == 0x12345679 &&
              machine.cpu_state(cpu).registers[3] == 1 && !machine.memory().irq_pending(cpu) &&
              machine.memory().read16(cpu, control) == 0x8501, "échange IPC ou acquittement incomplet");
        machine.step(cpu);
        check(machine.cpu_state(cpu).registers[4] == 1, "reprise du programme après échange IPC absente");
    }
    machine.memory().write32(arm9, send, 123);
    auto copy = machine;
    check(copy.memory().read32(arm7, receive) == 123 && machine.memory().peek32(arm7, receive) == 123 &&
          !(machine.memory().read16(arm7, control) & 0x100U), "copie de machine partage sa FIFO");
    auto invalid = rom;
    put32(invalid, 0x34, 0x04000000); put32(invalid, 0x38, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(ravenemu::nds::Cartridge{invalid}); },
                                         "chargement invalide accepté");
    check(machine.memory().peek32(arm7, receive) == 123 && machine.memory().irq_pending(arm7), "chargement refusé efface IPC");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7})
        check(machine.memory().peek32(cpu, receive) == 0 && machine.memory().read32(cpu, control) == 0x0101 &&
              !machine.memory().irq_pending(cpu), "chargement conserve l'ancien IPC");
}

} // namespace

int main() {
    for (const auto cpu : {arm9, arm7}) {
        sync_test(cpu);
        fifo_order_test(cpu);
        disabled_and_clear_test(cpu);
        irq_edges_test(cpu);
        inspection_and_fault_test(cpu);
        cpu_transfer_test(cpu);
    }
    reset_and_copy_test();
    machine_exchange_test();
}
