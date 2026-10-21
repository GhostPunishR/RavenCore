#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/firmware_fixture.hpp"

#include <array>
#include <limits>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm7 = Processor::arm7, arm9 = Processor::arm9;
constexpr std::uint32_t control = 0x040001c0, data = control + 2, flags = 0x04000214;
constexpr std::uint32_t spi_irq = 1U << 23U;
// GBATEK : durées typiques arrondies au cycle supérieur à 33 513 982 Hz.
constexpr std::uint32_t program_cycles = 40217, write_cycles = 368654, erase_cycles = 335140;

namespace {
std::uint8_t byte(Memory& memory, std::uint8_t value) {
    memory.write8(arm7,data,value); memory.advance_cycles(64);
    check(!(memory.peek16(arm7,control) & 0x80U), "byte SPI non terminé");
    return memory.read8(arm7,data);
}
void select(Memory& memory, bool hold = true) {
    memory.write16(arm7,control,0);
    memory.write16(arm7,control,hold ? 0xc900 : 0xc100);
}
void command(Memory& memory, std::uint8_t value) {
    select(memory,false); static_cast<void>(byte(memory,value));
}
void address(Memory& memory, std::uint8_t cmd, std::uint32_t destination) {
    select(memory); static_cast<void>(byte(memory,cmd));
    for (const auto shift : {16U,8U,0U}) static_cast<void>(byte(memory,static_cast<std::uint8_t>(destination >> shift)));
}
void packet(Memory& memory, std::uint8_t cmd, std::uint32_t destination, std::span<const std::uint8_t> payload = {}) {
    address(memory,cmd,destination);
    for (const auto value : payload) static_cast<void>(byte(memory,value));
    memory.write16(arm7,control,0); // remontée de CS après le dernier byte complet
}
std::uint8_t status(Memory& memory) {
    select(memory); static_cast<void>(byte(memory,5)); return byte(memory,0);
}
void write_one(Memory& memory, std::uint32_t destination, std::uint8_t value) {
    command(memory,6); packet(memory,0x0a,destination,std::array{value});
}
bool equals(const Memory& memory, std::uint32_t offset, std::span<const std::uint8_t> expected) {
    return std::equal(expected.begin(),expected.end(),memory.firmware().bytes().begin() + offset);
}

void program_write_and_persistence_test() {
    Memory memory; auto original = firmware();
    original[0x1207f] = 0x5a; original[0x12080] = 0xf0; original[0x12081] = 0x0f; original[0x12082] = 0xaa;
    memory.load_firmware(original);
    const auto baseline = memory.snapshot_firmware();
    check(!memory.firmware_dirty(), "image importée marquée modifiée");
    packet(memory,0x0a,0x12080,std::array<std::uint8_t,3>{0x12,0x34,0x56});
    check(!memory.firmware().busy() && equals(memory,0x12080,std::array<std::uint8_t,3>{0xf0,0x0f,0xaa}) &&
          !memory.firmware_dirty(), "écriture acceptée sans WREN");
    command(memory,6); packet(memory,2,0x12080,std::array<std::uint8_t,3>{0x0f,0xf0,0xff});
    check(memory.firmware().busy() && !memory.firmware_dirty() && equals(memory,0x12080,std::array<std::uint8_t,3>{0xf0,0x0f,0xaa}),
          "PP applique les bits avant la fin du cycle flash");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.snapshot_firmware()); }, "export accepté pendant WIP");
    check(status(memory) == 3, "RDSR doit présenter WIP et WEL pendant PP"); // 128 cycles écoulés
    memory.write32(arm7,flags,spi_irq);
    memory.advance_cycles(program_cycles - 128 - 1);
    check(memory.firmware().busy() && memory.firmware().bytes()[0x12080] == 0xf0, "PP termine trop tôt");
    memory.advance_cycles(1);
    check(!memory.firmware().busy() && memory.firmware_dirty() &&
          equals(memory,0x12080,std::array<std::uint8_t,3>{0,0,0xaa}) && memory.firmware().bytes()[0x1207f] == 0x5a &&
          memory.peek32(arm7,flags) == 0, "PP n'est pas un AND, déborde, ou produit une IRQ interne fictive");
    check(status(memory) == 0, "fin PP ne nettoie pas WEL/WIP");
    memory.acknowledge_firmware_saved(baseline.generation);
    check(memory.firmware_dirty(), "ancien acquittement nettoie une écriture récente");
    const auto first = memory.snapshot_firmware();
    memory.acknowledge_firmware_saved(first.generation);
    check(!memory.firmware_dirty(), "acquittement de la génération courante ignoré");

    write_one(memory,0x12080,0xff); memory.advance_cycles(write_cycles - 1);
    check(memory.firmware().busy() && memory.firmware().bytes()[0x12080] == 0, "PW termine avant 11 ms");
    memory.advance_cycles(1);
    check(memory.firmware_dirty() && equals(memory,0x1207f,std::array<std::uint8_t,4>{0x5a,0xff,0,0xaa}),
          "PW ne remet pas les bits à un ou détruit les voisins non écrits");
    const auto second = memory.snapshot_firmware();
    check(first.data[0x12080] == 0 && second.data[0x12080] == 0xff && second.generation != first.generation,
          "instantanés partagés ou génération non avancée");
    memory.acknowledge_firmware_saved(first.generation); check(memory.firmware_dirty(), "acquittement retardé perdu");
    memory.acknowledge_firmware_saved(second.generation);
    write_one(memory,0x12080,0xff); memory.advance_cycles(write_cycles);
    check(!memory.firmware_dirty() && memory.snapshot_firmware().generation == second.generation, "écriture identique salit le firmware");
    Memory reloaded; reloaded.load_firmware(second.data);
    check(!reloaded.firmware_dirty() && equals(reloaded,0x1207f,std::array<std::uint8_t,4>{0x5a,0xff,0,0xaa}),
          "export/rechargement ne conserve pas les octets");
    // Les jetons de l'ancienne image ne doivent jamais acquitter la suivante.
    memory.load_firmware(original); write_one(memory,0x12080,0x12); memory.advance_cycles(write_cycles);
    memory.acknowledge_firmware_saved(second.generation);
    check(memory.firmware_dirty(), "remplacement d'image réutilise une génération précédente");
}

void erase_and_protection_test() {
    for (const auto capacity : {0x40000U,0x80000U}) {
        auto original = firmware(capacity,capacity - 0x200);
        std::fill(original.begin() + 0x10000,original.begin() + 0x20000,0);
        original[0xffff] = 0x69; original[0x20000] = 0x96;
        Memory memory; memory.load_firmware(original);
        command(memory,6); packet(memory,0xdb,0x112ab);
        memory.advance_cycles(erase_cycles - 1);
        check(memory.firmware().busy() && memory.firmware().bytes()[0x112ab] == 0, "PE termine trop tôt");
        memory.advance_cycles(1);
        const auto bytes = memory.firmware().bytes();
        check(std::all_of(bytes.begin() + 0x11200,bytes.begin() + 0x11300,[](auto v) { return v == 0xff; }) &&
              bytes[0x111ff] == 0 && bytes[0x11300] == 0, "PE n'est pas aligné sur 256 octets");
        command(memory,6); packet(memory,0xd8,0x1abcd);
        check(status(memory) == 3, "SE ne démarre pas");
        command(memory,0xb9); command(memory,6); // ignorés pendant WIP
        memory.advance_cycles(std::numeric_limits<std::uint64_t>::max());
        check(!memory.firmware().busy() && std::all_of(bytes.begin() + 0x10000,bytes.begin() + 0x20000,[](auto v) { return v == 0xff; }) &&
              bytes[0xffff] == 0x69 && bytes[0x20000] == 0x96 && status(memory) == 0, "SE déborde de son secteur ou ne termine pas");
        select(memory); static_cast<void>(byte(memory,0x9f)); check(byte(memory,0) == 0x20, "DP accepté pendant WIP");
        packet(memory,0x0a,0x12000,std::array<std::uint8_t,1>{0});
        check(!memory.firmware().busy(), "WREN accepté pendant WIP reste actif après SE");
        const auto saved = memory.snapshot_firmware(); memory.acknowledge_firmware_saved(saved.generation);
        for (const auto cmd : {0x02,0x0a,0xdb,0xd8}) {
            command(memory,6);
            packet(memory,static_cast<std::uint8_t>(cmd),capacity + 0xff00,
                   cmd == 2 || cmd == 0x0a ? std::span<const std::uint8_t>{std::array<std::uint8_t,1>{0}} : std::span<const std::uint8_t>{});
            memory.advance_cycles(timer_clock_hz);
            check(!memory.firmware().busy() && !memory.firmware_dirty() && memory.firmware().bytes()[0xffff] == 0x69 &&
                  memory.firmware().bytes()[0x20] == original[0x20], "zone /W protégée modifiée via un miroir");
        }
        write_one(memory,capacity + 0x12000,0x42); memory.advance_cycles(write_cycles);
        check(memory.firmware().bytes()[0x12000] == 0x42, "adresse d'écriture ne respecte pas la capacité physique");
    }
}

void packets_and_clock_test() {
    Memory memory; memory.load_firmware(firmware());
    command(memory,6); address(memory,0x0a,0x12000); static_cast<void>(byte(memory,0x12));
    memory.advance_cycles(write_cycles);
    check(!memory.firmware().busy() && !memory.firmware_dirty(), "PW commence avant CS haut");
    memory.write8(arm7,data,0x34); memory.advance_cycles(31); memory.write16(arm7,control,0);
    memory.advance_cycles(write_cycles);
    check(!memory.firmware_dirty() && memory.firmware().bytes()[0x12000] == 0xff, "byte tronqué lance le préfixe du paquet");
    for (const auto cmd : {2,0x0a,0xdb,0xd8}) {
        command(memory,6); select(memory); static_cast<void>(byte(memory,static_cast<std::uint8_t>(cmd)));
        static_cast<void>(byte(memory,1)); static_cast<void>(byte(memory,0x20)); memory.write16(arm7,control,0);
        check(!memory.firmware().busy(), "adresse tronquée lance une opération");
    }
    command(memory,6); packet(memory,2,0x12000);
    check(!memory.firmware().busy(), "PP sans donnée lance un cycle");
    command(memory,6); packet(memory,0xdb,0x12000,std::array<std::uint8_t,1>{0});
    check(!memory.firmware().busy(), "PE avec byte supplémentaire efface la page");
    command(memory,6); address(memory,2,0x120ff); static_cast<void>(byte(memory,0x34));
    const auto before = memory.read32(arm7,control);
    expect_failure<ravenemu::Error>([&] { memory.write8(arm7,data,0x56); }, "franchissement de page accepté sans comportement validé");
    check(memory.read32(arm7,control) == before && !memory.firmware_dirty(), "erreur de page mutante");
    memory.clear_ram(); // paquet non terminé abandonné sans écrire le préfixe
    command(memory,6); address(memory,0x0a,0x12100);
    for (unsigned i = 0; i < 256; ++i) static_cast<void>(byte(memory,static_cast<std::uint8_t>(i)));
    expect_failure<ravenemu::Error>([&] { memory.write8(arm7,data,0); }, "257e byte accepté");
    memory.clear_ram(); check(!memory.firmware().busy() && !memory.firmware_dirty(), "reset valide un paquet inachevé");

    command(memory,6); address(memory,2,0x12000); memory.write16(arm7,control,0xc100); memory.write8(arm7,data,0x5a);
    auto split = memory;
    memory.advance_cycles(64U + program_cycles - 1U);
    check(memory.firmware().busy() && memory.firmware().bytes()[0x12000] == 0xff,
          "grande avance débite les cycles SPI à l'opération flash");
    memory.advance_cycles(1);
    split.advance_cycles(32); split.advance_cycles(32); split.advance_cycles(program_cycles);
    check(memory.snapshot_firmware().data == split.snapshot_firmware().data && memory.firmware().bytes()[0x12000] == 0x5a,
          "avances fractionnées diffèrent de l'avance groupée");
}

void reset_copy_and_generation_test() {
    Memory memory; const auto original = firmware(); memory.load_firmware(original);
    write_one(memory,0x12000,0x21); memory.advance_cycles(77);
    auto copy = memory;
    expect_failure<ravenemu::RomLoadError>([&] { memory.load_firmware({}); }, "chargement vide accepté pendant WIP");
    memory.clear_ram(); memory.advance_cycles(write_cycles - 77);
    check(memory.firmware_dirty() && memory.firmware().bytes()[0x12000] == 0x21 && memory.peek32(arm7,flags) == 0 &&
          copy.firmware().busy() && copy.firmware().bytes()[0x12000] == 0xff, "reset annule l'opération autonome ou copie partage l'image");
    copy.advance_cycles(write_cycles - 77);
    check(copy.snapshot_firmware().data == memory.snapshot_firmware().data, "copie WIP perd son contenu");
    const auto copied = copy.snapshot_firmware(); copy.acknowledge_firmware_saved(copied.generation);
    check(!copy.firmware_dirty() && memory.firmware_dirty(), "copies partagent l'acquittement de sauvegarde");
    write_one(memory,0x12001,0x43); memory.load_firmware(original);
    memory.advance_cycles(write_cycles);
    check(!memory.firmware().busy() && !memory.firmware_dirty() && memory.firmware().bytes()[0x12001] == 0xff,
          "nouvelle image reçoit une ancienne opération en attente");
    Memory absent;
    expect_failure<ravenemu::Error>([&] { static_cast<void>(absent.snapshot_firmware()); }, "export inventé sans image");
}

void settings_commit_and_reload_test() {
    const auto original = firmware(); auto updated = original;
    settings(updated,0x3ff00,11,9); put16(updated,0x3ff58,0x400); seal(updated,0x3ff00);
    Machine machine; machine.load_firmware(original);
    auto& memory = machine.memory(); command(memory,6);
    packet(memory,0x0a,0x3ff00,std::span<const std::uint8_t>{updated}.subspan(0x3ff00,0x100));
    check(memory.firmware().settings()->offset == 0x3fe00, "réglages changent avant la fin d'écriture");
    machine.advance_cycles(write_cycles);
    check(memory.firmware().settings()->offset == 0x3ff00 && memory.firmware().settings()->data[2] == 9,
          "cache des réglages pas recalculé après PW");
    machine.set_touch_position(128,96); check(machine.input_state().touch.x == 0x900, "nouvel étalonnage non pris en compte");
    const auto saved = machine.snapshot_firmware();
    machine.acknowledge_firmware_saved(saved.generation); check(!machine.firmware_dirty(), "API machine n'acquitte pas le firmware");
    auto edited = saved; edited.data[0] = 0;
    check(memory.firmware().bytes()[0] == 0x5a, "export ne possède pas ses octets");
    Machine reopened; reopened.load_firmware(saved.data); reopened.load_cartridge(Cartridge{cartridge_image()});
    reopened.set_touch_position(128,96);
    check(reopened.memory().peek8(arm9,firmware_settings_address + 2) == 9 && reopened.input_state().touch.x == 0x900 &&
          !reopened.firmware_dirty(), "sauvegarde/rechargement perd réglages et étalonnage");
    write_one(memory,0x3ff72,static_cast<std::uint8_t>(updated[0x3ff72] ^ 1U)); machine.advance_cycles(write_cycles);
    check(memory.firmware().settings()->offset == 0x3fe00, "CRC devenu invalide ne restaure pas l'ancienne copie");
    machine.acknowledge_firmware_saved(saved.generation); check(machine.firmware_dirty(), "ancien export masque un nouveau changement CRC");
}

void executable_test() {
    Program program; program.load(0,control);
    program.load(1,0xc100); program.code.push_back(0xe1c010b0); program.spi_byte(6);
    program.load(1,0xc900); program.code.push_back(0xe1c010b0);
    for (const auto value : {2,1,0x20,0}) program.spi_byte(static_cast<std::uint8_t>(value));
    program.load(1,0xc100); program.code.push_back(0xe1c010b0); program.spi_byte(0x5a);
    const auto poll = static_cast<unsigned>(program.code.size());
    program.load(1,0xc900); program.code.push_back(0xe1c010b0); program.spi_byte(5);
    program.load(1,0xc100); program.code.push_back(0xe1c010b0); program.spi_byte(0,2);
    program.code.push_back(0xe3120001); // tst r2,#WIP
    const auto branch = static_cast<unsigned>(program.code.size());
    program.code.push_back(0x1a000000U | ((poll - branch - 2U) & 0xffffffU));
    program.load(1,0xc900); program.code.push_back(0xe1c010b0);
    for (const auto value : {3,1,0x20,0}) program.spi_byte(static_cast<std::uint8_t>(value));
    program.load(1,0xc100); program.code.push_back(0xe1c010b0); program.spi_byte(0,2);
    program.store(2,0x02001000);
    const auto rom = cartridge_image({0xeafffffe},program.finish());
    Machine machine; machine.load_firmware(firmware()); machine.load_cartridge(Cartridge{rom});
    machine.run_cycles(45000);
    check(machine.memory().peek32(arm9,0x02001000) == 0x5a && machine.memory().firmware().bytes()[0x12000] == 0x5a &&
          machine.firmware_dirty() && !machine.memory().firmware().busy(), "ARM7 ne programme pas la flash puis ne sort pas de sa boucle WIP");
    const auto saved = machine.snapshot_firmware();
    machine.load_cartridge(Cartridge{rom});
    check(machine.firmware_dirty() && machine.memory().firmware().bytes()[0x12000] == saved.data[0x12000],
          "rechargement cartouche efface le firmware modifié");
}
} // namespace

int main() {
    program_write_and_persistence_test();
    erase_and_protection_test();
    packets_and_clock_test();
    reset_copy_and_generation_test();
    settings_commit_and_reload_test();
    executable_test();
}
