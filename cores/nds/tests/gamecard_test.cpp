#include <ravenemu/nds/machine.hpp>
#include <ravenemu/nds/gamecard.hpp>
#include "check.hpp"
#include "support/program_fixture.hpp"

#include <algorithm>
#include <array>
#include <limits>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t aux = 0x040001a0, control = aux + 4, command_port = aux + 8;
constexpr std::uint32_t data = 0x04100010, exmem = 0x04000204, flags = 0x04000214;
constexpr std::uint32_t ready = 1U << 23U, busy = 1U << 31U, card_irq = 1U << 19U;

namespace {
std::vector<std::uint8_t> image() {
    auto bytes = cartridge_image(); bytes.resize(0x10000,0xff);
    put32(bytes,0,0x2153444e); put32(bytes,0x8123,0x44332211); put32(bytes,0x8127,0x88776655);
    put32(bytes,0x8ffc,0x04030201); put32(bytes,0x8000,0x08070605); put32(bytes,0x9000,0x0c0b0a09);
    return bytes;
}
void owner(Memory& memory, Processor cpu) { memory.write16(arm9,exmem,cpu == arm7 ? 0x800 : 0); }
void command(Memory& memory, Processor cpu, std::uint8_t opcode, std::uint32_t address = 0) {
    const std::array<std::uint8_t,8> bytes{opcode,static_cast<std::uint8_t>(address >> 24U),
        static_cast<std::uint8_t>(address >> 16U),static_cast<std::uint8_t>(address >> 8U),static_cast<std::uint8_t>(address),0,0,0};
    for (unsigned i = 0; i < bytes.size(); ++i) memory.write8(cpu,command_port + i,bytes[i]);
}
void begin(Memory& memory, Processor cpu, std::uint8_t opcode, std::uint32_t address = 0,
           unsigned size = 7, std::uint32_t extra = 0) {
    memory.write16(cpu,aux,0xc000); command(memory,cpu,opcode,address);
    memory.write32(cpu,control,0xa0000000U | (size << 24U) | extra);
}
std::uint32_t next(Memory& memory, Processor cpu, std::uint64_t cycles = 20) {
    memory.advance_cycles(cycles);
    check(memory.peek32(cpu,control) & ready, "donnée cartouche pas prête");
    return memory.read32(cpu,data);
}
std::uint32_t dma_control(Processor cpu, unsigned count = 1, bool repeat = true) {
    return 0x80000000U | 0x40000000U | 0x04000000U | 0x01000000U | (repeat ? 0x02000000U : 0U) |
           (cpu == arm9 ? 5U << 27U : 2U << 28U) | count;
}
void dma(Memory& memory, Processor cpu, std::uint32_t destination, std::uint32_t cnt,
          std::uint32_t source = data) {
    memory.write32(cpu,0x040000b8,0); memory.write32(cpu,0x040000b0,source);
    memory.write32(cpu,0x040000b4,destination); memory.write32(cpu,0x040000b8,cnt);
}

void registers_and_raw_test() {
    Memory memory;
    check(memory.read32(arm9,aux) == 0 && memory.read32(arm9,control) == 0 && memory.read16(arm9,exmem) == 0x6000 &&
          memory.read16(arm7,exmem) == 0x6000 && memory.read32(arm9,data) == 0, "état cartouche initial");
    begin(memory,arm9,0x90); check(next(memory,arm9,60) == 0xffffffff, "slot vide ne renvoie pas FF");
    memory.clear_ram();
    auto bytes = image(); memory.load_gamecard(Cartridge{bytes},Gamecard::Mode::raw);
    memory.write16(arm9,aux,0xc000); command(memory,arm9,0x90);
    expect_failure<ravenemu::Error>([&] { memory.write32(arm9,control,0x87000000); }, "transfert avec reset maintenu accepté");
    check(memory.peek32(arm9,control) == 0, "échec de démarrage a libéré reset");
    memory.write32(arm9,control,0xa7000000);
    check(next(memory,arm9,60) == 0x7c2 && !(memory.read32(arm9,control) & busy) &&
          memory.peek32(arm9,flags) == card_irq, "identifiant du modèle MROM 8 Mio ou IRQ finale");
    memory.write32(arm9,flags,card_irq); memory.write32(arm9,control,0);
    check(memory.read32(arm9,control) == 0x20000000, "RESB peut être effacé après libération");
    begin(memory,arm9,0,0,5);
    for (unsigned i = 0; i < 2048; ++i) {
        const auto value = next(memory,arm9,i == 0 ? 60 : 20);
        if (i == 0 || i == 1024) check(value == 0x2153444e, "en-tête RAW non répété à 4 Kio");
    }
    begin(memory,arm9,0x9f,0,1);
    for (unsigned i = 0; i < 128; ++i) check(next(memory,arm9,i == 0 ? 60 : 20) == 0xffffffff, "dummy 9F non HIGH-Z");
    command(memory,arm9,0x3c);
    const auto before = memory.read32(arm9,control);
    expect_failure<ravenemu::Error>([&] { memory.write32(arm9,control,0xa0000000); }, "KEY1 simulé comme un succès");
    check(memory.read32(arm9,control) == before, "commande KEY1 refusée modifie ROMCTRL");
    command(memory,arm9,0xb7,0x8000);
    expect_failure<ravenemu::Error>([&] { memory.write32(arm9,control,0xa7000000); }, "B7 accepté avant le mode principal");
    memory.write16(arm9,aux,0xffff);
    check(memory.read16(arm9,aux) == 0xe043, "masque AUXSPICNT");
    expect_failure<ravenemu::Error>([&] { memory.write8(arm9,aux + 2,0x03); }, "sauvegarde SPI silencieusement simulée");
    memory.write16(arm9,aux,0xc000);
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm9,aux + 0x10)); }, "semences KEY2 implicitement prises en charge");
}

void read_protocol_and_timing_test() {
    Memory memory; auto bytes = image(); memory.load_gamecard(Cartridge{bytes}); bytes[0x8123] = 0;
    begin(memory,arm9,0xb7,0x8123);
    memory.advance_cycles(59);
    check((memory.read32(arm9,control) & (busy | ready)) == busy && memory.read32(arm9,data) == 0,
          "premier mot prêt avant commande + données");
    memory.advance_cycles(1);
    check(memory.peek32(arm9,data) == 0x44332211 && memory.peek32(arm9,data) == 0x44332211 &&
          (memory.read32(arm9,control) & (busy | ready)) == (busy | ready), "peek consomme le mot ou dépend du buffer hôte");
    memory.advance_cycles(std::numeric_limits<std::uint64_t>::max());
    check(memory.peek32(arm9,data) == 0x44332211 && memory.peek32(arm9,flags) == 0, "donnée prête écrasée ou IRQ avant consommation");
    memory.write32(arm9,command_port,0xffffffff); memory.write32(arm9,control,0);
    check(memory.read8(arm9,command_port) == 0xb7 && (memory.read32(arm9,control) & ready), "écriture pendant busy modifie le transfert");
    check(memory.read32(arm9,data) == 0x44332211 && !(memory.read32(arm9,control) & (busy | ready)) &&
          memory.peek32(arm9,flags) == card_irq, "dernier mot ne termine pas le bloc");
    memory.write32(arm9,flags,card_irq);
    check(memory.read32(arm9,data) == 0x44332211 && memory.peek32(arm9,flags) == 0, "lecture prématurée répète une IRQ");
    begin(memory,arm9,0xb8); memory.write16(arm9,aux,0x8000);
    check(next(memory,arm9,60) == 0x7c2 && memory.peek32(arm9,flags) == 0, "AUXSPICNT IRQ désactivée ignorée");
    begin(memory,arm9,0xb7,0x8123,7,0x08000000U | 7U);
    memory.advance_cycles(151); check(!(memory.read32(arm9,control) & ready), "débit lent ou gap1 ignoré");
    check(next(memory,arm9,1) == 0x44332211, "débit lent : 19 clocks à huit cycles");

    for (const auto address : {0x123U,0x7123U,0x808123U}) {
        begin(memory,arm9,0xb7,address);
        check(next(memory,arm9,60) == 0x44332211, "zone basse ou capacité physique mal miroitées");
    }
    begin(memory,arm9,0xb7,0x8ffc,1);
    check(next(memory,arm9,60) == 0x04030201 && next(memory,arm9) == 0x08070605, "B7 ne boucle pas dans le bloc de 4 Kio");
    memory.write16(arm9,aux,0); memory.write32(arm9,flags,card_irq);
    begin(memory,arm9,0xb7,0x10000); check(next(memory,arm9,60) == 0xffffffff, "dump tronqué non rempli par FF");
    begin(memory,arm9,0xb8,0,1);
    for (unsigned i = 0; i < 128; ++i) check(next(memory,arm9,i == 0 ? 60 : 20) == 0x7c2, "ID non répété");
    begin(memory,arm9,0xb8,0,0);
    memory.advance_cycles(39); check(memory.read32(arm9,control) & busy, "bloc nul termine avant la commande");
    memory.advance_cycles(1); check(!(memory.read32(arm9,control) & (busy | ready)), "bloc nul produit DRQ ou reste bloqué");
    auto large = image(); large[0x14] = 12; memory.load_gamecard(Cartridge{large});
    begin(memory,arm9,0xb8); check(next(memory,arm9,60) == 0x8000fec2, "capacité 512 Mio déclarée ignorée");
    begin(memory,arm9,0xb7,0x20008123,7,0x00402000);
    check(next(memory,arm9,60) == 0x44332211, "mode post-boot KEY2 ou miroir 512 Mio");
}

void lengths_gaps_and_errors_test() {
    Memory memory; memory.load_gamecard(Cartridge{image()});
    for (unsigned size = 1; size <= 7; ++size) {
        begin(memory,arm9,0xb8,0,size);
        const auto words = size == 7 ? 1U : (0x100U << size) / 4U;
        for (unsigned i = 0; i < words; ++i) {
            check(next(memory,arm9,i == 0 ? 60 : 20) == 0x7c2, "taille du bloc incorrecte");
            check(((memory.read32(arm9,control) & busy) != 0) == (i + 1 != words), "busy termine avant/après le nombre demandé");
        }
    }
    begin(memory,arm9,0xb8,0,2,3U << 16U);
    for (unsigned i = 0; i < 128; ++i) static_cast<void>(next(memory,arm9,i == 0 ? 60 : 20));
    memory.advance_cycles(34); check(!(memory.read32(arm9,control) & ready), "gap2 à 512 octets ignoré");
    check(next(memory,arm9,1) == 0x7c2, "gap2 doit durer 3 clocks plus quatre octets");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(arm9,data)); }, "demi-mot CARD_DATA accepté");
    memory.advance_cycles(20); const auto latched = memory.peek32(arm9,data);
    const auto state = memory.peek32(arm9,control);
    std::array<std::uint32_t,2> result{0xdeadbeef,0xdeadbeef};
    expect_failure<ravenemu::Error>([&] { memory.read_words(arm9,data,result); }, "plage après CARD_DATA acceptée");
    check(memory.peek32(arm9,control) == state && result[0] == 0xdeadbeef, "lecture multiple fautive consomme une donnée");
    memory.write32(arm9,data,0); check(memory.peek32(arm9,data) == latched && (memory.read32(arm9,control) & ready), "écriture sur CARD_DATA consomme le mot");
    memory.write16(arm9,aux,0); memory.write32(arm9,flags,card_irq); memory.advance_cycles(1000);
    check(!(memory.read32(arm9,control) & (busy | ready)) && memory.peek32(arm9,flags) == 0, "annulation produit une IRQ différée");
    command(memory,arm9,0); // commande RAW interdite dans le mode principal
    const std::array<std::uint32_t,2> start{0xc000,0xa7000000};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm9,aux,start); }, "écriture combinée accepte une commande interdite");
    check(memory.read16(arm9,aux) == 0, "validation multiple a activé le slot avant une erreur");
    command(memory,arm9,0xb7,0x8123); memory.write16(arm9,aux,0xc000);
    for (const auto unsupported : {0xe7000000U,0xa7008000U,0x20008000U}) {
        const auto before = memory.peek32(arm9,control);
        expect_failure<ravenemu::Error>([&] { memory.write32(arm9,control,unsupported); }, "écriture NAND/semence KEY2 simulée");
        check(memory.peek32(arm9,control) == before, "contrôle refusé a été appliqué");
    }
}

void ownership_and_irq_test() {
    Memory memory; memory.load_gamecard(Cartridge{image()});
    memory.write16(arm9,exmem,0xffff); memory.write16(arm7,exmem,0x0012);
    check(memory.read16(arm9,exmem) == 0xe8ff && memory.read16(arm7,exmem) == 0xe892, "EXMEM : bits partagés/locaux et masques");
    memory.write16(arm7,exmem,0); check(memory.read16(arm9,exmem) == 0xe8ff && memory.read16(arm7,exmem) == 0xe880,
                                          "ARM7 modifie les droits partagés");
    memory.write32(arm9,control,0xa7000000); check(memory.read32(arm9,control) == 0, "CPU non propriétaire pilote le bus");
    begin(memory,arm7,0xb7,0x8123); memory.advance_cycles(60);
    check(memory.read32(arm9,data) == 0 && memory.peek32(arm9,control) == 0 && (memory.read32(arm7,control) & ready),
          "CPU non propriétaire consomme la donnée");
    const auto before = memory.read16(arm9,exmem);
    expect_failure<ravenemu::Error>([&] { memory.write32(arm9,exmem,0); }, "changement de propriétaire accepté en cours de bloc");
    check(memory.read16(arm9,exmem) == before, "changement de droits refusé modifie les bits locaux");
    check(memory.read32(arm7,data) == 0x44332211 && memory.peek32(arm7,flags) == card_irq &&
          memory.peek32(arm9,flags) == 0 && !memory.irq_pending(arm7), "fin de bloc ne cible pas son propriétaire");
    memory.write32(arm7,0x04000210,card_irq); memory.write32(arm7,0x04000208,1);
    check(memory.irq_pending(arm7), "IF cartouche perdu avant IME/IE");
    memory.write32(arm7,flags,card_irq); memory.advance_cycles(1000);
    check(!memory.irq_pending(arm7), "IRQ de fin répétée après acquittement");
    owner(memory,arm9); begin(memory,arm9,0xb8); check(next(memory,arm9,60) == 0x7c2, "transfert après réattribution idle impossible");
}

void dma_test() {
    for (const auto cpu : {arm9,arm7}) {
        Memory memory; memory.load_gamecard(Cartridge{image()}); owner(memory,cpu);
        dma(memory,cpu,0x02002000,dma_control(cpu)); begin(memory,cpu,0xb7,0x8123,1);
        memory.advance_cycles(59); check(!memory.dma_pending(cpu), "DMA avant DRQ");
        memory.advance_cycles(1);
        for (unsigned i = 0; i < 128; ++i) {
            check(memory.dma_pending(cpu) && memory.step_dma(cpu), "DMA non déclenché par un mot cartouche");
            if (i < 2) check(memory.peek32(cpu,0x02002000 + i * 4) == (i == 0 ? 0x44332211 : 0x88776655), "DMA inverse les octets ou perd sa destination");
            check(!memory.dma_pending(cpu) && !memory.step_dma(cpu), "DMA consomme plus d'un mot par DRQ");
            if (i != 127) { memory.advance_cycles(19); check(!memory.dma_pending(cpu), "mot suivant transféré trop tôt"); memory.advance_cycles(1); }
        }
        check((memory.peek32(cpu,flags) & (card_irq | 0x100U)) == (card_irq | 0x100U) &&
              memory.peek32(cpu == arm9 ? arm7 : arm9,flags) == 0 && (memory.read32(cpu,0x040000b8) & busy), "IRQ DMA/fin cartouche ou répétition");
        memory.write32(cpu,0x040000b8,0);
        dma(memory,cpu,0x02003000,dma_control(cpu,2,false)); begin(memory,cpu,0xb7,0x8123,1);
        memory.advance_cycles(60); memory.step_dma(cpu);
        check(!memory.dma_pending(cpu) && (memory.read32(cpu,0x040000b8) & busy), "DMA compteur 2 pas suspendu après un mot");
        memory.advance_cycles(20); memory.step_dma(cpu);
        check(!(memory.read32(cpu,0x040000b8) & busy) && memory.read32(cpu,0x02003004) == 0x88776655,
              "DMA non répété ne termine pas après son compteur");
    }
}

void dma_errors_and_tcm_test() {
    Memory memory; memory.load_gamecard(Cartridge{image()});
    begin(memory,arm9,0xb7,0x8123,1); memory.advance_cycles(60);
    dma(memory,arm9,0x04000060,dma_control(arm9));
    expect_failure<DmaTransferError>([&] { memory.step_dma(arm9); }, "destination DMA invalide acceptée");
    check(memory.dma_pending(arm9) && memory.peek32(arm9,data) == 0x44332211 && (memory.peek32(arm9,control) & ready),
          "échec DMA consomme la donnée ou son déclenchement");
    for (const auto cnt : {dma_control(arm9) & ~0x04000000U,dma_control(arm9) & ~0x01000000U}) {
        dma(memory,arm9,0x02002000,cnt);
        expect_failure<DmaTransferError>([&] { memory.step_dma(arm9); }, "DMA cartouche mal configuré accepté");
    }
    dma(memory,arm9,0x02002000,dma_control(arm9));
    check(memory.dma_pending(arm9), "armer DMA après DRQ perd le mot déjà prêt");
    memory.cp15().write(9,1,0,0x0410000a); memory.cp15().write(1,0,0,0x12078);
    memory.write32(arm9,data,0xcafebabe);
    check(memory.read32(arm9,data) == 0xcafebabe && (memory.read32(arm9,control) & ready), "lecture TCM touche le port cartouche");
    check(memory.step_dma(arm9) && memory.peek32(arm9,0x02002000) == 0x44332211 && !(memory.peek32(arm9,control) & ready),
          "DMA lit la TCM au lieu du port externe");
}

void copy_and_reload_test() {
    auto bytes = image(); Machine machine; machine.load_cartridge(Cartridge{bytes});
    begin(machine.memory(),arm9,0xb7,0x8123,1); machine.advance_cycles(17);
    auto copy = machine;
    machine.advance_cycles(43);
    check(!(copy.memory().peek32(arm9,control) & ready) && machine.memory().read32(arm9,data) == 0x44332211,
          "copie partage progression cartouche");
    copy.advance_cycles(43); check(copy.memory().read32(arm9,data) == 0x44332211, "copie perd le média ou le curseur");
    auto invalid = bytes; invalid[0x14] = 0xff;
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(Cartridge{invalid}); }, "capacité invalide acceptée au démarrage");
    check(next(machine.memory(),arm9) == 0x88776655, "chargement refusé perd le média courant ou le transfert");
    bytes[0x8123] = 0x99; machine.load_cartridge(Cartridge{bytes});
    check(machine.memory().read32(arm9,flags) == 0 && machine.memory().read32(arm9,control) == 0x20000000,
          "rechargement ne remet pas les registres cartouche à zéro");
    begin(machine.memory(),arm9,0xb7,0x8123); check(next(machine.memory(),arm9,60) == 0x44332299, "nouveau média non installé");
    check(next(copy.memory(),arm9) == 0x88776655, "copie utilise le média remplacé d'une autre machine");
    machine.memory().clear_ram();
    begin(machine.memory(),arm9,0xb7,0x8123); check(next(machine.memory(),arm9,60) == 0x44332299, "reset RAM détache le média");
}

void executable_test(bool use_dma, Processor cpu) {
    Program program;
    if (cpu == arm9) {
        program.load(0,0x04000304); program.load(1,0x8003); program.code.push_back(0xe1c010b0);
        program.load(0,0x04000000); program.load(1,0x10000); program.code.push_back(0xe5801000);
    }
    if (use_dma) {
        program.load(0,0x040000b0); program.load(1,data); program.code.push_back(0xe5801000);
        program.load(1,0x02002000); program.code.push_back(0xe5801004);
        program.load(1,dma_control(cpu)); program.code.push_back(0xe5801008);
    }
    program.load(0,aux); program.load(1,0xc000); program.code.push_back(0xe1c010b0);
    program.load(0,command_port); program.load(1,0x800000b7); program.code.push_back(0xe5801000);
    program.code.push_back(0xe3a01000); program.code.push_back(0xe5801004);
    program.load(0,control); program.load(1,use_dma ? 0xa1000000 : 0xa7000000); program.code.push_back(0xe5801000);
    // LDR / LSR / TST / Bcond : attendre busy=0 (DMA) ou DRQ=1 (CPU).
    program.code.insert(program.code.end(),{0xe5901000,use_dma ? 0xe1a01c21U : 0xe1a01821U,0xe3110080,
                                           use_dma ? 0x1afffffbU : 0x0afffffbU});
    program.load(0,use_dma ? 0x02002000 : data); program.code.push_back(0xe5902000);
    program.store(2,0x02001000);
    if (cpu == arm9) { program.load(0,0x05000000); program.code.push_back(0xe1c020b0); }
    const auto code = program.finish();
    auto rom = cpu == arm9 ? cartridge_image(code) : cartridge_image({0xeafffffe},code);
    rom.resize(0x10000,0xff); put32(rom,0x8000,0x7c00001f);
    Machine machine; machine.load_cartridge(Cartridge{rom}); owner(machine.memory(),cpu); machine.run_cycles(5000);
    check(machine.memory().peek32(arm9,0x02001000) == 0x7c00001f && (cpu == arm7 || machine.render_frame().upper[0] == 0x1f) &&
          (machine.memory().peek32(cpu,flags) & card_irq) && !(machine.memory().peek32(cpu,control) & busy) &&
          machine.memory().peek32(cpu,command_port) == 0x800000b7,
          "programme CPU/DMA ne lit pas sa ressource cartouche ou n'en utilise pas le résultat");
}
} // namespace

int main() {
    registers_and_raw_test();
    read_protocol_and_timing_test();
    lengths_gaps_and_errors_test();
    ownership_and_irq_test();
    dma_test();
    dma_errors_and_tcm_test();
    copy_and_reload_test();
    for (const auto cpu : {arm9,arm7}) { executable_test(false,cpu); executable_test(true,cpu); }
}
