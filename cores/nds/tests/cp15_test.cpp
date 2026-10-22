#include <ravenemu/nds/cpu.hpp>
#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <vector>
#include <utility>

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t base = 0x02000000;
constexpr std::uint32_t dtcm = 0x02010000;
constexpr std::uint32_t high = 0x2078;
constexpr std::uint32_t data_enable = 1U << 16U;
constexpr std::uint32_t data_load = 1U << 17U;
constexpr std::uint32_t code_enable = 1U << 18U;
constexpr std::uint32_t code_load = 1U << 19U;

namespace {
constexpr std::uint32_t cp(bool read, unsigned rd, unsigned crn, unsigned crm, unsigned op2 = 0) {
    return 0xee000f10U | (read ? 0x100000U : 0U) | (crn << 16U) | (rd << 12U) | (op2 << 5U) | crm;
}
void control(Memory& memory, std::uint32_t value) { memory.cp15().write(1, 0, 0, value); }
void region(Memory& memory, bool code, std::uint32_t value) { memory.cp15().write(9, 1, code ? 1 : 0, value); }
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
void execute(Cpu& cpu, Memory& memory, std::uint32_t raw) {
    cpu.set_register(15, base);
    memory.write32(arm9, base, raw);
    cpu.step(memory);
}

void registers_test() {
    Memory memory;
    Cpu cpu{arm9};
    cpu.reset(base, false);
    for (const auto& [op, expected] : std::array<std::pair<unsigned, std::uint32_t>, 4>{{
        {0, 0x41059461}, {1, 0x0f0d2112}, {2, 0x00140180}, {7, 0x41059461}}}) {
        execute(cpu, memory, cp(true, 0, 0, 0, op));
        check(cpu.state().registers[0] == expected, "identification ARM946/TCM/cache");
    }
    cpu.set_register(0, 0xfffff07e); // R/W et réservés, sauf MPU et endian
    execute(cpu, memory, cp(false, 0, 1, 0));
    execute(cpu, memory, cp(true, 1, 1, 0));
    check(cpu.state().registers[1] == 0x000ff07c, "masques et bits fixes du contrôle ARM946");
    control(memory, high);
    cpu.set_register(0, dtcm | 0xfca); // bits réservés effacés, taille 16 Kio
    execute(cpu, memory, cp(false, 0, 9, 1));
    execute(cpu, memory, cp(true, 1, 9, 1));
    check(cpu.state().registers[1] == (dtcm | 0xa), "registre DTCM");
    cpu.set_register(0, 0xfffff020); // ITCM fixe à zéro, fenêtre 32 Mio
    execute(cpu, memory, cp(false, 0, 9, 1, 1));
    execute(cpu, memory, cp(true, 1, 9, 1, 1));
    check(cpu.state().registers[1] == 0x20, "base ITCM fixe");
    // MRC pc : NZCV seulement, pas de branchement, Q conservé.
    cpu.set_register(2, 0x88000000);
    execute(cpu, memory, 0xe128f002); // MSR cpsr_f,r2 : N/Q
    execute(cpu, memory, cp(true, 15, 0, 0));
    check(!cpu.state().negative && cpu.state().zero && !cpu.state().carry && !cpu.state().overflow &&
          cpu.state().sticky_overflow && cpu.state().registers[15] == base + 4, "MRC pc / NZCV/Q");
    for (const auto value : {0x80U}) {
        cpu.set_register(0, value);
        memory.write32(arm9, base, cp(false, 0, 1, 0));
        cpu.set_register(15, base);
        const auto before = cpu.state();
        const auto before_control = memory.cp15().read(1, 0, 0);
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "endian non émulé accepté");
        check(cpu.state() == before && memory.cp15().read(1, 0, 0) == before_control, "erreur CP15 non atomique");
    }
    for (const auto raw : {cp(false, 0, 0, 0), cp(true, 0, 8, 0), cp(true, 0, 1, 0) | 0x200000U, cp(false, 15, 1, 0)}) {
        memory.write32(arm9, base, raw);
        cpu.set_register(15, base);
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "variante CP15 absente acceptée");
        check(cpu.state() == before, "diagnostic CP15 a modifié le CPU");
    }
    for (const auto size : {0U, 2U, 24U, 31U}) {
        const auto before = memory.cp15().read(9, 1, 0);
        expect_failure<ravenemu::Error>([&] { region(memory, false, size << 1U); }, "taille réservée acceptée");
        check(memory.cp15().read(9, 1, 0) == before, "taille refusée a changé DTCM");
    }
    // Une condition fausse ne doit ni accéder à CP15 ni modifier son état.
    memory.write32(arm9, base, (cp(false, 0, 8, 0) & 0x0fffffffU) | 0x10000000U); // NE, Z=1
    cpu.set_register(15, base);
    cpu.step(memory);
    check(cpu.state().registers[15] == base + 4, "CP15 conditionnel ignoré");
}

void tcm_bus_test() {
    Memory memory;
    memory.write32(arm9, dtcm, 0x11112222);
    region(memory, false, dtcm | 0xa);
    control(memory, high | data_enable);
    check(memory.read32(arm9, dtcm) == 0 && memory.read32(arm7, dtcm) == 0x11112222,
          "DTCM privée ARM9 et initialement nulle");
    memory.write8(arm9, dtcm, 0x12);
    memory.write8(arm9, dtcm + 1, 0x34);
    memory.write16(arm9, dtcm + 2, 0x5678);
    check(memory.read32(arm9, dtcm) == 0x56783412 && memory.peek16(arm9, dtcm + 2) == 0x5678,
          "DTCM octets/demi-mots/mots little-endian");
    check(memory.fetch32(arm9, dtcm) == 0x11112222, "fetch doit ignorer DTCM");
    control(memory, high | data_enable | data_load);
    check(memory.read32(arm9, dtcm) == 0x11112222, "DTCM en chargement : lecture externe");
    memory.write32(arm9, dtcm, 0xabcdef01);
    control(memory, high | data_enable);
    check(memory.read32(arm9, dtcm) == 0xabcdef01 && memory.read32(arm7, dtcm) == 0x11112222,
          "chargement DTCM sans écrasement RAM");
    region(memory, false, 0x0202000a);
    check(memory.read32(arm9, 0x02020000) == 0xabcdef01 && memory.read32(arm9, dtcm) == 0x11112222,
          "déplacement DTCM sans perte");
    // Fenêtre virtuelle 64 Kio : quatre miroirs physiques de 16 Kio.
    region(memory, false, 0x0202000e);
    check(memory.read32(arm9, 0x0202c000) == 0xabcdef01, "miroirs DTCM");
    region(memory, false, 0x02020006); // 4 Kio seulement
    memory.write32(arm7, 0x02021000, 0x76543210);
    check(memory.read32(arm9, 0x02021000) == 0x76543210, "limite de fenêtre virtuelle DTCM");
    expect_failure<ravenemu::Error>([&] { memory.write32(arm9, 0x02020001, 1); }, "TCM non alignée acceptée");

    region(memory, true, 0x20); // ITCM 32 Mio de miroirs
    control(memory, high | code_enable | data_enable);
    memory.write32(arm9, 0, 0xe3a0002a);
    check(memory.fetch32(arm9, 0x01008000) == 0xe3a0002a && memory.read32(arm9, 0x01ff8000) == 0xe3a0002a,
          "miroirs ITCM physiques 32 Kio et fin de fenêtre");
    check(memory.read32(arm9, 0x02020000) == 0xabcdef01, "limite ITCM 32 Mio");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm7, 0x01008000)); }, "ITCM visible ARM7");
    region(memory, false, 0xa); // chevauchement à zéro
    memory.write32(arm9, 0, 0xdeadbeef);
    check(memory.read32(arm9, 0) == 0xdeadbeef, "ITCM prioritaire");
    control(memory, high | data_enable);
    check(memory.read32(arm9, 0) == 0xabcdef01, "écriture chevauchante a modifié DTCM");
    region(memory, true, 0x2e); // 4 Gio : arithmétique 64 bits et vecteurs hauts
    control(memory, high | code_enable);
    memory.write32(arm9, 0xfffffffcu, 0xcafebabe);
    check(memory.fetch32(arm9, 0x7ffc) == 0xcafebabe, "ITCM 4 Gio / dernier mot");
    // ITCM masque aussi MMIO : aucun effet FIFO caché.
    memory.write32(arm9, 0x04000188, 0x22334455);
    check(memory.read32(arm9, 0x188) == 0x22334455, "ITCM avant décodage MMIO");
    control(memory, high);
    check(memory.read16(arm7, 0x04000184) == 0x0101, "écriture ITCM a envoyé un mot IPC");
}

void fetch_execution_test() {
    Memory memory;
    Cpu cpu{arm9};
    // Chargement par mêmes adresses que la RAM, puis fetch depuis ITCM.
    memory.write32(arm9, base, 0xe3a0002a); // MOV r0,#42
    region(memory, true, 0x22); // 64 Mio, base couverte
    control(memory, high | code_enable | code_load);
    check(memory.fetch32(arm9, base) == 0xe3a0002a, "ITCM load : fetch externe");
    memory.write32(arm9, base, memory.read32(arm9, base));
    memory.write32(arm7, base, 0xe3a00007); // modifier uniquement la RAM sous-jacente
    control(memory, high | code_enable);
    cpu.reset(base, false);
    cpu.step(memory);
    check(cpu.state().registers[0] == 42, "exécution ARM depuis ITCM");
    control(memory, high | code_enable | code_load);
    cpu.reset(base, false);
    cpu.step(memory);
    check(cpu.state().registers[0] == 7, "exécution RAM pendant chargement ITCM");
    memory.write16(arm9, base, 0x2031); // Thumb MOV r0,#49, écrit ITCM
    control(memory, high | code_enable);
    cpu.reset(base, true);
    cpu.step(memory);
    check(cpu.state().registers[0] == 49, "exécution Thumb depuis ITCM");
    control(memory, high);
    memory.write32(arm9, base, 0xe590f000); // LDR pc,[r0]
    memory.write32(arm9, base + 0x100, base + 0x201);
    for (const auto legacy : {false, true}) {
        control(memory, high | (legacy ? 0x8000U : 0U));
        cpu.reset(base, false);
        cpu.set_register(0, base + 0x100);
        cpu.step(memory);
        check(cpu.state().thumb == !legacy && cpu.state().registers[15] == base + 0x200,
              "CP15 L4 / interworking LDR pc");
    }
}

void tcm_transfer_test() {
    Memory memory;
    Cpu cpu{arm9};
    cpu.reset(base, false);
    region(memory, false, dtcm | 0xa);
    control(memory, high | data_enable);
    cpu.set_register(0, dtcm);
    cpu.set_register(1, 0x12345678);
    execute(cpu, memory, 0xe5801000); // STR r1,[r0]
    execute(cpu, memory, 0xe5902000); // LDR r2,[r0]
    check(cpu.state().registers[2] == 0x12345678 && memory.read32(arm7, dtcm) == 0,
          "STR/LDR CPU utilisent DTCM");
    cpu.set_register(2, 0xabcdef01);
    execute(cpu, memory, 0xe8a00006); // STMIA r0!,{r1,r2}
    cpu.set_register(0, dtcm);
    execute(cpu, memory, 0xe8b00018); // LDMIA r0!,{r3,r4}
    check(cpu.state().registers[3] == 0x12345678 && cpu.state().registers[4] == 0xabcdef01 &&
          cpu.state().registers[0] == dtcm + 8, "LDM/STM et writeback DTCM");
    region(memory, false, 0x0100000a);
    memory.write32(arm9, 0x01003ffc, 42);
    memory.write32(arm9, base, 0xe8a00006);
    cpu.set_register(0, 0x01003ffc);
    cpu.set_register(15, base);
    const auto before = cpu.state();
    expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "STM franchit TCM vers bus non mappé");
    check(cpu.state() == before && memory.read32(arm9, 0x01003ffc) == 42,
          "STM fautif a modifié une partie de TCM");
}

void vectors_and_privilege_test() {
    Memory memory;
    Cpu cpu{arm9};
    region(memory, true, 0xc); // ITCM 32 Kio, vecteurs bas
    control(memory, code_enable);
    memory.write32(arm9, 8, 0xe1b0f00e); // SWI : MOVS pc,lr
    memory.write32(arm9, base, 0xef000000);
    cpu.reset(base, false);
    cpu.step(memory);
    check(cpu.state().registers[15] == 8 && cpu.state().mode == CpuMode::supervisor, "SWI vecteur bas CP15");
    cpu.step(memory);
    check(cpu.state().registers[15] == base + 4 && cpu.state().mode == CpuMode::system, "retour SWI ITCM");
    memory.write16(arm9, base, 0xdf00); // SWI Thumb, vecteurs bas également
    cpu.reset(base, true);
    cpu.step(memory);
    check(cpu.state().registers[15] == 8 && cpu.state().registers[14] == base + 2,
          "SWI Thumb / vecteur bas CP15");
    cpu.step(memory);
    check(cpu.state().thumb && cpu.state().registers[15] == base + 2, "retour SWI Thumb ITCM");
    for (const auto high_vectors : {false, true}) {
        control(memory, code_enable | (high_vectors ? high : 0U));
        cpu.reset(base, false);
        cpu.set_register(0, 0x1f);
        execute(cpu, memory, 0xe121f000); // autoriser IRQ/FIQ
        cpu.set_interrupt_lines(true);
        cpu.step(memory);
        check(cpu.state().registers[15] == (high_vectors ? 0xffff0018U : 0x18U), "IRQ vecteur CP15");
        cpu.reset(base, false);
        cpu.set_register(0, 0x1f);
        execute(cpu, memory, 0xe121f000);
        cpu.set_interrupt_lines(false, true);
        cpu.step(memory);
        check(cpu.state().registers[15] == (high_vectors ? 0xffff001cU : 0x1cU), "FIQ vecteur CP15");
    }
    control(memory, high);
    cpu.reset(base, false);
    cpu.set_register(0, 0x10);
    execute(cpu, memory, 0xe121f000); // User
    execute(cpu, memory, cp(false, 0, 1, 0));
    check(cpu.state().mode == CpuMode::undefined && cpu.state().registers[15] == 0xffff0004 &&
          memory.cp15().read(1, 0, 0) == high, "CP15 interdit en User / exception UND");
    Cpu other{arm7};
    other.reset(base, false);
    memory.write32(arm7, base, cp(true, 0, 0, 0));
    other.step(memory);
    check(other.state().mode == CpuMode::undefined && other.state().registers[15] == 4,
          "CP15 absent sur ARM7 / vecteur UND bas");
}

void dma_test() {
    Memory memory;
    region(memory, false, dtcm | 0xa);
    control(memory, high | data_enable);
    memory.write32(arm7, dtcm, 0x11112222);
    memory.write32(arm7, dtcm + 4, 0x33334444);
    memory.write32(arm9, dtcm, 0xaaaaaaaa);
    memory.write32(arm9, dtcm + 4, 0xbbbbbbbb);
    memory.write32(arm9, 0x040000b0, dtcm);
    memory.write32(arm9, 0x040000b4, dtcm + 4);
    memory.write32(arm9, 0x040000b8, 0x84000001);
    check(memory.step_dma(arm9), "DMA non exécuté");
    check(memory.read32(arm7, dtcm + 4) == 0x11112222 && memory.read32(arm9, dtcm + 4) == 0xbbbbbbbb,
          "DMA doit ignorer TCM source et destination");
    // Une ITCM couvrant même MMIO ne doit pas masquer la source DMAFILL.
    region(memory, true, 0x2e);
    memory.write32(arm9, 0x040000e0, 0x76543210);
    memory.write32(arm9, 0x040000b0, 0x040000e0);
    memory.write32(arm9, 0x040000b4, dtcm + 8);
    memory.write32(arm9, 0x040000b8, 0x85000001);
    control(memory, high | code_enable | data_enable);
    memory.write32(arm9, 0x040000e0, 0xffffffff); // ITCM seulement
    check(memory.step_dma(arm9) && memory.read32(arm7, dtcm + 8) == 0x76543210,
          "DMAFILL externe malgré ITCM recouvrante");
}

void machine_test() {
    std::vector<std::uint8_t> rom(0x228);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, base); put32(rom, 0x28, base); put32(rom, 0x2c, 0x20);
    put32(rom, 0x30, 0x220); put32(rom, 0x34, 0x03800000); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 8);
    put32(rom, 0x200, 0xe59f0010); // LDR r0,[pc,#16] -> DTCM config
    put32(rom, 0x204, cp(false, 0, 9, 1));
    put32(rom, 0x208, 0xe59f000c); // LDR r0,[pc,#12] -> contrôle
    put32(rom, 0x20c, cp(false, 0, 1, 0));
    put32(rom, 0x210, cp(true, 1, 1, 0));
    put32(rom, 0x214, 0xeafffffe);
    put32(rom, 0x218, dtcm | 0xa); put32(rom, 0x21c, high | data_enable);
    put32(rom, 0x220, 0xeafffffe); put32(rom, 0x224, 0);
    Machine machine;
    machine.load_cartridge(Cartridge{rom});
    machine.run_steps(arm9, 5);
    check(machine.cpu_state(arm9).registers[1] == (high | data_enable), "ROM configure CP15 via MCR/MRC");
    machine.memory().write32(arm9, dtcm, 0x12345678);
    auto copy = machine;
    copy.memory().write32(arm9, dtcm, 0x87654321);
    control(copy.memory(), 0);
    check(machine.memory().cp15().read(1, 0, 0) == (high | data_enable) &&
          machine.memory().read32(arm9, dtcm) == 0x12345678, "copie CP15/TCM indépendante");
    control(copy.memory(), high | data_enable);
    check(copy.memory().read32(arm9, dtcm) == 0x87654321, "copie stockage DTCM");
    auto invalid = rom;
    put32(invalid, 0x28, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(Cartridge{invalid}); }, "ROM invalide acceptée");
    check(machine.memory().read32(arm9, dtcm) == 0x12345678, "échec chargement a effacé TCM");
    machine.load_cartridge(Cartridge{rom});
    check(machine.memory().cp15().read(1, 0, 0) == (high | data_enable) &&
          machine.memory().cp15().read(9, 1, 0) == 0x0300000a &&
          machine.cpu_state(arm9).instructions == 0, "rechargement ne prépare pas la DTCM des piles de boot");
    region(machine.memory(), false, dtcm | 0xa);
    control(machine.memory(), high | data_enable);
    check(machine.memory().read32(arm9, dtcm) == 0, "rechargement ne vide pas TCM");
}
} // namespace

int main() {
    registers_test();
    tcm_bus_test();
    fetch_execution_test();
    tcm_transfer_test();
    vectors_and_privilege_test();
    dma_test();
    machine_test();
}
