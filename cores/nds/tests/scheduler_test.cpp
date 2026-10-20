#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t base9 = 0x02000000;
constexpr std::uint32_t base7 = 0x03800000;
constexpr std::uint32_t bad = 0xee070e15;
using Phase = SchedulerState::Phase;

namespace {
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
std::vector<std::uint8_t> image(std::initializer_list<std::uint32_t> a9 = {0xe2800001,0xeafffffd},
                                std::initializer_list<std::uint32_t> a7 = {0xe2800001,0xeafffffd}) {
    const auto offset7 = 0x200U + a9.size() * 4U;
    std::vector<std::uint8_t> bytes(offset7 + a7.size() * 4U);
    put32(bytes,0x20,0x200); put32(bytes,0x24,base9); put32(bytes,0x28,base9);
    put32(bytes,0x2c,static_cast<std::uint32_t>(a9.size() * 4U));
    put32(bytes,0x30,static_cast<std::uint32_t>(offset7)); put32(bytes,0x34,base7); put32(bytes,0x38,base7);
    put32(bytes,0x3c,static_cast<std::uint32_t>(a7.size() * 4U));
    std::size_t offset = 0x200;
    for (const auto op : a9) { put32(bytes,offset,op); offset += 4; }
    for (const auto op : a7) { put32(bytes,offset,op); offset += 4; }
    return bytes;
}
void timer(Memory& memory, Processor cpu, std::uint16_t reload = 0, bool irq = false) {
    memory.write32(cpu,0x04000100,(irq ? 0xc00000U : 0x800000U) | reload);
}

void budget_and_chunks_test() {
    Machine machine;
    expect_failure<ravenemu::Error>([&] { machine.run_cycles(0); }, "machine vide ordonnancée");
    expect_failure<ravenemu::Error>([&] { machine.run_frame(); }, "image sans cartouche exécutée");
    machine.load_cartridge(Cartridge{image()});
    timer(machine.memory(),arm9); timer(machine.memory(),arm7);
    const auto before9 = machine.cpu_state(arm9), before7 = machine.cpu_state(arm7);
    machine.run_cycles(0);
    check(machine.cpu_state(arm9) == before9 && machine.cpu_state(arm7) == before7 &&
          machine.scheduler_state() == SchedulerState{}, "budget nul a avancé machine");
    machine.run_cycles(100);
    check(arm9_clock_hz == 2U * arm7_clock_hz && arm7_clock_hz == timer_clock_hz &&
          machine.cpu_state(arm9).instructions == 200 && machine.cpu_state(arm7).instructions == 100 &&
          machine.cpu_state(arm9).registers[0] == 100 && machine.cpu_state(arm7).registers[0] == 50,
          "rapport des créneaux ARM9/ARM7");
    check(machine.scheduler_state().cycles == 100 && machine.scheduler_state().phase == Phase::arm9_first &&
          machine.memory().read16(arm9,0x04000100) == 100 && machine.memory().read16(arm7,0x04000100) == 100 &&
          machine.memory().cycles_until_frame() == LcdTiming::frame_cycles - 100, "horloge partagée timers/LCD");
    auto split = machine;
    machine.run_cycles(1000);
    for (const auto n : {0U,1U,17U,2U,0U,980U}) split.run_cycles(n);
    check(machine.scheduler_state() == split.scheduler_state() && machine.cpu_state(arm9) == split.cpu_state(arm9) &&
          machine.cpu_state(arm7) == split.cpu_state(arm7) && machine.memory().read32(arm9,0x04000100) ==
          split.memory().read32(arm9,0x04000100) && machine.memory().cycles_until_frame() == split.memory().cycles_until_frame(),
          "budget fractionné change l'exécution");
    const auto clock = machine.scheduler_state(); const auto t = machine.memory().read16(arm9,0x04000100);
    const auto unchanged9 = machine.cpu_state(arm9), unchanged7 = machine.cpu_state(arm7);
    expect_failure<std::overflow_error>([&] { machine.run_cycles(std::numeric_limits<std::uint64_t>::max()); },
                                       "budget débordant accepté");
    check(machine.scheduler_state() == clock && machine.cpu_state(arm9) == unchanged9 &&
          machine.cpu_state(arm7) == unchanged7, "budget débordant a modifié la machine");
    const auto count7 = machine.cpu_state(arm7).instructions;
    machine.run_steps(arm9,3);
    check(machine.scheduler_state() == clock && machine.memory().read16(arm9,0x04000100) == t &&
          machine.cpu_state(arm7).instructions == count7, "API de pas explicite a changé de contrat");
}

void ipc_and_shared_ram_test() {
    Machine shared;
    shared.load_cartridge(Cartridge{image({0xe59f100c,0xe5910000,0xe3500000,0x0afffffc,0xeafffffe,0x02001000},
        {0xe59f1008,0xe3a00007,0xe5810000,0xeafffffe,0x02001000})});
    shared.run_cycles(10);
    check(shared.cpu_state(arm9).registers[0] == 7 && shared.memory().read32(arm9,0x02001000) == 7,
          "ARM9 ne voit pas le message RAM d'ARM7");
    Machine ipc;
    ipc.load_cartridge(Cartridge{image({0xee070f90,0xe3a0002a,0xeafffffe},
        {0xe59f1008,0xe3a00007,0xe5810000,0xeafffffe,0x04000188})});
    ipc.memory().write16(arm9,0x04000184,0x8400); ipc.memory().write16(arm7,0x04000184,0x8000);
    ipc.memory().write32(arm9,0x04000210,1U << 18U); ipc.memory().write32(arm9,0x04000208,1);
    ipc.run_cycles(2);
    check(ipc.cpu_state(arm9).waiting_for_interrupt && ipc.cpu_state(arm7).instructions == 2,
          "WFI ARM9 bloque ARM7");
    ipc.run_cycles(1);
    check(!ipc.cpu_state(arm9).waiting_for_interrupt && ipc.cpu_state(arm9).instructions == 1 &&
          ipc.memory().peek32(arm9,0x04100000) == 7, "créneau ARM9 suivant ne voit pas IRQ IPC d'ARM7");
    ipc.run_cycles(1);
    check(ipc.cpu_state(arm9).registers[0] == 42 && ipc.cpu_state(arm9).instructions == 3,
          "reprise du programme ARM9 après message IPC");
}

void timer_wake_test() {
    Machine machine;
    machine.load_cartridge(Cartridge{image({0xe3a0001f,0xe121f000,0xee070f90,0xeafffffe},{0xeafffffe})});
    std::vector<std::uint8_t> bios(0x1000);
    put32(bios,0x18,0xe2811001); put32(bios,0x1c,0xe25ef004);
    machine.memory().load_bios(arm9,bios);
    machine.memory().write32(arm9,0x04000210,8); machine.memory().write32(arm9,0x04000208,1);
    timer(machine.memory(),arm9,0xfffb,true);
    machine.run_cycles(5);
    check(machine.cpu_state(arm9).waiting_for_interrupt && machine.cpu_state(arm9).instructions == 3 &&
          machine.memory().irq_pending(arm9), "timer n'avance pas pendant WFI");
    machine.run_cycles(1);
    check(machine.cpu_state(arm9).mode == CpuMode::irq && !machine.cpu_state(arm9).waiting_for_interrupt &&
          machine.cpu_state(arm9).registers[1] == 1 && machine.cpu_state(arm7).instructions == 6,
          "IRQ timer/gestionnaire ne sont pas exécutés par l'ordonnanceur");
    machine.memory().write32(arm9,0x04000214,8);
    machine.run_cycles(1);
    check(machine.cpu_state(arm9).mode == CpuMode::system && machine.cpu_state(arm9).registers[15] == base9 + 12 &&
          machine.cpu_state(arm9).instructions == 6, "retour IRQ après WFI");
}

void dma_and_fifo_test() {
    Machine machine; machine.load_cartridge(Cartridge{image({0xeafffffe},{0xeafffffe})});
    auto& memory = machine.memory();
    for (unsigned i = 0; i < 3; ++i) memory.write32(arm9,0x02001000 + i * 4U,0x12345678U + i);
    memory.write32(arm9,0x040000b0,0x02001000); memory.write32(arm9,0x040000b4,0x02002000);
    memory.write32(arm9,0x040000b8,0xc4000003);
    memory.write32(arm7,0x040000b0,0x02001000); memory.write32(arm7,0x040000b4,0x02003000);
    memory.write32(arm7,0x040000b8,0x84000003);
    machine.run_cycles(1);
    check(machine.cpu_state(arm9).instructions == 0 && machine.cpu_state(arm7).instructions == 0 &&
          memory.read32(arm9,0x02002004) == 0x12345679 && memory.read32(arm9,0x02003000) == 0x12345678 &&
          memory.read32(arm9,0x02003004) == 0, "quota DMA indépendant des deux CPU");
    machine.run_cycles(2);
    check(!memory.dma_pending(arm9) && !memory.dma_pending(arm7) && machine.cpu_state(arm9).instructions == 3 &&
          machine.cpu_state(arm7).instructions == 0 && (memory.read32(arm9,0x04000214) & 0x100U), "fin DMA et reprise CPU");
    machine.load_cartridge(Cartridge{image({0xeafffffe},{0xeafffffe})});
    memory.write32(arm9,0x02001000,0xfc00801f);
    memory.write32(arm9,0x040000b0,0x02001000); memory.write32(arm9,0x040000b4,0x02002000);
    memory.write32(arm9,0x040000b8,0x94000001); // HBlank ARM9
    machine.run_cycles(LcdTiming::visible_cycles);
    check(memory.dma_pending(arm9) && memory.read32(arm9,0x02002000) == 0, "DMA HBlank doit attendre le prochain créneau");
    machine.run_cycles(1);
    check(memory.read32(arm9,0x02002000) == 0xfc00801f, "DMA HBlank jamais servi");
    machine.load_cartridge(Cartridge{image({0xeafffffe},{0xeafffffe})});
    memory.write32(arm9,0x02001000,0xfc00801f);
    memory.write8(arm9,0x04000243,0x80);
    memory.write16(arm9,0x04000304,0x8001);
    memory.write32(arm9,0x04000000,0x30000);
    memory.write32(arm9,0x04000064,0xa2030000);
    memory.write32(arm9,0x040000b0,0x02001000); memory.write32(arm9,0x040000b4,0x04000068);
    memory.write32(arm9,0x040000b8,0xa7400004); // FIFO, mot, répétition, source/destination fixes
    machine.run_cycles(LcdTiming::visible_cycles);
    const auto frame = machine.render_frame();
    for (unsigned x = 0; x < 256; ++x)
        check(frame.upper[x] == ((x & 1U) ? 0x7c00 : 0x1f), "image FIFO produite par l'ordonnanceur");
    for (unsigned x = 0; x < 128; ++x)
        check(memory.read16(arm9,0x06860000 + x * 2U) == ((x & 1U) ? 0xfc00 : 0x801f), "capture du flux FIFO synchronisé");
    check(machine.cpu_state(arm9).instructions == 2U * LcdTiming::visible_cycles - 128U &&
          machine.cpu_state(arm7).instructions == LcdTiming::visible_cycles, "DMA FIFO ne suspend pas le bon nombre de créneaux");
}

void mpu_test() {
    Machine machine; machine.load_cartridge(Cartridge{image({0xeafffffe})});
    auto& memory = machine.memory();
    std::vector<std::uint8_t> bios(0x1000);
    put32(bios,0x0c,0xe3a00013); put32(bios,0x10,0xee050f70); put32(bios,0x14,0xe25ef004);
    memory.load_bios(arm9,bios);
    memory.cp15().write(6,0,0,base9 | 0x2bU); memory.cp15().write(6,1,0,0xffff0017);
    memory.cp15().write(5,0,3,0x10); memory.cp15().write(1,0,0,0x2079);
    machine.run_cycles(3);
    check(machine.cpu_state(arm9).mode == CpuMode::system && machine.cpu_state(arm9).registers[15] == base9 &&
          machine.cpu_state(arm9).instructions == 5 && machine.cpu_state(arm7).instructions == 3 &&
          memory.cp15().read(5,0,3) == 0x13, "exceptions MPU interrompent le progrès de l'autre CPU");
}

void cpu_errors_test() {
    Machine machine; machine.load_cartridge(Cartridge{image({bad})});
    expect_failure<CpuExecutionError>([&] { machine.run_cycles(1); }, "erreur premier créneau ignorée");
    check(machine.scheduler_state() == SchedulerState{} && machine.cpu_state(arm7).instructions == 0,
          "erreur initiale a avancé l'horloge");
    machine.memory().write32(arm9,base9,0xeafffffe); machine.run_cycles(1);
    check(machine.cpu_state(arm9).instructions == 2 && machine.cpu_state(arm7).instructions == 1, "reprise premier créneau");
    machine.load_cartridge(Cartridge{image({0xe2800001,0xeafffffd},{bad})});
    expect_failure<CpuExecutionError>([&] { machine.run_cycles(1); }, "erreur ARM7 ignorée");
    check(machine.scheduler_state().phase == Phase::arm7 && machine.scheduler_state().cycles == 0 &&
          machine.cpu_state(arm9).instructions == 1, "phase ARM7 non conservée");
    machine.memory().write32(arm7,base7,0xeafffffe); machine.run_cycles(1);
    check(machine.cpu_state(arm9).registers[0] == 1 && machine.cpu_state(arm9).instructions == 2 &&
          machine.cpu_state(arm7).instructions == 1, "reprise rejoue le premier créneau ARM9");
    machine.load_cartridge(Cartridge{image({0xe2800001,bad})});
    expect_failure<CpuExecutionError>([&] { machine.run_cycles(1); }, "erreur second créneau ignorée");
    check(machine.scheduler_state().phase == Phase::arm9_second && machine.cpu_state(arm7).instructions == 1,
          "phase second créneau non conservée");
    machine.run_cycles(0);
    machine.memory().write32(arm9,base9 + 4,0xeafffffd); machine.run_cycles(1);
    check(machine.cpu_state(arm9).registers[0] == 1 && machine.cpu_state(arm7).instructions == 1 &&
          machine.scheduler_state().cycles == 1, "reprise rejoue le créneau ARM7");
    // Une erreur DMA garde aussi la phase sans rejouer un CPU déjà terminé.
    machine.memory().write32(arm7,0x040000b0,0x01000000);
    machine.memory().write32(arm7,0x040000b4,0x02001000);
    machine.memory().write32(arm7,0x040000b8,0x84000001);
    expect_failure<DmaTransferError>([&] { machine.run_cycles(1); }, "DMA non mappé accepté");
    check(machine.scheduler_state().phase == Phase::arm7 && machine.scheduler_state().cycles == 1,
          "erreur DMA perd sa phase");
    const auto n9 = machine.cpu_state(arm9).instructions;
    machine.memory().write32(arm7,0x040000b8,0); machine.run_cycles(1);
    check(machine.cpu_state(arm9).instructions == n9 + 1, "reprise après DMA rejoue ARM9");
}

void peripheral_error_test() {
    Machine machine; machine.load_cartridge(Cartridge{image({0xeafffffe},{0xeafffffe})});
    auto& memory = machine.memory();
    memory.write8(arm9,0x04000240,0x80); memory.write32(arm9,0x04000064,0x81000000);
    machine.run_cycles(LcdTiming::visible_cycles - 1);
    expect_failure<ravenemu::Error>([&] { machine.run_cycles(1); }, "capture 3D ignorée");
    const auto n9 = machine.cpu_state(arm9).instructions, n7 = machine.cpu_state(arm7).instructions;
    check(machine.scheduler_state().phase == Phase::peripherals && machine.scheduler_state().cycles == 1535 &&
          memory.cycles_until_frame() == LcdTiming::frame_cycles - 1535, "phase périphériques non conservée");
    memory.write32(arm9,0x04000064,0x80000000); machine.run_cycles(1);
    check(machine.scheduler_state().cycles == 1536 && machine.cpu_state(arm9).instructions == n9 &&
          machine.cpu_state(arm7).instructions == n7 && memory.read16(arm9,0x06800000) == 0xffff,
          "reprise périphériques rejoue les CPU ou oublie la capture");
}

void copy_reset_and_frame_test() {
    const auto rom = image({0xee070f90,0xe3a0002a,0xeafffffe},{bad});
    Machine machine; machine.load_cartridge(Cartridge{rom});
    expect_failure<CpuExecutionError>([&] { machine.run_cycles(1); }, "ARM7 fautif accepté");
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    const auto initial = machine.scheduler_state();
    const auto asleep = machine.cpu_state(arm9);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(Cartridge{invalid}); }, "ROM invalide acceptée");
    check(machine.scheduler_state() == initial && machine.cpu_state(arm9) == asleep, "échec chargement efface attente/phase");
    auto copy = machine;
    for (auto* m : {&machine,&copy}) m->memory().write32(arm7,base7,0xeafffffe);
    copy.set_interrupt_lines(arm9,true);
    machine.run_cycles(1); copy.run_cycles(1);
    check(machine.cpu_state(arm9).waiting_for_interrupt && !copy.cpu_state(arm9).waiting_for_interrupt &&
          machine.scheduler_state().cycles == 1 && copy.scheduler_state().cycles == 1, "copies partagent attente ou phase");
    copy.run_cycles(1);
    check(copy.cpu_state(arm9).registers[0] == 42 && machine.cpu_state(arm9).registers[0] == 0, "copie du CPU en attente");
    machine.load_cartridge(Cartridge{image({0xeafffffe},{0xeafffffe})});
    check(machine.scheduler_state() == SchedulerState{} && !machine.cpu_state(arm9).waiting_for_interrupt,
          "chargement ne réinitialise pas l'ordonnanceur");
    machine.advance_cycles(LcdTiming::frame_cycles - 3); // outils explicites restent sans CPU
    check(machine.scheduler_state().cycles == 0 && machine.cpu_state(arm9).instructions == 0, "avance manuelle exécute CPU");
    machine.run_frame();
    check(machine.scheduler_state().cycles == 3 && machine.memory().cycles_until_frame() == LcdTiming::frame_cycles,
          "run_frame n'utilise pas la phase LCD réelle");
    machine.run_frame();
    check(machine.scheduler_state().cycles == LcdTiming::frame_cycles + 3U &&
          machine.cpu_state(arm9).instructions == 2U * (LcdTiming::frame_cycles + 3U) &&
          machine.cpu_state(arm7).instructions == LcdTiming::frame_cycles + 3U &&
          machine.memory().cycles_until_frame() == LcdTiming::frame_cycles, "run_frame à phase zéro doit exécuter une image entière");
}
} // namespace

int main() {
    budget_and_chunks_test();
    ipc_and_shared_ram_test();
    timer_wake_test();
    dma_and_fifo_test();
    mpu_test();
    cpu_errors_test();
    peripheral_error_test();
    copy_reset_and_frame_test();
}
