#include <ravenemu/nds/cpu.hpp>
#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <utility>
#include <vector>

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t base = 0x02000000;
constexpr std::uint32_t data = base + 0x4000;
constexpr std::uint32_t high = 0x2078;

namespace {
constexpr std::uint32_t cp(bool read, unsigned rd, unsigned crn, unsigned crm, unsigned op = 0) {
    return 0xee000f10U | (read ? 0x100000U : 0U) | (crn << 16U) | (rd << 12U) | (op << 5U) | crm;
}
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
void region(Cp15& control, unsigned n, std::uint32_t address, unsigned size = 11) {
    control.write(6, n, 0, address | (size << 1U) | 1U);
}
void permissions(Cp15& control, std::uint32_t values, bool code = false) {
    control.write(5, 0, code ? 3 : 2, values);
}
void setup(Memory& memory, bool high_vectors = true) {
    auto& control = memory.cp15();
    region(control, 0, base);
    region(control, 1, data);
    region(control, 2, high_vectors ? 0xffff0000U : 0U);
    permissions(control, 0x103); // RAM code R/W, données interdites, vecteurs privilégiés
    permissions(control, 0x103, true);
    if (high_vectors) {
        std::vector<std::uint8_t> bios(0x1000);
        put32(bios, 0x0c, 0xe25ef004); // SUBS pc,lr,#4 : retenter le fetch
        put32(bios, 0x10, 0xe25ef008); // SUBS pc,lr,#8 : retenter l'accès
        memory.load_bios(arm9, bios);
    } else {
        control.write(9, 1, 1, 0xc); // ITCM 32 Kio pour les vecteurs bas
        control.write(1, 0, 0, 0x40000);
        memory.write32(arm9, 0x0c, 0xe25ef004);
        memory.write32(arm9, 0x10, 0xe25ef008);
    }
    control.write(1, 0, 0, high_vectors ? high | 1U : 0x40079U);
}

void registers_test() {
    Cp15 control;
    check(control.permits(0xdeadbeef, Cp15::Access::write, false), "MPU désactivée bloque un accès");
    control.write(2, 0, 0, 0x12345678);
    control.write(2, 0, 1, 0xabcdef12);
    control.write(3, 0, 0, 0x87654321);
    check(control.read(2, 0, 0) == 0x78 && control.read(2, 0, 1) == 0x12 && control.read(3, 0, 0) == 0x21,
          "masques et indépendance C/B des régions MPU");
    control.write(5, 0, 0, 0xffffe4e4);
    check(control.read(5, 0, 0) == 0xe4e4 && control.read(5, 0, 2) == 0x32103210 &&
          control.read(5, 0, 3) == 0, "AP compactes / étendues et indépendance code-données");
    control.write(5, 0, 3, 0x65036503);
    check(control.read(5, 0, 1) == 0x9393 && control.read(5, 0, 3) == 0x65036503,
          "lecture compacte des AP étendues");
    control.write(5, 0, 1, 0xffff);
    check(control.read(5, 0, 3) == 0x33333333, "AP compactes remplacent les AP étendues");
    for (const auto ap : {4U, 7U, 8U, 15U}) {
        expect_failure<ravenemu::Error>([&] { control.write(5, 0, 3, 0x3333333U | (ap << 28U)); }, "AP réservée acceptée");
        check(control.read(5, 0, 3) == 0x33333333, "écriture AP refusée partiellement appliquée");
    }
    for (unsigned i = 0; i < 8; ++i) {
        control.write(6, i, i & 1U, base + i * 0x1000U + 0xfd7U);
        check(control.read(6, i, 0) == base + i * 0x1000U + 0x17U &&
              control.read(6, i, 1) == control.read(6, i, 0), "régions unifiées / alias / bits réservés");
    }
    for (const auto invalid : {0x02000015U, 0x0200003fU, 0x02001019U}) {
        const auto before = control.read(6, 7, 0);
        expect_failure<ravenemu::Error>([&] { control.write(6, 7, 0, invalid); }, "région MPU invalide acceptée");
        check(control.read(6, 7, 0) == before, "région refusée a modifié la configuration");
    }
    control.write(6, 7, 1, 0); // désactivation sans taille valide
    check(control.read(6, 7, 0) == 0, "région MPU désactivée");
    control.write(1, 0, 0, high | 1U);
    check(control.read(1, 0, 0) == (high | 1U), "MPU ne peut être activée");
    control.reset();
    check(control.read(1, 0, 0) == high && control.read(6, 0, 0) == 0 && control.read(5, 0, 2) == 0 &&
          control.read(2, 0, 0) == 0 && control.read(3, 0, 0) == 0, "reset MPU incomplet");
}

void permission_matrix_test() {
    // Tables indépendantes : R privilégié, W privilégié, R User, W User.
    struct Row { unsigned ap; std::array<bool, 4> allowed; };
    const std::array<Row, 6> rows{{{0,{false,false,false,false}}, {1,{true,true,false,false}},
        {2,{true,true,true,false}}, {3,{true,true,true,true}}, {5,{true,false,false,false}}, {6,{true,false,true,false}}}};
    Cp15 control;
    control.write(1, 0, 0, high | 1U);
    for (unsigned region_number = 0; region_number < 8; ++region_number) {
        for (unsigned i = 0; i < 8; ++i) control.write(6, i, 0, 0);
        region(control, region_number, base);
        for (const auto& row : rows) {
            permissions(control, row.ap << (region_number * 4U));
            permissions(control, row.ap << (region_number * 4U), true);
            for (unsigned i = 0; i < 4; ++i) {
                const auto access = (i & 1U) ? Cp15::Access::write : Cp15::Access::read;
                check(control.permits(base + 0xfff, access, i < 2) == row.allowed[i], "table AP données User/privilégié");
            }
            check(control.permits(base, Cp15::Access::execute, true) == row.allowed[0] &&
                  control.permits(base, Cp15::Access::execute, false) == row.allowed[2], "table AP instructions");
            check(!control.permits(base - 1, Cp15::Access::read, true) &&
                  !control.permits(base + 0x1000, Cp15::Access::read, true), "arrière-plan ou frontière de région accessible");
        }
    }
    // 4 Gio ne doit ni déborder ni ignorer le dernier octet.
    region(control, 0, 0, 31);
    permissions(control, 3);
    permissions(control, 0x60000003, true);
    for (const auto address : {0U, 0x7fffffffU, 0xffffffffU})
        check(control.permits(address, Cp15::Access::write, false), "région 4 Gio / débordement");
    // La région 7 encore active, sans permission de données, masque la région 0.
    check(!control.permits(base, Cp15::Access::read, true) &&
          control.permits(base, Cp15::Access::execute, false), "priorité 7 et permissions code/données séparées");
    control.write(6, 7, 1, 0);
    check(control.permits(base, Cp15::Access::write, false), "désactivation prioritaire ne révèle pas la région 0");
    for (unsigned n = 1; n < 8; ++n) {
        region(control, n, base);
        check(!control.permits(base, Cp15::Access::read, true), "priorité des régions superposées");
        control.write(6, n, 0, 0);
    }
}

void prefetch_test() {
    for (const auto high_vectors : {false, true}) for (const auto thumb : {false, true}) {
        Memory memory;
        setup(memory, high_vectors);
        Cpu cpu{arm9};
        const auto pc = base + (thumb ? 2U : 0U);
        if (thumb) memory.write16(arm9, pc, 0x202a);
        else memory.write32(arm9, pc, 0xe3a0002a);
        permissions(memory.cp15(), 0x100, true); // code interdit, vecteurs autorisés
        cpu.reset(pc, thumb);
        cpu.set_register(13, 0x02008000);
        const auto before = cpu.state();
        cpu.step(memory);
        check(cpu.state().mode == CpuMode::abort && !cpu.state().thumb && cpu.state().irq_disabled &&
              cpu.state().spsr() == before.cpsr() && cpu.state().registers[14] == pc + 4 &&
              cpu.state().registers[15] == (high_vectors ? 0xffff000cU : 0xcU) && cpu.state().instructions == 0,
              "Prefetch Abort ARM/Thumb : vecteur, LR, SPSR et compteur");
        permissions(memory.cp15(), 0x103, true);
        cpu.step(memory); // gestionnaire : SUBS pc,lr,#4
        check(cpu.state().mode == CpuMode::system && cpu.state().thumb == thumb &&
              cpu.state().registers[15] == pc && cpu.state().registers[13] == 0x02008000, "retour Prefetch Abort");
        cpu.step(memory);
        check(cpu.state().registers[0] == 42 && cpu.state().instructions == 2, "reprise instruction après Prefetch Abort");
    }
}

void data_abort_test() {
    const std::array<std::uint32_t, 7> arm_opcodes{
        0xe5a01004, 0xe5b02004, 0xe5c01000, 0xe1c010b0, 0xe1d020b0, 0xe1d020d0, 0xe1d020f0};
    const std::array<std::uint16_t, 6> thumb_opcodes{0x6001, 0x6801, 0x7001, 0x7801, 0x8001, 0x8801};
    Memory memory;
    setup(memory);
    for (const auto thumb : {false, true}) {
        const auto count = thumb ? thumb_opcodes.size() : arm_opcodes.size();
        for (std::size_t i = 0; i < count; ++i) {
            Cpu cpu{arm9};
            cpu.reset(base, thumb);
            cpu.set_register(0, data);
            cpu.set_register(1, 0x12345678);
            cpu.set_register(2, 0xabcdef01);
            memory.write32(arm9, data, 0xaabbccdd);
            memory.write32(arm9, data + 4, 0x11223344);
            if (thumb) memory.write16(arm9, base, thumb_opcodes[i]);
            else memory.write32(arm9, base, arm_opcodes[i]);
            const auto before = cpu.state();
            cpu.step(memory);
            check(cpu.state().mode == CpuMode::abort && cpu.state().registers[15] == 0xffff0010 &&
                  cpu.state().registers[14] == base + 8 && cpu.state().spsr() == before.cpsr() &&
                  cpu.state().instructions == 0, "Data Abort : état ARM/Thumb");
            check(cpu.state().registers[0] == data && cpu.state().registers[1] == 0x12345678 &&
                  cpu.state().registers[2] == 0xabcdef01 && memory.read32(arm9, data) == 0xaabbccdd &&
                  memory.read32(arm9, data + 4) == 0x11223344, "Data Abort a validé données, destination ou writeback");
        }
    }
    // Le gestionnaire modifie lui-même les permissions par MCR puis retente LDR.
    std::vector<std::uint8_t> bios(0x1000);
    put32(bios, 0x10, cp(false, 2, 5, 0, 2));
    put32(bios, 0x14, 0xe25ef008);
    memory.load_bios(arm9, bios);
    Cpu cpu{arm9};
    cpu.reset(base, false);
    cpu.set_register(0, data); cpu.set_register(2, 0x133);
    memory.write32(arm9, base, 0xe5901000);
    for (unsigned i = 0; i < 4; ++i) cpu.step(memory);
    check(cpu.state().mode == CpuMode::system && cpu.state().registers[1] == 0xaabbccdd &&
          cpu.state().registers[15] == base + 4 && cpu.state().instructions == 3, "gestionnaire MPU / reprise LDR");
    // User peut lire AP=2, mais pas écrire ; le fetch utilise ses propres AP.
    permissions(memory.cp15(), 0x123);
    cpu.reset(base, false);
    cpu.set_register(0, data); cpu.set_register(3, 0x10);
    memory.write32(arm9, base, 0xe121f003);
    memory.write32(arm9, base + 4, 0xe5901000);
    memory.write32(arm9, base + 8, 0xe5801000);
    cpu.step(memory); cpu.step(memory);
    check(cpu.state().mode == CpuMode::user && cpu.state().registers[1] == 0xaabbccdd, "lecture User AP=2");
    cpu.step(memory);
    check(cpu.state().mode == CpuMode::abort && (cpu.state().spsr() & 0x1fU) == 0x10 &&
          cpu.state().registers[14] == base + 16, "écriture User AP=2 / LR Data Abort");
    // Une page exécutable seulement en privilégié doit refuser le fetch User.
    permissions(memory.cp15(), 0x101, true);
    cpu.reset(base, false); cpu.set_register(3, 0x10);
    memory.write32(arm9, base, 0xe121f003);
    cpu.step(memory); cpu.step(memory);
    check(cpu.state().registers[15] == 0xffff000c && (cpu.state().spsr() & 0x1fU) == 0x10 &&
          cpu.state().instructions == 1, "permissions de fetch User ignorées");
    permissions(memory.cp15(), 0x103, true);
    // Un literal Thumb est une lecture de données, même dans une page exécutable.
    permissions(memory.cp15(), 0x100);
    cpu.reset(base, true);
    memory.write16(arm9, base, 0x4800);
    cpu.step(memory);
    check(cpu.state().registers[15] == 0xffff0010, "literal Thumb contourne permissions de données");
}

void multiple_and_mmio_test() {
    Memory memory;
    setup(memory);
    const auto boundary = base + 0xffc;
    Cpu cpu{arm9};
    // Le modèle conserve la validation transactionnelle LDM/STM, y compris MPU.
    for (const auto raw : {0xe8a00006U, 0xe8b00006U}) {
        memory.write32(arm9, base, raw);
        memory.write32(arm9, boundary, 0xaaaaaaaa);
        memory.write32(arm9, boundary + 4, 0xbbbbbbbb);
        cpu.reset(base, false);
        cpu.set_register(0, boundary); cpu.set_register(1, 0x11111111); cpu.set_register(2, 0x22222222);
        cpu.step(memory);
        check(cpu.state().mode == CpuMode::abort && cpu.state().registers[0] == boundary &&
              cpu.state().registers[1] == 0x11111111 && cpu.state().registers[2] == 0x22222222 &&
              memory.read32(arm9, boundary) == 0xaaaaaaaa && memory.read32(arm9, boundary + 4) == 0xbbbbbbbb,
              "transfert multiple fautif doit conserver le contrat transactionnel");
    }
    memory.write16(arm9, 0x04000184, 0x8000);
    memory.write16(arm7, 0x04000184, 0x8000);
    memory.write32(arm7, 0x04000188, 0x12345678);
    memory.write32(arm9, base, 0xe5901000);
    cpu.reset(base, false); cpu.set_register(0, 0x04100000); cpu.step(memory);
    check(cpu.state().mode == CpuMode::abort && memory.peek32(arm9, 0x04100000) == 0x12345678,
          "lecture refusée retire un mot IPC");
    memory.write32(arm9, base, 0xe5801000);
    cpu.reset(base, false); cpu.set_register(0, 0x04000188); cpu.set_register(1, 42); cpu.step(memory);
    check(cpu.state().mode == CpuMode::abort && (memory.read16(arm7, 0x04000184) & 0x100U),
          "écriture refusée envoie un mot IPC");
    // Inspection native et chargement restent utilisables sous MPU.
    check(memory.read32(arm9, 0x04100000) == 0x12345678, "inspection hôte bloquée par MPU");
}

void tcm_dma_and_arm7_test() {
    Memory memory;
    setup(memory);
    memory.cp15().write(9, 1, 0, data | 0xa);
    memory.cp15().write(1, 0, 0, high | 0x10001);
    memory.write32(arm9, data, 0x11223344);
    memory.write32(arm9, base, 0xe5901000);
    Cpu cpu{arm9};
    cpu.reset(base, false); cpu.set_register(0, data); cpu.step(memory);
    check(cpu.state().mode == CpuMode::abort, "DTCM contourne MPU");
    permissions(memory.cp15(), 0x133);
    cpu.step(memory); cpu.step(memory);
    check(cpu.state().registers[1] == 0x11223344 && memory.read32(arm7, data) == 0, "DTCM autorisée après retour Abort");
    permissions(memory.cp15(), 0);
    permissions(memory.cp15(), 0, true);
    memory.write32(arm7, data, 0xdeadbeef);
    memory.write32(arm9, 0x040000b0, data);
    memory.write32(arm9, 0x040000b4, data + 4);
    memory.write32(arm9, 0x040000b8, 0x84000001);
    cpu.reset(base, false); cpu.step(memory);
    check(cpu.state().registers[15] == base && memory.read32(arm7, data + 4) == 0xdeadbeef &&
          memory.read32(arm9, data + 4) == 0, "DMA ne doit consulter ni MPU ni DTCM");
    cpu.step(memory);
    check(cpu.state().registers[15] == 0xffff000c, "DMA doit précéder l'exception fetch");
    memory.write32(arm7, base, 0xe3a0002a);
    Cpu other{arm7}; other.reset(base, false); other.step(memory);
    check(other.state().registers[0] == 42 && other.state().mode == CpuMode::system, "MPU ARM9 affecte ARM7");
    // ITCM reste protégée sur le chemin de fetch, avant le décodage matériel.
    memory.cp15().write(9, 1, 1, 0xc);
    memory.cp15().write(1, 0, 0, high | 0x50001);
    memory.write32(arm9, 0x1000, 0xe3a00007);
    cpu.reset(0x1000, false); cpu.step(memory);
    check(cpu.state().registers[15] == 0xffff000c, "fetch ITCM contourne MPU");
    region(memory.cp15(), 3, 0x1000);
    permissions(memory.cp15(), 0x3103, true);
    cpu.reset(0x1000, false); cpu.step(memory);
    check(cpu.state().registers[0] == 7, "fetch ITCM autorisée reste bloquée");
    // Les permissions portent sur l'adresse CPU, sans propagation aux miroirs RAM.
    permissions(memory.cp15(), 0x103, true);
    cpu.reset(0x02400000, false); cpu.step(memory);
    check(cpu.state().registers[15] == 0xffff000c, "miroir RAM contourne régions MPU");
}

void cache_commands_test() {
    Memory memory;
    Cpu cpu{arm9};
    for (const auto& [crm, op] : std::array<std::pair<unsigned, unsigned>, 9>{{
        {5,0},{5,1},{6,0},{6,1},{10,1},{10,2},{13,1},{14,1},{14,2}}}) {
        memory.write32(arm9, base, cp(false, 0, 7, crm, op));
        cpu.reset(base, false); cpu.set_register(0, 0xdeadbeef); cpu.step(memory);
        check(cpu.state().instructions == 1 && cpu.state().registers[0] == 0xdeadbeef,
              "maintenance cache cohérente doit terminer sans accès au bus");
    }
    for (const auto raw : {cp(false, 0, 7, 0, 4), cp(false, 0, 7, 8, 2), cp(true, 0, 7, 5)}) {
        memory.write32(arm9, base, raw); cpu.reset(base, false);
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "WFI ou lecture cache absente ignorée");
        check(cpu.state() == before, "commande cache refusée a modifié CPU");
    }
}

void machine_test() {
    // Démarrage synthétique : le code configure puis active sa propre MPU.
    std::vector<std::uint32_t> program;
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 6> settings{{
        {cp(false,0,6,0), base | 0x2bU}, {cp(false,0,5,0,2),3}, {cp(false,0,5,0,3),3},
        {cp(false,0,2,0),1}, {cp(false,0,3,0),1}, {cp(false,0,1,0),high | 1U}}};
    const auto literal_start = static_cast<std::uint32_t>(settings.size() * 8U + 8U);
    for (std::uint32_t i = 0; i < settings.size(); ++i) {
        program.push_back(0xe59f0000U | (literal_start + i * 4U - (i * 8U + 8U)));
        program.push_back(settings[i].first);
    }
    program.push_back(0xe3a0102a); program.push_back(0xeafffffe);
    for (const auto& setting : settings) program.push_back(setting.second);
    std::vector<std::uint8_t> rom(0x200 + program.size() * 4U + 4U);
    put32(rom,0x20,0x200); put32(rom,0x24,base); put32(rom,0x28,base);
    put32(rom,0x2c,static_cast<std::uint32_t>(program.size() * 4U));
    const auto offset = static_cast<std::uint32_t>(0x200 + program.size() * 4U);
    put32(rom,0x30,offset); put32(rom,0x34,0x03800000); put32(rom,0x38,0x03800000); put32(rom,0x3c,4);
    for (std::size_t i = 0; i < program.size(); ++i) put32(rom,0x200+i*4U,program[i]);
    put32(rom,offset,0xeafffffe);
    Machine machine; machine.load_cartridge(Cartridge{rom}); machine.run_steps(arm9,13);
    check(machine.cpu_state(arm9).registers[1] == 42 && machine.memory().cp15().read(1,0,0) == (high|1U) &&
          !machine.memory().cp15().permits(0x04000000,Cp15::Access::read,true), "ROM d'initialisation MPU");
    auto copy = machine;
    permissions(copy.memory().cp15(),0);
    check(machine.memory().cp15().read(5,0,2) == 3, "copie MPU partage ses permissions");
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    const auto before = machine.cpu_state(arm9);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(Cartridge{invalid}); }, "ROM invalide acceptée");
    check(machine.cpu_state(arm9) == before && machine.memory().cp15().read(6,0,0) == (base|0x2bU),
          "chargement refusé efface MPU");
    machine.load_cartridge(Cartridge{rom});
    check(machine.memory().cp15().read(1,0,0) == high && machine.memory().cp15().read(6,0,0) == 0 &&
          machine.memory().cp15().read(5,0,2) == 0 && machine.memory().cp15().read(2,0,0) == 0,
          "rechargement conserve la MPU précédente");
}
} // namespace

int main() {
    registers_test();
    permission_matrix_test();
    prefetch_test();
    data_abort_test();
    multiple_and_mmio_test();
    tcm_dma_and_arm7_test();
    cache_commands_test();
    machine_test();
}
