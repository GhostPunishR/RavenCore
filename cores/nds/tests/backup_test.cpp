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
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t aux = 0x040001a0, exmem = 0x04000204;
constexpr std::uint32_t write_cycles = 167570;

void prepare(Memory& memory, BackupType type) {
    memory.load_gamecard(Cartridge{cartridge_image()});
    memory.load_backup(type);
}
std::uint8_t byte(Memory& memory, std::uint8_t output, bool last = false, Processor cpu = arm9, unsigned baud = 0) {
    memory.write16(cpu,aux,static_cast<std::uint16_t>(0xe000U | (last ? 0U : 0x40U) | baud));
    memory.write8(cpu,aux + 2,output);
    memory.advance_cycles(64U << baud);
    return memory.read8(cpu,aux + 2);
}
void packet(Memory& memory, std::initializer_list<std::uint8_t> bytes, Processor cpu = arm9) {
    unsigned i = 0;
    for (const auto value : bytes) static_cast<void>(byte(memory,value,++i == bytes.size(),cpu));
}
std::uint8_t status(Memory& memory, Processor cpu = arm9) {
    static_cast<void>(byte(memory,0x05,false,cpu));
    return byte(memory,0,true,cpu);
}
void address(Memory& memory, std::uint8_t command, unsigned location, Processor cpu = arm9) {
    const auto small = memory.backup().type() == BackupType::eeprom_512;
    static_cast<void>(byte(memory,static_cast<std::uint8_t>(command | (small && location & 0x100U ? 8U : 0U)),false,cpu));
    if (!small) static_cast<void>(byte(memory,static_cast<std::uint8_t>(location >> 8U),false,cpu));
    static_cast<void>(byte(memory,static_cast<std::uint8_t>(location),false,cpu));
}
void write(Memory& memory, unsigned location, std::span<const std::uint8_t> bytes, Processor cpu = arm9) {
    address(memory,2,location,cpu);
    for (unsigned i = 0; i < bytes.size(); ++i) static_cast<void>(byte(memory,bytes[i],i + 1 == bytes.size(),cpu));
}
std::uint8_t read(Memory& memory, unsigned location, Processor cpu = arm9) {
    address(memory,3,location,cpu);
    return byte(memory,0,true,cpu);
}

void profiles_and_pages() {
    for (const auto type : {BackupType::eeprom_512,BackupType::eeprom_8k,BackupType::eeprom_64k}) {
        Memory memory; prepare(memory,type);
        const auto size = type == BackupType::eeprom_512 ? 512U : (type == BackupType::eeprom_8k ? 8192U : 65536U);
        const auto page = type == BackupType::eeprom_512 ? 16U : (type == BackupType::eeprom_8k ? 32U : 128U);
        const auto initial_status = type == BackupType::eeprom_512 ? 0xf0U : 0U;
        check(memory.backup().bytes().size() == size && status(memory) == initial_status && !memory.backup_dirty(),
              "profil EEPROM initial");
        packet(memory,{0x9f});
        check(byte(memory,0x9f) == 0xff && byte(memory,0,true) == 0xff, "EEPROM invente un identifiant JEDEC");
        const std::array<std::uint8_t,4> values{0x12,0x34,0x56,0x78};
        write(memory,size - 2,values);
        memory.advance_cycles(write_cycles);
        check(read(memory,size - 2) == 0xff && !memory.backup_dirty(), "écriture sans WREN acceptée");
        packet(memory,{6}); check(status(memory) == (initial_status | 2U), "WREN sans WEL");
        packet(memory,{4}); check(status(memory) == initial_status, "WRDI sans effet");
        packet(memory,{6}); write(memory,size - 2,values);
        check(memory.backup().busy() && status(memory) == (initial_status | 3U) && !memory.backup_dirty(),
              "cycle interne ou WEL/WIP EEPROM");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.snapshot_backup()); }, "snapshot pendant WIP accepté");
        packet(memory,{4}); // ignoré pendant WIP
        check(status(memory) == (initial_status | 3U), "WRDI accepté pendant WIP");
        memory.advance_cycles(write_cycles);
        check(!memory.backup().busy() && status(memory) == initial_status && memory.backup_dirty(), "écriture non terminée");
        check(read(memory,size - 2) == 0x12 && read(memory,size - 1) == 0x34 &&
              read(memory,size - page) == 0x56 && read(memory,size - page + 1) == 0x78 && read(memory,0) == 0xff,
              "écriture ne boucle pas dans sa page ou adresse haute perdue");
        address(memory,3,size - 1);
        check(byte(memory,0) == 0x34 && byte(memory,0,true) == 0xff, "lecture ne boucle pas à la capacité");
        if (type != BackupType::eeprom_512) check(read(memory,2 * size - 2) == 0x12, "miroir d'adresse EEPROM");
        // Plus d'une page : les derniers octets gagnent sur les mêmes cellules.
        std::vector<std::uint8_t> overlong(page + 2,0xa5); overlong[page] = 0x5a; overlong[page + 1] = 0xc3;
        packet(memory,{6}); write(memory,0,overlong); memory.advance_cycles(write_cycles);
        check(read(memory,0) == 0x5a && read(memory,1) == 0xc3 && read(memory,page - 1) == 0xa5 &&
              read(memory,page) == 0xff, "rebouclage d'une écriture de plus d'une page");
    }
}

void transport_and_validation() {
    Memory memory;
    expect_failure<ravenemu::Error>([&] { memory.load_backup(BackupType::eeprom_8k); }, "EEPROM sans cartouche acceptée");
    prepare(memory,BackupType::none);
    check(status(memory) == 0xff && read(memory,0) == 0xff && !memory.backup_dirty(), "bus de sauvegarde absent");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.snapshot_backup()); }, "snapshot sans EEPROM accepté");
    memory.load_backup(BackupType::eeprom_8k);
    for (unsigned baud = 0; baud < 4; ++baud) {
        memory.write16(arm9,aux,static_cast<std::uint16_t>(0xe040U | baud));
        memory.write16(arm9,aux + 2,0xff05);
        check(memory.read16(arm9,aux) == (0xe0c0U | baud) && memory.read8(arm9,aux + 3) == 0, "busy AUXSPI ou octet haut DATA");
        expect_failure<ravenemu::Error>([&] { memory.write16(arm9,exmem,0x800); }, "propriétaire changé pendant un octet");
        memory.write8(arm7,aux + 2,0xaa);
        memory.write8(arm9,aux + 2,0x99); // strobe pendant busy ignoré
        memory.advance_cycles((64U << baud) - 1U);
        check(memory.read16(arm9,aux) & 0x80U, "fin AUXSPI prématurée");
        memory.write16(arm9,aux,0xe043); // baud/hold figés pendant l'octet
        memory.advance_cycles(1);
        check(memory.read16(arm9,aux) == (0xe040U | baud), "configuration changée pendant busy");
        check(byte(memory,0,true,arm9,baud) == 0, "RDSR altéré par un strobe ignoré");
        check(memory.peek32(arm9,0x04000214) == 0 && !memory.dma_pending(arm9), "AUXSPI a levé IRQ19 ou DRQ");
    }
    memory.write16(arm9,aux,0xe040);
    const auto before = memory.snapshot_backup();
    expect_failure<ravenemu::Error>([&] { memory.write8(arm9,aux + 2,0xab); }, "commande inconnue acceptée");
    check(memory.read16(arm9,aux) == 0xe040 && memory.snapshot_backup().generation == before.generation,
          "diagnostic modifie le périphérique");
    // Un STM qui échoue sur le registre suivant ne doit pas envoyer WREN.
    const std::array<std::uint32_t,2> invalid_packet{0x0006a000,0x00008000};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm9,aux,invalid_packet); }, "STM KEY2 invalide accepté");
    check(memory.read16(arm9,aux) == 0xe040 && status(memory) == 0, "STM invalide démarre AUXSPI");
    memory.write16(arm9,exmem,0x800);
    check(memory.read32(arm9,aux) == 0, "registres AUXSPI visibles au non-propriétaire");
    packet(memory,{6},arm7); check(status(memory,arm7) == 2, "AUXSPI ARM7 ou WREN");
    packet(memory,{4},arm7);
    memory.write16(arm9,exmem,0);
    // Le registre 32 bits peut configurer le bus et déclencher son premier octet.
    memory.write32(arm9,aux,0x0006a000); memory.advance_cycles(64);
    check(status(memory) == 2, "strobe combiné AUXSPICNT/DATA");
    memory.write16(arm9,aux,0x8000);
    memory.write8(arm9,aux + 2,4); memory.advance_cycles(1000);
    check(status(memory) == 2, "strobe en mode ROM touche à l'EEPROM");
}

void persistence_and_reset() {
    Machine machine; machine.load_cartridge(Cartridge{cartridge_image()}); machine.load_backup(BackupType::eeprom_512);
    auto& memory = machine.memory();
    const auto before = machine.snapshot_backup();
    packet(memory,{6}); packet(memory,{0x0a,0x23,0x42});
    memory.advance_cycles(write_cycles - 1);
    check(memory.backup().busy() && memory.backup().bytes()[0x123] == 0xff, "écriture EEPROM trop tôt visible");
    memory.clear_ram(); // le cycle lancé reste autonome, les registres série sont remis à zéro
    memory.advance_cycles(1);
    check(read(memory,0x123) == 0x42 && machine.backup_dirty(), "reset abandonne une écriture interne acceptée");
    const auto saved = machine.snapshot_backup();
    check(saved.data.size() == 512 && saved.status == 0, "snapshot ajoute un suffixe au .sav");
    machine.acknowledge_backup_saved(before.generation);
    check(machine.backup_dirty(), "ancien acquittement nettoie les nouveaux octets");
    machine.acknowledge_backup_saved(saved.generation); check(!machine.backup_dirty(), "acquittement courant refusé");
    packet(memory,{6}); packet(memory,{0x0a,0x23,0x42}); memory.advance_cycles(write_cycles);
    check(!machine.backup_dirty() && machine.snapshot_backup().generation == saved.generation, "écriture identique marque dirty");
    Machine restored; restored.load_cartridge(Cartridge{cartridge_image()}); restored.load_backup(BackupType::eeprom_512,saved.data,saved.status);
    check(read(restored.memory(),0x123) == 0x42 && !restored.backup_dirty(), "rechargement EEPROM");
    Memory independent = memory;
    packet(independent,{6}); packet(independent,{0x0a,0x23,0x99}); independent.advance_cycles(write_cycles);
    check(read(memory,0x123) == 0x42 && read(independent,0x123) == 0x99, "copie de machine partage les sauvegardes mutables");
    // CS interrompu pendant le dernier octet : aucun paquet partiel ne se termine.
    packet(memory,{6}); address(memory,2,0x123);
    memory.write16(arm9,aux,0xa000); memory.write8(arm9,aux + 2,0x99); memory.advance_cycles(63);
    memory.write16(arm9,aux,0); memory.advance_cycles(write_cycles);
    check(read(memory,0x123) == 0x42 && !memory.backup().busy(), "désactivation valide un octet incomplet");
    packet(memory,{6}); address(memory,2,0x123); static_cast<void>(byte(memory,0x99));
    memory.clear_ram(); memory.advance_cycles(write_cycles);
    check(read(memory,0x123) == 0x42 && status(memory) == 0xf0, "reset termine un paquet sans CS haut");
    // Aucune modification si l'image ou son profil est invalide.
    std::array<std::uint8_t,17> bad{};
    expect_failure<ravenemu::Error>([&] { machine.load_backup(BackupType::eeprom_512,bad); }, "taille de .sav invalide acceptée");
    expect_failure<ravenemu::Error>([&] { machine.load_backup(static_cast<BackupType>(999)); }, "profil inconnu accepté");
    expect_failure<ravenemu::Error>([&] { machine.load_backup(BackupType::eeprom_512,{},0x80); }, "état non volatil invalide accepté");
    check(machine.snapshot_backup().data == saved.data, "chargement invalide détruit la sauvegarde");
    machine.load_cartridge(Cartridge{cartridge_image()});
    check(memory.backup().type() == BackupType::none && !machine.backup_dirty(), "nouvelle cartouche hérite d'un autre .sav");
    machine.load_backup(BackupType::eeprom_512,saved.data);
    packet(memory,{6}); packet(memory,{2,0x23,0x55}); memory.advance_cycles(write_cycles);
    machine.acknowledge_backup_saved(saved.generation);
    check(machine.backup_dirty(), "ancien acquittement accepté après remplacement du média");
}

void selection_and_write_boundaries() {
    Memory memory; prepare(memory,BackupType::eeprom_8k);
    packet(memory,{6}); address(memory,2,0x123);
    static_cast<void>(byte(memory,0x5a)); // CS reste bas, pas d'opération interne
    check(!memory.backup().busy() && !memory.backup_dirty(), "écriture démarre avant CS haut");
    expect_failure<ravenemu::Error>([&] { memory.load_backup(BackupType::eeprom_8k); }, "remplacement au milieu du paquet accepté");
    memory.write16(arm9,aux,0x8000); // terminer le paquet puis passer en mode ROM
    check(memory.backup().busy(), "passage en ROM perd un paquet complet");
    expect_failure<ravenemu::Error>([&] { memory.load_backup(BackupType::eeprom_8k); }, "remplacement pendant WIP accepté");
    memory.advance_cycles(write_cycles - 32);
    // WREN commencé pendant WIP reste ignoré, même si le cycle finit avant CS.
    packet(memory,{6});
    check(status(memory) == 0 && read(memory,0x123) == 0x5a, "commande ignorée réappliquée à la fin de WIP");
    packet(memory,{6}); address(memory,2,0x124);
    memory.write16(arm9,aux,0xa000); memory.write8(arm9,aux + 2,0xc3);
    expect_failure<ravenemu::Error>([&] { memory.load_backup(BackupType::eeprom_8k); }, "remplacement pendant octet accepté");
    // Une avance unique doit finir l'octet, lancer puis terminer l'écriture.
    memory.advance_cycles(std::numeric_limits<std::uint64_t>::max());
    check(read(memory,0x124) == 0xc3 && status(memory) == 0, "excès de cycles après CS non appliqué");
    const auto saved = memory.snapshot_backup();
    packet(memory,{1,0x0c}); memory.advance_cycles(write_cycles); // WRSR sans WREN
    check(status(memory) == 0 && memory.snapshot_backup().generation == saved.generation, "WRSR sans WREN accepté");
    memory.acknowledge_backup_saved(saved.generation);
    packet(memory,{6}); packet(memory,{1,0x0c}); memory.advance_cycles(write_cycles);
    check(memory.backup_dirty() && memory.snapshot_backup().data == saved.data && memory.snapshot_backup().status == 0x0c,
          "modification du seul état non volatil non marquée dirty");
    memory.clear_ram(); check(status(memory) == 0x0c, "reset perd les protections non volatiles");
}

void protection() {
    for (const auto type : {BackupType::eeprom_512,BackupType::eeprom_8k,BackupType::eeprom_64k}) {
        Memory memory; prepare(memory,type);
        const auto size = static_cast<unsigned>(memory.backup().bytes().size());
        for (const auto protection : {1U,2U,3U,0U}) {
            packet(memory,{6}); packet(memory,{1,static_cast<std::uint8_t>(0xf3U | (protection << 2U))});
            memory.advance_cycles(write_cycles);
            const auto expected = (type == BackupType::eeprom_512 ? 0xf0U : 0x80U) | (protection << 2U);
            check(status(memory) == expected, "masque WRSR ou SRWD avec /W haut");
            const auto start = protection == 1 ? size * 3 / 4 : (protection == 2 ? size / 2 : 0U);
            const auto previous = memory.backup().bytes()[start];
            const std::array<std::uint8_t,1> changed{static_cast<std::uint8_t>(previous ^ 0xffU)};
            packet(memory,{6}); write(memory,start,changed); memory.advance_cycles(write_cycles);
            check(read(memory,start) == (protection ? previous : changed[0]), "protection EEPROM incorrecte");
            if (start) {
                packet(memory,{6}); write(memory,start - 1,changed); memory.advance_cycles(write_cycles);
                check(read(memory,start - 1) == changed[0], "zone non protégée refusée");
            }
            const auto saved = memory.snapshot_backup();
            Memory restored; restored.load_gamecard(Cartridge{cartridge_image()}); restored.load_backup(type,saved.data,saved.status);
            check(status(restored) == expected && !restored.backup_dirty(), "état non volatil EEPROM non restauré");
        }
    }
}

void cpu_programs() {
    for (const auto cpu : {arm9,arm7}) {
        Program program;
        program.load(0,aux); program.load(2,0xa000); program.code.push_back(0xe1c020b0); // strh r2,[r0]
        program.spi_byte(6);
        program.load(2,0xa040); program.code.push_back(0xe1c020b0);
        program.spi_byte(0x0a); program.spi_byte(0x23);
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0x5a);
        // Attendre WIP par RDSR, avec CS haut entre interrogations.
        const auto poll = static_cast<unsigned>(program.code.size());
        program.load(2,0xa040); program.code.push_back(0xe1c020b0); program.spi_byte(5);
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0,3);
        program.code.push_back(0xe3130001); // tst r3,#1
        const auto branch = static_cast<unsigned>(program.code.size());
        program.code.push_back(0x1a000000U | ((poll - branch - 2U) & 0xffffffU));
        program.load(2,0xa040); program.code.push_back(0xe1c020b0); program.spi_byte(0x0b); program.spi_byte(0x23);
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0,3);
        program.store(3,0x02001000);
        const auto code = program.finish();
        Machine machine;
        machine.load_cartridge(Cartridge{cpu == arm9 ? cartridge_image(code) : cartridge_image({0xeafffffe},code)});
        machine.load_backup(BackupType::eeprom_512);
        if (cpu == arm7) machine.memory().write16(arm9,exmem,0x800);
        machine.run_cycles(write_cycles + 12000);
        check(machine.memory().read32(arm9,0x02001000) == 0x5a && machine.snapshot_backup().data[0x123] == 0x5a,
              "programme ARM9/ARM7 ne sauvegarde pas puis relit par AUXSPI");
    }
}
}

int main() {
    profiles_and_pages(); transport_and_validation(); persistence_and_reset(); selection_and_write_boundaries();
    protection(); cpu_programs();
}
