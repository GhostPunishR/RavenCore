#include <ravenemu/nds/cpu.hpp>
#include "check.hpp"

#include <vector>

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t base = 0x02000000;
constexpr std::uint32_t wfi = 0xee070f90;
constexpr std::uint32_t alternate_wfi = 0xee070f58;

namespace {
void masked_wake_test(std::uint32_t opcode) {
    Memory memory;
    Cpu cpu{arm9}; cpu.reset(base, false);
    memory.write32(arm9, base, opcode);
    memory.write32(arm9, base + 4, 0xe3a0102a);
    cpu.step(memory);
    check(cpu.state().waiting_for_interrupt && cpu.state().registers[15] == base + 4 &&
          cpu.state().instructions == 1, "WFI doit terminer puis suspendre les instructions");
    const auto asleep = cpu.state();
    for (unsigned i = 0; i < 10; ++i) cpu.step(memory);
    check(cpu.state() == asleep, "CPU suspendu modifié sans interruption");
    memory.request_interrupt(arm9, 8);
    memory.write32(arm9, 0x04000210, 8);
    cpu.step(memory);
    check(cpu.state() == asleep, "ARM9 WFI doit vérifier IME");
    memory.write32(arm9, 0x04000208, 1);
    memory.write32(arm9, 0x04000210, 0);
    cpu.step(memory);
    check(cpu.state() == asleep, "IF seul sans IE réveille WFI");
    memory.write32(arm9, 0x04000210, 8);
    cpu.step(memory);
    check(!cpu.state().waiting_for_interrupt && cpu.state().irq_disabled && cpu.state().mode == CpuMode::system &&
          cpu.state().registers[15] == base + 4 && cpu.state().instructions == 1, "réveil malgré CPSR.I, sans IRQ ni instruction");
    memory.write32(arm9, 0x04000214, 8);
    cpu.step(memory);
    check(cpu.state().registers[1] == 42 && cpu.state().instructions == 2, "reprise après WFI");
    cpu.reset(base, false); cpu.step(memory); cpu.reset(base, false);
    check(!cpu.state().waiting_for_interrupt, "reset conserve WFI");
}

void exceptions_test() {
    Memory memory;
    Cpu cpu{arm9}; cpu.reset(base, false);
    cpu.set_register(0, 0x1f);
    memory.write32(arm9, base, 0xe121f000); // autoriser IRQ/FIQ
    memory.write32(arm9, base + 4, wfi);
    cpu.step(memory); cpu.step(memory);
    const auto status = cpu.state().cpsr();
    cpu.set_interrupt_lines(true);
    cpu.step(memory);
    check(!cpu.state().waiting_for_interrupt && cpu.state().mode == CpuMode::irq &&
          cpu.state().registers[15] == 0xffff0018 && cpu.state().registers[14] == base + 12 &&
          cpu.state().spsr() == status && cpu.state().instructions == 2, "IRQ après WFI / LR et SPSR");
    cpu.reset(base, false); cpu.set_register(0, 0x1f); cpu.step(memory); cpu.step(memory);
    cpu.set_interrupt_lines(true, true); cpu.step(memory);
    check(cpu.state().mode == CpuMode::fiq && !cpu.state().waiting_for_interrupt &&
          cpu.state().registers[15] == 0xffff001c, "priorité FIQ au réveil WFI");
    cpu.reset(base + 4, false); cpu.step(memory);
    cpu.set_interrupt_lines(false, true); cpu.step(memory);
    check(cpu.state().mode == CpuMode::system && !cpu.state().waiting_for_interrupt,
          "FIQ physique masquée ne réveille pas WFI");
}

void decode_and_privilege_test() {
    Memory memory;
    Cpu cpu{arm9}; cpu.reset(base, false);
    memory.write32(arm9, base, wfi & 0x0fffffffU); // EQ, Z initial faux
    cpu.step(memory);
    check(!cpu.state().waiting_for_interrupt && cpu.state().instructions == 1, "WFI conditionnelle ignorée suspend le CPU");
    memory.write32(arm9, base, 0xe121f000); // User
    memory.write32(arm9, base + 4, wfi);
    cpu.reset(base, false); cpu.set_register(0, 0x10); cpu.step(memory); cpu.step(memory);
    check(cpu.state().mode == CpuMode::undefined && !cpu.state().waiting_for_interrupt &&
          cpu.state().registers[15] == 0xffff0004, "WFI accessible en User");
    memory.write32(arm7, base, alternate_wfi);
    Cpu other{arm7}; other.reset(base, false); other.step(memory);
    check(other.state().mode == CpuMode::undefined && !other.state().waiting_for_interrupt &&
          other.state().registers[15] == 4, "WFI CP15 accessible sur ARM7");
    for (const auto raw : {wfi | 0x100000U, wfi | 0x200000U, wfi | 0xf000U}) {
        memory.write32(arm9, base, raw); cpu.reset(base, false);
        const auto before = cpu.state();
        expect_failure<CpuExecutionError>([&] { cpu.step(memory); }, "variante WFI non modélisée acceptée");
        check(cpu.state() == before, "variante WFI refusée a modifié CPU");
    }
}

void dma_and_mpu_test() {
    Memory memory;
    Cpu cpu{arm9}; cpu.reset(base, false);
    memory.write32(arm9, base, wfi); cpu.step(memory);
    memory.write32(arm9, 0x02001000, 0xdeadbeef);
    memory.write32(arm9, 0x040000b0, 0x02001000);
    memory.write32(arm9, 0x040000b4, 0x02002000);
    memory.write32(arm9, 0x040000b8, 0xc4000001);
    memory.write32(arm9, 0x04000210, 0x100); memory.write32(arm9, 0x04000208, 1);
    cpu.step(memory);
    check(cpu.state().waiting_for_interrupt && cpu.state().instructions == 1 &&
          memory.read32(arm9, 0x02002000) == 0xdeadbeef, "DMA ne fonctionne pas pendant WFI");
    cpu.step(memory);
    check(!cpu.state().waiting_for_interrupt && cpu.state().instructions == 1, "IRQ DMA ne réveille pas WFI");
    memory.write32(arm9, base, wfi); cpu.reset(base, false); cpu.step(memory);
    memory.write32(arm9, 0x04000214, 0x100);
    memory.cp15().write(1, 0, 0, 0x2079); // MPU sans région : tout interdit
    const auto asleep = cpu.state(); cpu.step(memory);
    check(cpu.state() == asleep, "CPU en WFI consulte la MPU ou lit une instruction");
    cpu.set_interrupt_lines(true); cpu.step(memory); // réveil masqué, pas de fetch
    check(!cpu.state().waiting_for_interrupt && cpu.state().mode == CpuMode::system, "réveil WFI doit précéder le fetch");
    cpu.step(memory);
    check(cpu.state().mode == CpuMode::abort && cpu.state().registers[15] == 0xffff000c,
          "permissions MPU oubliées après WFI");
}
} // namespace

int main() {
    masked_wake_test(wfi);
    masked_wake_test(alternate_wfi);
    exceptions_test();
    decode_and_privilege_test();
    dma_and_mpu_test();
}
