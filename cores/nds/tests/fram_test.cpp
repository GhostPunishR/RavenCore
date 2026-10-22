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
constexpr std::uint32_t aux = 0x040001a0, data = aux + 2;
constexpr std::array types{BackupType::fram_8k,BackupType::fram_32k};

void prepare(Memory& memory, BackupType type) {
    memory.load_gamecard(Cartridge{cartridge_image()}); memory.load_backup(type);
}
std::uint8_t byte(Memory& memory, std::uint8_t output, bool last = false, Processor cpu = arm9) {
    memory.write16(cpu,aux,last ? 0xe000 : 0xe040);
    memory.write8(cpu,data,output); memory.advance_cycles(64);
    return memory.read8(cpu,data);
}
void command(Memory& memory, std::uint8_t value, Processor cpu = arm9) {
    static_cast<void>(byte(memory,value,true,cpu));
}
void address(Memory& memory, std::uint8_t cmd, unsigned location, Processor cpu = arm9) {
    static_cast<void>(byte(memory,cmd,false,cpu));
    static_cast<void>(byte(memory,static_cast<std::uint8_t>(location >> 8U),false,cpu));
    static_cast<void>(byte(memory,static_cast<std::uint8_t>(location),false,cpu));
}
std::uint8_t status(Memory& memory) {
    static_cast<void>(byte(memory,5)); return byte(memory,0,true);
}
std::uint8_t read(Memory& memory, unsigned location) {
    address(memory,3,location); return byte(memory,0,true);
}
void write(Memory& memory, unsigned location, std::span<const std::uint8_t> payload) {
    address(memory,2,location);
    for (unsigned i = 0; i < payload.size(); ++i) static_cast<void>(byte(memory,payload[i],i + 1 == payload.size()));
}
void write_status(Memory& memory, std::uint8_t value) {
    static_cast<void>(byte(memory,1)); static_cast<void>(byte(memory,value,true));
}

void profiles_and_streams() {
    for (unsigned i = 0; i < types.size(); ++i) {
        Memory memory; prepare(memory,types[i]);
        const auto capacity = i == 0 ? 8192U : 32768U;
        check(memory.backup().type() == types[i] && memory.backup().bytes().size() == capacity &&
              !memory.backup_dirty() && !memory.backup().busy() && status(memory) == 0, "état FRAM initial");
        static_cast<void>(byte(memory,0x9f));
        check(byte(memory,0) == 0xff && byte(memory,0,true) == 0xff, "FRAM générique invente un identifiant JEDEC");
        const std::array<std::uint8_t,4> values{0x12,0x34,0x56,0x78};
        write(memory,31,values);
        check(read(memory,31) == 0xff && !memory.backup_dirty(), "FRAM écrit sans WREN");
        command(memory,6); check(status(memory) == 2, "WREN FRAM");
        command(memory,4); check(status(memory) == 0, "WRDI FRAM");
        command(memory,6); write(memory,31,values);
        check(!memory.backup().busy() && status(memory) == 0 && memory.backup_dirty(), "FRAM produit un WIP ou garde WEL");
        address(memory,3,31);
        for (unsigned j = 0; j < values.size(); ++j)
            check(byte(memory,0,j + 1 == values.size()) == values[j], "FRAM reboucle dans une page EEPROM");
        const auto saved = memory.snapshot_backup(); memory.acknowledge_backup_saved(saved.generation);
        command(memory,6); write(memory,31,values);
        check(!memory.backup_dirty() && memory.snapshot_backup().generation == saved.generation, "écriture FRAM identique marque dirty");
        // Une transaction dépasse largement la plus grande page EEPROM.
        command(memory,6); address(memory,2,capacity - 130);
        for (unsigned j = 0; j < 300; ++j) static_cast<void>(byte(memory,static_cast<std::uint8_t>(j ^ 0xa5U),j == 299));
        for (unsigned j = 0; j < 300; ++j)
            check(read(memory,(capacity - 130 + j) % capacity) == static_cast<std::uint8_t>(j ^ 0xa5U), "flux FRAM limité ou miroir de capacité");
        check(read(memory,2 * capacity - 1) == static_cast<std::uint8_t>(129 ^ 0xa5U), "adresse FRAM 16 bits non miroîtée");
        address(memory,3,capacity - 1);
        check(byte(memory,0) == static_cast<std::uint8_t>(129 ^ 0xa5U) &&
              byte(memory,0,true) == static_cast<std::uint8_t>(130 ^ 0xa5U), "lecture FRAM ne reboucle pas à la capacité");
        check(memory.peek32(arm9,0x04000214) == 0 && !memory.dma_pending(arm9), "FRAM produit IRQ19/DRQ");
    }
}

void byte_boundaries_and_abort() {
    for (const auto type : types) {
        Memory memory; prepare(memory,type);
        const auto baseline = memory.snapshot_backup();
        command(memory,6); address(memory,2,0x123);
        memory.write8(arm9,data,0x42);
        expect_failure<ravenemu::Error>([&] { memory.load_backup(type); }, "remplacement FRAM pendant un octet accepté");
        memory.advance_cycles(63);
        check(memory.backup().bytes()[0x123] == 0xff && !memory.backup_dirty(), "FRAM modifiée au strobe ou avant la fin de l'octet");
        memory.write8(arm9,data,0x99); // strobe pendant busy : ne remplace pas 42
        memory.advance_cycles(1);
        const auto first = memory.snapshot_backup();
        check(first.data[0x123] == 0x42 && memory.backup_dirty() && !memory.backup().busy() &&
              memory.read16(arm9,aux) == 0xe040, "FRAM attend CS haut ou une période WIP");
        expect_failure<ravenemu::Error>([&] { memory.load_backup(type); }, "remplacement d'une FRAM sélectionnée accepté");
        memory.acknowledge_backup_saved(first.generation); check(!memory.backup_dirty(), "snapshot FRAM sous CS ne s'acquitte pas");
        auto copy = memory;
        memory.write8(arm9,data,0x56); memory.advance_cycles(64);
        memory.acknowledge_backup_saved(first.generation); check(memory.backup_dirty(), "acquittement FRAM ancien nettoie l'octet suivant");
        check(memory.backup().bytes()[0x124] == 0x56 && copy.backup().bytes()[0x124] == 0xff, "copies FRAM partagent leur image");
        memory.write8(arm9,data,0x78); memory.advance_cycles(63); memory.write16(arm9,aux,0);
        memory.advance_cycles(std::numeric_limits<std::uint64_t>::max());
        check(read(memory,0x123) == 0x42 && read(memory,0x124) == 0x56 && read(memory,0x125) == 0xff && status(memory) == 0,
              "annulation d'un octet retire les écritures FRAM terminées ou applique l'octet tronqué");
        copy.write8(arm9,data,0x99); copy.advance_cycles(32); copy.clear_ram(); copy.advance_cycles(32);
        check(read(copy,0x123) == 0x42 && read(copy,0x124) == 0xff && status(copy) == 0,
              "reset FRAM perd un octet validé ou termine un octet tronqué");
        command(memory,6); address(memory,2,0x123); static_cast<void>(byte(memory,0xaa));
        memory.clear_ram();
        check(read(memory,0x123) == 0xaa && status(memory) == 0, "reset abandonne une écriture FRAM déjà terminée");
        // Ni l'horloge sans strobe ni un ancien acquittement ne rejouent des données.
        const auto stable = memory.snapshot_backup(); memory.acknowledge_backup_saved(stable.generation);
        memory.advance_cycles(10000); memory.acknowledge_backup_saved(baseline.generation);
        check(!memory.backup_dirty() && memory.snapshot_backup().generation == stable.generation, "octet FRAM rejoué sans strobe");
    }
}

void protection_and_persistence() {
    for (const auto type : types) {
        Machine machine; machine.load_cartridge(Cartridge{cartridge_image()}); machine.load_backup(type);
        auto& memory = machine.memory();
        const auto size = static_cast<unsigned>(memory.backup().bytes().size());
        for (const auto protection : {1U,2U,3U,0U}) {
            const auto old_status = status(memory);
            write_status(memory,0x8c); check(status(memory) == old_status, "WRSR FRAM sans WREN accepté");
            command(memory,6); static_cast<void>(byte(memory,1));
            memory.write8(arm9,data,static_cast<std::uint8_t>(0xf3U | (protection << 2U)));
            memory.advance_cycles(63);
            check(memory.snapshot_backup().status == (old_status & 0x8cU), "WRSR FRAM modifie l'état avant la fin de l'octet");
            memory.advance_cycles(1);
            check(memory.snapshot_backup().status == (0x80U | (protection << 2U)) && !memory.backup().busy(), "WRSR FRAM retardé ou mal masqué");
            memory.write8(arm9,data,0); memory.advance_cycles(32); // octet superflu incomplet après WRSR terminé
            memory.write16(arm9,aux,0); // CS haut efface WEL ; /W haut ne verrouille pas WRSR
            check(status(memory) == (0x80U | (protection << 2U)), "WRSR conserve WEL");
            const auto boundary = protection == 1 ? size * 3 / 4 : (protection == 2 ? size / 2 : 0U);
            const auto old = memory.backup().bytes()[boundary];
            const std::array<std::uint8_t,1> changed{static_cast<std::uint8_t>(old ^ 0xffU)};
            command(memory,6); write(memory,boundary,changed);
            check(read(memory,boundary) == (protection ? old : changed[0]), "protection de la FRAM incorrecte");
            if (boundary) {
                const auto high = memory.backup().bytes()[boundary];
                command(memory,6); write(memory,boundary - 1,std::array<std::uint8_t,2>{0x5a,0xa5});
                check(read(memory,boundary - 1) == 0x5a && read(memory,boundary) == high,
                      "FRAM ne contrôle pas la protection à chaque octet du flux");
            }
            const auto saved = machine.snapshot_backup();
            check(saved.data.size() == size && saved.status == (0x80U | (protection << 2U)), "snapshot FRAM ajoute un suffixe");
            Machine restored; restored.load_cartridge(Cartridge{cartridge_image()}); restored.load_backup(type,saved.data,saved.status);
            check(restored.snapshot_backup().data == saved.data && status(restored.memory()) == status(memory) &&
                  !restored.backup_dirty(), "rechargement FRAM perd les données ou protections");
            machine.acknowledge_backup_saved(saved.generation); check(!machine.backup_dirty(), "API Machine acquittement FRAM");
            memory.clear_ram(); check(status(memory) == saved.status, "reset perd l'état non volatil FRAM");
        }
        machine.load_cartridge(Cartridge{cartridge_image()});
        check(memory.backup().type() == BackupType::none && !machine.backup_dirty(), "nouvelle cartouche hérite de la FRAM");
    }
}

void transport_and_validation() {
    Memory memory; prepare(memory,BackupType::fram_32k);
    for (const auto invalid : {0x0a,0x0b,0xdb,0xff}) {
        memory.write16(arm9,aux,0xe040);
        const auto before = memory.read32(arm9,aux);
        expect_failure<ravenemu::Error>([&] { memory.write8(arm9,data,static_cast<std::uint8_t>(invalid)); }, "commande non FRAM acceptée");
        check(memory.read32(arm9,aux) == before && !memory.backup_dirty(), "commande refusée modifie la FRAM");
    }
    const auto saved = memory.snapshot_backup();
    expect_failure<ravenemu::Error>([&] { memory.load_backup(BackupType::fram_8k,saved.data); }, "taille FRAM incompatible acceptée");
    expect_failure<ravenemu::Error>([&] { memory.load_backup(BackupType::fram_32k,{},0x40); }, "état FRAM réservé accepté");
    check(memory.snapshot_backup().generation == saved.generation, "import FRAM invalide change la génération");
    command(memory,6); static_cast<void>(byte(memory,2)); static_cast<void>(byte(memory,0x12,true));
    check(!memory.backup_dirty() && status(memory) == 2, "adresse FRAM incomplète écrit ou efface WEL");
    command(memory,4);
    command(memory,6); address(memory,2,0x50);
    const std::array<std::uint32_t,2> invalid_packet{0x005aa000,0x00008000};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm9,aux,invalid_packet); }, "STM avec semence KEY2 invalide accepté");
    check(memory.backup().bytes()[0x50] == 0xff && !(memory.read16(arm9,aux) & 0x80U), "prévalidation STM écrit dans la FRAM");
    static_cast<void>(byte(memory,0x5a,true));
    check(read(memory,0x50) == 0x5a, "STM refusé a avancé le curseur FRAM");
    for (unsigned baud = 0; baud < 4; ++baud) {
        command(memory,6); address(memory,2,0x100 + baud);
        memory.write16(arm9,aux,static_cast<std::uint16_t>(0xe000U | baud));
        memory.write8(arm9,data,0x5a); memory.advance_cycles((64U << baud) - 1U);
        check(memory.backup().bytes()[0x100 + baud] == 0xff && (memory.read16(arm9,aux) & 0x80U), "débit FRAM ignoré");
        memory.advance_cycles(1); check(read(memory,0x100 + baud) == 0x5a, "dernier cycle FRAM non appliqué");
    }
    // L'accès et le WEL sont communs aux deux CPU selon EXMEM, pas deux FRAM.
    memory.write16(arm9,0x04000204,0x800);
    check(memory.read32(arm9,aux) == 0, "FRAM visible au non-propriétaire");
    memory.write8(arm9,data,0x99);
    command(memory,6,arm7); address(memory,2,0x456,arm7); static_cast<void>(byte(memory,0x69,true,arm7));
    memory.write16(arm9,0x04000204,0);
    check(read(memory,0x456) == 0x69, "FRAM ARM7 non partagée avec ARM9");
}

void executable_programs() {
    for (const auto cpu : {arm9,arm7}) {
        Program program; program.load(0,aux);
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(6);
        program.load(2,0xa040); program.code.push_back(0xe1c020b0);
        for (const auto value : {2,0,0xff,0x12}) program.spi_byte(static_cast<std::uint8_t>(value));
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0x34);
        // Aucun polling WIP ni attente après le dernier octet SPI.
        program.load(2,0xa040); program.code.push_back(0xe1c020b0);
        for (const auto value : {3,1,0}) program.spi_byte(static_cast<std::uint8_t>(value));
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0,3);
        program.store(3,0x02001000);
        const auto code = program.finish();
        Machine machine;
        machine.load_cartridge(Cartridge{cpu == arm9 ? cartridge_image(code) : cartridge_image({0xeafffffe},code)});
        machine.load_backup(BackupType::fram_32k);
        if (cpu == arm7) machine.memory().write16(arm9,0x04000204,0x800);
        machine.run_cycles(2500);
        check(machine.memory().peek32(arm9,0x02001000) == 0x34 && machine.snapshot_backup().data[0xff] == 0x12 &&
              machine.snapshot_backup().data[0x100] == 0x34 && !machine.memory().backup().busy(),
              "programme ARM9/ARM7 ne relit pas immédiatement la FRAM après franchissement d'une page");
    }
}
}

int main() {
    profiles_and_streams(); byte_boundaries_and_abort(); protection_and_persistence();
    transport_and_validation(); executable_programs();
}
