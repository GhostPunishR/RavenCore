#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/firmware_fixture.hpp"

#include <algorithm>
#include <array>
#include <limits>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t aux = 0x040001a0, data = aux + 2;
constexpr std::uint32_t program_cycles = 40217, write_cycles = 368654, erase_cycles = 335140;
constexpr std::uint32_t sector_cycles = timer_clock_hz;
constexpr std::array types{BackupType::flash_256k,BackupType::flash_512k,BackupType::flash_1m};

void prepare(Memory& memory, BackupType type, std::span<const std::uint8_t> bytes = {}) {
    memory.load_gamecard(Cartridge{cartridge_image()}); memory.load_backup(type,bytes);
}
std::uint8_t byte(Memory& memory, std::uint8_t output, bool last = false, Processor cpu = arm9) {
    memory.write16(cpu,aux,last ? 0xe000 : 0xe040);
    memory.write8(cpu,data,output); memory.advance_cycles(64);
    return memory.read8(cpu,data);
}
void command(Memory& memory, std::uint8_t output, Processor cpu = arm9) {
    static_cast<void>(byte(memory,output,true,cpu));
}
void address(Memory& memory, std::uint8_t cmd, std::uint32_t location, Processor cpu = arm9) {
    static_cast<void>(byte(memory,cmd,false,cpu));
    for (const auto shift : {16U,8U,0U}) static_cast<void>(byte(memory,static_cast<std::uint8_t>(location >> shift),false,cpu));
}
void packet(Memory& memory, std::uint8_t cmd, std::uint32_t location, std::span<const std::uint8_t> payload = {}) {
    address(memory,cmd,location);
    for (const auto value : payload) static_cast<void>(byte(memory,value));
    memory.write16(arm9,aux,0); // terminer la transaction après le dernier octet complet
}
std::uint8_t status(Memory& memory) {
    static_cast<void>(byte(memory,5)); return byte(memory,0,true);
}
std::uint8_t read(Memory& memory, std::uint32_t location, bool fast = false, Processor cpu = arm9) {
    address(memory,fast ? 0x0b : 3,location,cpu);
    if (fast) check(byte(memory,0,false,cpu) == 0xff, "FAST ne consomme pas son octet dummy");
    return byte(memory,0,true,cpu);
}
void write_one(Memory& memory, std::uint8_t cmd, std::uint32_t location, std::uint8_t value) {
    command(memory,6); packet(memory,cmd,location,std::array{value});
}

void profiles_and_reads() {
    for (unsigned i = 0; i < types.size(); ++i) {
        Memory memory; const auto size = 0x40000U << i;
        std::vector<std::uint8_t> bytes(size,0xff);
        bytes[0] = 0x12; bytes[1] = 0x34; bytes[size - 1] = 0x56;
        prepare(memory,types[i],bytes);
        check(memory.backup().type() == types[i] && memory.backup().bytes().size() == size &&
              !memory.backup_dirty() && status(memory) == 0, "profil flash initial");
        static_cast<void>(byte(memory,0x9f));
        check(byte(memory,0) == 0x20 && byte(memory,0) == 0x40 && byte(memory,0) == 0x12 + i &&
              byte(memory,0,true) == 0xff, "identifiant M45PE20/40/80 incorrect");
        check(read(memory,0) == 0x12 && read(memory,size,true) == 0x12 &&
              read(memory,0xffffff) == 0x56, "adresse flash 24 bits ou miroir de capacité");
        address(memory,3,size - 1);
        check(byte(memory,0) == 0x56 && byte(memory,0) == 0x12 && byte(memory,0,true) == 0x34,
              "lecture flash ne boucle pas sur la capacité");
        const auto before = memory.snapshot_backup();
        expect_failure<ravenemu::Error>([&] { memory.load_backup(types[i],std::span{bytes}.first(size - 1)); }, "image tronquée acceptée");
        expect_failure<ravenemu::Error>([&] { memory.load_backup(types[i],{},4); }, "protection EEPROM inventée sur M45PE");
        check(memory.snapshot_backup().generation == before.generation && read(memory,0) == 0x12,
              "chargement refusé remplace la flash");
    }
}

void program_write_and_erase() {
    for (const auto type : types) {
        Memory memory; prepare(memory,type);
        const auto size = static_cast<std::uint32_t>(memory.backup().bytes().size());
        const auto baseline = memory.snapshot_backup();
        packet(memory,0x0a,0x20,std::array<std::uint8_t,2>{0x5a,0xf0});
        check(!memory.backup().busy() && !memory.backup_dirty(), "PW sans WREN accepté");
        command(memory,6); check(status(memory) == 2, "WREN flash");
        command(memory,4); check(status(memory) == 0, "WRDI flash");
        command(memory,6); packet(memory,2,0x20,std::array<std::uint8_t,2>{0x5a,0xf0});
        check(memory.backup().busy() && !memory.backup_dirty() && memory.backup().bytes()[0x20] == 0xff,
              "PP appliqué avant son cycle interne");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.snapshot_backup()); }, "snapshot flash accepté pendant WIP");
        check(status(memory) == 3, "WEL/WIP flash pendant PP");
        memory.advance_cycles(program_cycles - 129);
        check(memory.backup().busy(), "PP flash termine avant 1,2 ms");
        memory.advance_cycles(1);
        check(read(memory,0x20) == 0x5a && read(memory,0x21) == 0xf0 && read(memory,0x1f) == 0xff &&
              status(memory) == 0 && memory.backup_dirty(), "PP flash ou zone basse faussement protégée");
        memory.acknowledge_backup_saved(baseline.generation); check(memory.backup_dirty(), "acquittement ancien accepté");
        write_one(memory,2,size + 0x20,0xa5); memory.advance_cycles(program_cycles);
        check(read(memory,0x20) == 0 && read(memory,0x21) == 0xf0, "PP doit seulement abaisser les bits");
        write_one(memory,0x0a,0x20,0xff); memory.advance_cycles(write_cycles - 1);
        check(memory.backup().busy() && memory.backup().bytes()[0x20] == 0, "PW flash termine avant 11 ms");
        memory.advance_cycles(1);
        check(read(memory,0x20) == 0xff && read(memory,0x21) == 0xf0, "PW ne restaure pas les bits ou détruit les voisins");
        const auto saved = memory.snapshot_backup(); memory.acknowledge_backup_saved(saved.generation);
        write_one(memory,2,0x21,0xff); memory.advance_cycles(program_cycles);
        check(!memory.backup_dirty() && memory.snapshot_backup().generation == saved.generation, "PP sans changement marque dirty");
        write_one(memory,0x0a,0x100,0x42); memory.advance_cycles(write_cycles);
        command(memory,6); packet(memory,0xdb,0x2f);
        memory.advance_cycles(erase_cycles - 1); check(memory.backup().busy(), "PE flash termine trop tôt");
        memory.advance_cycles(1);
        check(std::all_of(memory.backup().bytes().begin(),memory.backup().bytes().begin() + 0x100,[](auto v) { return v == 0xff; }) &&
              read(memory,0x100) == 0x42, "PE non aligné ou débordant");
        write_one(memory,0x0a,0x10000,0x24); memory.advance_cycles(write_cycles);
        command(memory,6); packet(memory,0xd8,0xabcd);
        check(status(memory) == 3, "SE flash ne démarre pas");
        command(memory,0xb9); command(memory,6); // ignorés pendant WIP
        memory.advance_cycles(sector_cycles - 257);
        check(memory.backup().busy(), "SE flash termine avant 1 s");
        memory.advance_cycles(1);
        check(!memory.backup().busy() && status(memory) == 0 && read(memory,0x100) == 0xff &&
              read(memory,0x10000) == 0x24, "SE flash déborde ou garde WEL");
        packet(memory,0x0a,0x100,std::array<std::uint8_t,1>{0});
        check(!memory.backup().busy(), "WREN reçu pendant WIP réappliqué après SE");
        check(memory.peek32(arm9,0x04000214) == 0 && !memory.dma_pending(arm9), "flash interne lève IRQ/DRQ cartouche");
    }
}

void packets_sleep_and_lifecycle() {
    Machine machine; machine.load_cartridge(Cartridge{cartridge_image()}); machine.load_backup(BackupType::flash_512k);
    auto& memory = machine.memory();
    command(memory,0xb9);
    check(status(memory) == 0xff && read(memory,0) == 0xff, "flash endormie répond aux lectures");
    command(memory,6); command(memory,0xab); check(status(memory) == 0, "WREN accepté pendant DP");
    static_cast<void>(byte(memory,0x9f)); check(byte(memory,0,true) == 0x20, "RDP ne réveille pas la flash");
    memory.write16(arm9,aux,0xe040);
    const auto registers = memory.read32(arm9,aux);
    expect_failure<ravenemu::Error>([&] { memory.write8(arm9,data,1); }, "WRSR EEPROM accepté sur flash M45PE");
    check(memory.read32(arm9,aux) == registers, "commande refusée démarre un échange");
    for (const auto cmd : {2,0x0a,0xdb,0xd8}) {
        command(memory,6); static_cast<void>(byte(memory,static_cast<std::uint8_t>(cmd)));
        static_cast<void>(byte(memory,0)); static_cast<void>(byte(memory,0x12,true));
        check(!memory.backup().busy() && !memory.backup_dirty(), "adresse flash tronquée lance une opération");
    }
    command(memory,6); packet(memory,2,0x12000);
    check(!memory.backup().busy(), "PP flash sans donnée accepté");
    command(memory,6); packet(memory,0xdb,0x12000,std::array<std::uint8_t,1>{0});
    check(!memory.backup().busy(), "PE flash avec octet supplémentaire accepté");
    command(memory,6); address(memory,0x0a,0x120ff); static_cast<void>(byte(memory,0x12));
    expect_failure<ravenemu::Error>([&] { memory.write8(arm9,data,0x34); }, "flash autorise une écriture au-delà de la page");
    expect_failure<ravenemu::Error>([&] { machine.load_backup(BackupType::flash_512k); }, "session flash sélectionnée non reconnue");
    memory.clear_ram(); memory.advance_cycles(write_cycles);
    check(!machine.backup_dirty() && read(memory,0x120ff) == 0xff, "reset valide le préfixe d'un paquet refusé");
    command(memory,6); address(memory,0x0a,0x12000); static_cast<void>(byte(memory,0x12));
    memory.write8(arm9,data,0x34); memory.advance_cycles(63); memory.write16(arm9,aux,0);
    memory.advance_cycles(write_cycles);
    check(!machine.backup_dirty(), "octet tronqué valide un paquet flash partiel");
    write_one(memory,0x0a,0x7ffff,0x56); memory.advance_cycles(123);
    auto independent = memory;
    expect_failure<ravenemu::Error>([&] { machine.load_backup(BackupType::flash_512k); }, "remplacement flash pendant WIP accepté");
    memory.clear_ram(); memory.advance_cycles(write_cycles - 123);
    check(machine.backup_dirty() && read(memory,0x7ffff) == 0x56 && independent.backup().busy(), "reset annule WIP ou copie partage son horloge");
    independent.advance_cycles(std::numeric_limits<std::uint64_t>::max());
    const auto saved = machine.snapshot_backup();
    check(saved.data == independent.snapshot_backup().data && saved.data.size() == 0x80000 && saved.status == 0,
          "copie de flash ou format .sav");
    machine.acknowledge_backup_saved(saved.generation); check(!machine.backup_dirty(), "acquittement flash courant");
    Machine restored; restored.load_cartridge(Cartridge{cartridge_image()}); restored.load_backup(BackupType::flash_512k,saved.data);
    check(read(restored.memory(),0x7ffff,true) == 0x56 && !restored.backup_dirty(), "rechargement .sav flash");
    command(memory,0xb9); memory.clear_ram(); check(read(memory,0x7ffff) == 0x56, "reset ne quitte pas DP");
    machine.load_cartridge(Cartridge{cartridge_image()});
    check(memory.backup().type() == BackupType::none && !machine.backup_dirty(), "flash transférée à une nouvelle cartouche");
}

void independent_firmware_and_backup() {
    Memory memory; prepare(memory,BackupType::flash_256k);
    const auto original = firmware(); memory.load_firmware(original);
    const auto fw_byte = [&](std::uint8_t value, bool last) {
        memory.write16(arm7,0x040001c0,last ? 0x8100 : 0x8900);
        memory.write8(arm7,0x040001c2,value); memory.advance_cycles(64);
        return memory.read8(arm7,0x040001c2);
    };
    static_cast<void>(fw_byte(6,true));
    // Le WEL du firmware ne doit pas autoriser une écriture cartouche.
    packet(memory,0x0a,0,std::array<std::uint8_t,1>{0x42}); check(!memory.backup().busy(), "WEL partagé entre les deux bus");
    for (const auto value : {2,1,0x20,0}) static_cast<void>(fw_byte(static_cast<std::uint8_t>(value),false));
    static_cast<void>(fw_byte(0x5a,true));
    write_one(memory,0x0a,0,0x42);
    check(memory.firmware().busy() && memory.backup().busy(), "deux opérations flash ne peuvent pas coexister");
    memory.advance_cycles(program_cycles);
    check(!memory.firmware().busy() && memory.backup().busy() && memory.firmware().bytes()[0x12000] == 0x5a &&
          memory.backup().bytes()[0] == 0xff, "horloges ou opérations des deux flash confondues");
    memory.advance_cycles(write_cycles);
    check(read(memory,0) == 0x42 && memory.firmware().bytes()[0] == original[0] &&
          memory.backup_dirty() && memory.firmware_dirty(), "protection /W ou données partagées entre firmware et cartouche");
    const auto backup = memory.snapshot_backup(); memory.acknowledge_backup_saved(backup.generation);
    check(!memory.backup_dirty() && memory.firmware_dirty(), "acquittements des deux supports confondus");
}

void executable_programs() {
    for (const auto cpu : {arm9,arm7}) {
        Program program; program.load(0,aux);
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(6);
        program.load(2,0xa040); program.code.push_back(0xe1c020b0);
        for (const auto value : {0x0a,7,0x12,0x34}) program.spi_byte(static_cast<std::uint8_t>(value));
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0x69);
        const auto poll = static_cast<unsigned>(program.code.size());
        program.load(2,0xa040); program.code.push_back(0xe1c020b0); program.spi_byte(5);
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0,3);
        program.code.push_back(0xe3130001);
        const auto branch = static_cast<unsigned>(program.code.size());
        program.code.push_back(0x1a000000U | ((poll - branch - 2U) & 0xffffffU));
        program.load(2,0xa040); program.code.push_back(0xe1c020b0);
        for (const auto value : {3,7,0x12,0x34}) program.spi_byte(static_cast<std::uint8_t>(value));
        program.load(2,0xa000); program.code.push_back(0xe1c020b0); program.spi_byte(0,3);
        program.store(3,0x02001000);
        const auto code = program.finish();
        Machine machine;
        machine.load_cartridge(Cartridge{cpu == arm9 ? cartridge_image(code) : cartridge_image({0xeafffffe},code)});
        machine.load_backup(BackupType::flash_512k);
        if (cpu == arm7) machine.memory().write16(arm9,0x04000204,0x800);
        machine.run_cycles(write_cycles + 12000);
        check(machine.memory().peek32(arm9,0x02001000) == 0x69 && machine.snapshot_backup().data[0x71234] == 0x69,
              "programme ARM9/ARM7 ne sauvegarde pas puis ne relit pas sa flash");
    }
}
}

int main() {
    profiles_and_reads(); program_write_and_erase(); packets_sleep_and_lifecycle();
    independent_firmware_and_backup(); executable_programs();
}
