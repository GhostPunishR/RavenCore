#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <vector>

using ravenemu::nds::Cpu;
using ravenemu::nds::CpuMode;
using ravenemu::nds::DmaController;
using ravenemu::nds::DmaTransferError;
using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t dma0 = 0x040000b0;
constexpr std::uint32_t flags = 0x04000214;
constexpr std::uint32_t source = 0x02001000;
constexpr std::uint32_t destination = 0x02002000;
constexpr std::uint32_t enabled = 0x80000000;
constexpr std::uint32_t irq = 0x40000000;
constexpr std::uint32_t word = 0x04000000;

namespace {

void configure(Memory& memory, Processor cpu, unsigned channel, std::uint32_t src, std::uint32_t dst, std::uint32_t control) {
    const std::array<std::uint32_t, 3> registers{src, dst, control};
    memory.write_words(cpu, dma0 + channel * 12, registers);
}

void put_register(DmaController& dma, unsigned offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) dma.write_byte(offset + i, static_cast<std::uint8_t>(value >> (i * 8U)));
}

void register_test(Processor cpu) {
    Memory memory;
    for (unsigned channel = 0; channel < 4; ++channel) {
        const auto address = dma0 + channel * 12;
        const auto source_mask = cpu == arm7 && channel == 0 ? 0x07fffffeU : 0x0ffffffeU;
        const auto destination_mask = cpu == arm7 && channel != 3 ? 0x07fffffeU : 0x0ffffffeU;
        const auto count_mask = cpu == arm9 ? 0x1fffffU : (channel == 3 ? 0xffffU : 0x3fffU);
        check(memory.read32(cpu, address) == 0 && memory.read32(cpu, address + 4) == 0 &&
              memory.read32(cpu, address + 8) == 0, "registres DMA initiaux");
        memory.write32(cpu, address, 0xffffffff);
        memory.write32(cpu, address + 4, 0xffffffff);
        memory.write32(cpu, address + 8, 0x7fffffff); // sans enable
        check(memory.read32(cpu, address) == source_mask && memory.read32(cpu, address + 4) == destination_mask &&
              memory.read32(cpu, address + 8) == (cpu == arm9 ? 0x7fffffffU : 0x77e00000U | count_mask),
              "masques SAD/DAD/CNT propres au processeur et au canal");
        memory.write8(cpu, address, 0x35);
        memory.write16(cpu, address + 2, 0x0200);
        check(memory.read32(cpu, address) == 0x0200ff34, "voies d'octets et demi-mots SAD");
        DmaController dma{cpu};
        put_register(dma, channel * 12, source + 3);
        put_register(dma, channel * 12 + 4, destination + 3);
        put_register(dma, channel * 12 + 8, enabled | word); // count zéro = longueur maximale
        const auto transfer = dma.next();
        check(transfer && transfer->channel == channel && transfer->remaining == count_mask + 1 &&
              transfer->source == source && transfer->destination == destination, "compte zéro ou alignement interne 32 bits");
        dma.reset();
        check(!dma.next() && dma.read_byte(channel * 12 + 11) == 0, "reset DMA conserve un canal");
        dma.write_byte(0x30, 0xff);
        check(dma.read_byte(0x30) == 0 && !dma.next(), "offset hors plage du contrôleur");
    }
}

void transfer_test(Processor cpu) {
    for (unsigned channel = 0; channel < 4; ++channel) for (const auto width : {2U, 4U}) {
        Memory memory;
        for (unsigned i = 0; i < 3; ++i) {
            if (width == 4) memory.write32(cpu, source + i * width, 0x12345670 + i);
            else memory.write16(cpu, source + i * width, static_cast<std::uint16_t>(0xabc0 + i));
        }
        const auto control = enabled | irq | (width == 4 ? word : 0U) | 3U;
        configure(memory, cpu, channel, source, destination, control);
        check(memory.dma_pending(cpu) && memory.read32(cpu, destination) == 0, "DMA doit attendre son premier pas");
        for (unsigned i = 0; i < 3; ++i) {
            check(memory.step_dma(cpu), "unité DMA en attente non traitée");
            const auto value = width == 4 ? memory.read32(cpu, destination + i * width) : memory.read16(cpu, destination + i * width);
            check(value == (width == 4 ? 0x12345670 + i : 0xabc0 + i), "copie DMA 16/32 bits");
            check(memory.read32(cpu, flags) == (i == 2 ? 1U << (8U + channel) : 0U), "IRQ DMA avant/après la dernière unité");
        }
        check(!memory.dma_pending(cpu) && !memory.step_dma(cpu) && !memory.irq_pending(cpu), "DMA fini reste actif ou contourne IME/IE");
        check(memory.read32(cpu, dma0 + channel * 12) == source && memory.read32(cpu, dma0 + channel * 12 + 4) == destination &&
              memory.read32(cpu, dma0 + channel * 12 + 8) == (control & ~enabled), "registres programmés doivent rester stables, sauf enable");
        memory.write32(cpu, 0x04000210, 1U << (8U + channel));
        memory.write32(cpu, 0x04000208, 1);
        check(memory.irq_pending(cpu), "IRQ de fin DMA perdue lorsque masquée");
    }
}

void address_modes_test(Processor cpu) {
    for (const auto width : {2U, 4U}) for (unsigned src_mode = 0; src_mode < 3; ++src_mode)
        for (unsigned dst_mode = 0; dst_mode < 4; ++dst_mode) {
            Memory memory;
            for (unsigned i = 0; i < 3; ++i) {
                if (width == 4) memory.write32(cpu, source + i * width, 0x10000010 + i);
                else memory.write16(cpu, source + i * width, static_cast<std::uint16_t>(0x10 + i));
            }
            const auto src = source + (src_mode == 1 ? 2 * width : 0);
            const auto dst = destination + (dst_mode == 1 ? 2 * width : 0);
            configure(memory, cpu, 0, src, dst, enabled | (width == 4 ? word : 0U) | (src_mode << 23U) | (dst_mode << 21U) | 3U);
            for (unsigned i = 0; i < 3; ++i) check(memory.step_dma(cpu), "DMA adressé absent");
            for (unsigned i = 0; i < 3; ++i) {
                if (dst_mode == 2 && i != 2) continue; // seul le dernier stockage demeure
                const auto input_index = src_mode == 0 ? i : (src_mode == 1 ? 2 - i : 0);
                const auto output_address = dst_mode == 1 ? dst - i * width : (dst_mode == 2 ? dst : dst + i * width);
                const auto actual = width == 4 ? memory.read32(cpu, output_address) : memory.read16(cpu, output_address);
                check(actual == (width == 4 ? 0x10000010U : 0x10U) + input_index, "incrément/décrément/fixe/reload DMA");
            }
        }
    Memory memory;
    for (unsigned i = 0; i < 4; ++i) memory.write32(cpu, source + i * 4, i + 1);
    configure(memory, cpu, 0, source, source + 4, enabled | word | 3);
    for (unsigned i = 0; i < 3; ++i) memory.step_dma(cpu);
    for (unsigned i = 0; i < 4; ++i) check(memory.read32(cpu, source + i * 4) == 1, "copie chevauchante doit lire la RAM à chaque unité");
}

void priority_and_latching_test(Processor cpu) {
    Memory memory;
    for (unsigned i = 0; i < 3; ++i) memory.write32(cpu, source + 4 * i, 10 + i);
    configure(memory, cpu, 3, source, destination + 0x30, enabled | word | 3);
    memory.step_dma(cpu);
    configure(memory, cpu, 1, source, destination + 0x10, enabled | word | 2);
    configure(memory, cpu, 0, source, destination, enabled | word | 1);
    memory.step_dma(cpu);
    check(memory.read32(cpu, destination) == 10 && memory.read32(cpu, destination + 0x10) == 0 &&
          memory.read32(cpu, destination + 0x34) == 0, "DMA0 doit préempter DMA1 et DMA3");
    memory.step_dma(cpu); memory.step_dma(cpu);
    check(memory.read32(cpu, destination + 0x14) == 11 && memory.read32(cpu, destination + 0x34) == 0, "priorité DMA1 sur DMA3");
    memory.step_dma(cpu); memory.step_dma(cpu);
    check(memory.read32(cpu, destination + 0x38) == 12 && !memory.dma_pending(cpu), "reprise du canal DMA3 suspendu");
    configure(memory, cpu, 0, source, destination, enabled | word | 0x02000000U | 3); // repeat en immédiat
    memory.step_dma(cpu);
    configure(memory, cpu, 0, source + 4, destination + 0x40, enabled | word | 2); // registres futurs, pas de front enable
    memory.step_dma(cpu); memory.step_dma(cpu);
    check(memory.read32(cpu, destination + 8) == 12 && memory.read32(cpu, destination + 0x40) == 0 &&
          !memory.dma_pending(cpu), "écritures actives ont rechargé les pointeurs ou repeat a bouclé en immédiat");
    memory.write32(cpu, dma0 + 8, enabled | word | 2);
    memory.step_dma(cpu); memory.step_dma(cpu);
    check(memory.read32(cpu, destination + 0x40) == 11 && memory.read32(cpu, destination + 0x44) == 12,
          "nouveau front enable n'utilise pas les registres programmés");
    configure(memory, cpu, 0, source, destination, enabled | irq | word | 3);
    memory.step_dma(cpu);
    memory.write8(cpu, dma0 + 11, 0);
    check(!memory.dma_pending(cpu) && memory.read32(cpu, flags) == 0, "annulation DMA doit arrêter sans IRQ de fin");
}

void deferred_and_fill_test(Processor cpu) {
    Memory memory;
    const auto timings = cpu == arm9 ? 7U : 3U;
    const auto shift = cpu == arm9 ? 27U : 28U;
    for (unsigned timing = 1; timing <= timings; ++timing) {
        memory.write32(cpu, dma0 + 8, 0);
        configure(memory, cpu, 0, source, destination, enabled | word | (timing << shift) | 1);
        check(!memory.dma_pending(cpu) && !memory.step_dma(cpu) && (memory.read32(cpu, dma0 + 8) & enabled),
              "un DMA différé ne doit pas être exécuté en immédiat");
    }
    if (cpu == arm7) {
        memory.write32(cpu, dma0 + 8, 0);
        memory.write32(cpu, source, 0xaabbccdd);
        configure(memory, cpu, 0, source, destination, enabled | word | 0x08000000U | 1); // bit 27 réservé
        check(memory.step_dma(cpu) && memory.read32(cpu, destination) == 0xaabbccdd, "bit 27 ARM7 doit être ignoré");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(cpu, 0x040000e0)); }, "DMAFILL ARM7 accessible");
        return;
    }
    memory.clear_ram();
    for (unsigned channel = 0; channel < 4; ++channel) {
        const auto fill = 0x040000e0 + channel * 4;
        memory.write32(cpu, fill, 0x12340000 + channel);
        memory.write8(cpu, fill, static_cast<std::uint8_t>(0x50 + channel));
        memory.write16(cpu, fill + 2, 0xabcd);
        configure(memory, cpu, channel, fill, destination + channel * 16, enabled | word | 0x01000000U | 3);
        for (unsigned i = 0; i < 3; ++i) memory.step_dma(cpu);
        for (unsigned i = 0; i < 3; ++i)
            check(memory.read32(cpu, destination + channel * 16 + i * 4) == 0xabcd0050 + channel, "remplissage depuis DMAFILL");
    }
    configure(memory, cpu, 0, 0x040000e2, destination, enabled | 0x01000000U | 3);
    for (unsigned i = 0; i < 3; ++i) memory.step_dma(cpu);
    check(memory.read16(cpu, destination) == 0xabcd && memory.read16(cpu, destination + 4) == 0xabcd, "demi-mot haut DMAFILL");
}

void faults_test(Processor cpu) {
    Memory memory;
    memory.write32(cpu, source, 0x12345678);
    memory.write32(cpu, destination, 0xaaaaaaaa);
    for (const auto src : {0U, 0x04000184U, 0x04100000U}) {
        memory.write32(cpu, dma0 + 8, 0);
        configure(memory, cpu, 0, src, destination, enabled | irq | word | 1);
        try {
            memory.step_dma(cpu);
            check(false, "source DMA non prise en charge acceptée");
        } catch (const DmaTransferError& error) {
            check(error.processor() == cpu && error.transfer().channel == 0 && error.transfer().source == src &&
                  error.transfer().destination == destination && error.transfer().remaining == 1, "diagnostic DMA incomplet");
        }
        check(memory.read32(cpu, destination) == 0xaaaaaaaa && memory.read32(cpu, flags) == 0 && memory.dma_pending(cpu),
              "échec DMA a modifié destination, demande ou progression");
    }
    memory.write32(cpu, dma0 + 8, 0);
    configure(memory, cpu, 0, source, 0x04000188, enabled | word | 1); // FIFO exclue de ce jalon
    expect_failure<DmaTransferError>([&] { memory.step_dma(cpu); }, "destination DMA MMIO acceptée");
    check(memory.peek32(cpu == arm9 ? arm7 : arm9, 0x04100000) == 0, "échec DMA a envoyé un message IPC");
    memory.write32(cpu, dma0 + 8, 0);
    configure(memory, cpu, 0, source, destination, enabled | word | (3U << 23U) | 1);
    expect_failure<DmaTransferError>([&] { memory.step_dma(cpu); }, "mode source réservé accepté");
    memory.write32(cpu, dma0 + 8, 0);
    memory.set_wram_control(3);
    const auto end = cpu == arm9 ? 0x02fffffcU : 0x03fffffcU;
    configure(memory, cpu, 0, source, end, enabled | irq | word | 2);
    memory.step_dma(cpu);
    expect_failure<DmaTransferError>([&] { memory.step_dma(cpu); }, "DMA franchit une région inaccessible");
    check(memory.read32(cpu, end) == 0x12345678 && memory.read32(cpu, flags) == 0 && memory.dma_pending(cpu),
          "unité précédente perdue ou IRQ prématurée après erreur");
    memory.clear_ram();
    std::array<std::uint32_t, 17> words{}; // b0..f0, dernier mot non mappé
    words[0] = source; words[1] = destination; words[2] = enabled | word | 1;
    expect_failure<ravenemu::Error>([&] { memory.write_words(cpu, dma0, words); }, "configuration multiple DMA partielle acceptée");
    check(!memory.dma_pending(cpu) && memory.read32(cpu, dma0) == 0 && memory.read32(cpu, dma0 + 8) == 0,
          "écriture multiple refusée a armé un DMA");
}

void cpu_stall_test() {
    Memory memory;
    Cpu cpu9{arm9}, cpu7{arm7};
    cpu9.reset(0x02000000, false); cpu7.reset(0x03800000, false);
    memory.write32(arm9, 0x02000000, 0xe321f01f); // MSR cpsr_c,#0x1f
    memory.write32(arm7, 0x03800000, 0xe2877001); // ADD r7,r7,#1
    cpu9.step(memory);
    memory.write32(arm9, source, 0x11223344);
    memory.write32(arm9, source + 4, 0xaabbccdd);
    configure(memory, arm9, 0, source, destination, enabled | word | 2);
    memory.write32(arm9, 0x04000210, 0x100);
    memory.write32(arm9, 0x04000208, 1);
    memory.request_interrupt(arm9, 0x100);
    const auto before = cpu9.state();
    cpu9.step(memory);
    check(cpu9.state() == before && memory.dma_pending(arm9), "DMA doit suspendre même un CPU avec IRQ en attente");
    cpu7.step(memory);
    check(cpu7.state().registers[7] == 1 && memory.dma_pending(arm9), "DMA ARM9 ne doit pas suspendre ARM7");
    cpu9.step(memory);
    check(cpu9.state() == before && !memory.dma_pending(arm9), "dernière unité DMA doit consommer son propre pas");
    cpu9.step(memory);
    check(cpu9.state().mode == CpuMode::irq && cpu9.state().instructions == before.instructions, "IRQ doit entrer après le DMA");
}

void copy_reset_test(Processor cpu) {
    Memory memory;
    memory.write32(cpu, source, 11); memory.write32(cpu, source + 4, 22);
    configure(memory, cpu, 0, source, destination, enabled | irq | word | 2);
    memory.step_dma(cpu);
    auto copy = memory;
    copy.step_dma(cpu);
    check(memory.dma_pending(cpu) && !copy.dma_pending(cpu) && memory.read32(cpu, destination + 4) == 0 &&
          copy.read32(cpu, destination + 4) == 22 && memory.read32(cpu, flags) == 0, "copie de bus partage canal ou IRQ DMA");
    memory.clear_ram();
    check(!memory.dma_pending(cpu) && !memory.step_dma(cpu) && memory.read32(cpu, dma0 + 8) == 0, "reset DMA incomplet");
    if (cpu == arm9) check(memory.read32(cpu, 0x040000e0) == 0, "reset conserve DMAFILL");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

void machine_program_test() {
    std::vector<std::uint8_t> rom(0x320);
    put32(rom, 0x20, 0x200); put32(rom, 0x24, 0x02000000); put32(rom, 0x28, 0x02000000); put32(rom, 0x2c, 0x90);
    put32(rom, 0x30, 0x290); put32(rom, 0x34, 0x03800000); put32(rom, 0x38, 0x03800000); put32(rom, 0x3c, 0x90);
    for (const auto offset : {0x200U, 0x290U}) {
        const auto base = offset == 0x200 ? 0x02000000U : 0x03800000U;
        put32(rom, offset,      0xe59f0058); // LDR r0,[pc,#88] -> +0x60, IME
        put32(rom, offset + 4,  0xe59f1058); // LDR r1,[pc,#88] -> +0x64, IRQ DMA0
        put32(rom, offset + 8,  0xe5801008); // STR r1,[r0,#8] : IE
        put32(rom, offset + 12, 0xe3a01001); // MOV r1,#1
        put32(rom, offset + 16, 0xe5801000); // STR r1,[r0] : IME
        put32(rom, offset + 20, 0xe321f01f); // MSR cpsr_c,#0x1f
        put32(rom, offset + 24, 0xe59f004c); // LDR r0,[pc,#76] -> +0x6c, DMA0
        put32(rom, offset + 28, 0xe59f104c); // LDR r1,[pc,#76] -> +0x70, source
        put32(rom, offset + 32, 0xe5801000); // STR r1,[r0] : SAD
        put32(rom, offset + 36, 0xe59f1048); // LDR r1,[pc,#72] -> +0x74, destination
        put32(rom, offset + 40, 0xe5801004); // STR r1,[r0,#4] : DAD
        put32(rom, offset + 44, 0xe59f1044); // LDR r1,[pc,#68] -> +0x78, contrôle
        put32(rom, offset + 48, 0xe5801008); // STR r1,[r0,#8] : démarrer DMA
        put32(rom, offset + 52, 0xe2844001); // ADD r4,r4,#1 après retour IRQ
        put32(rom, offset + 56, 0xeafffffe); // B .
        put32(rom, offset + 0x60, 0x04000208); put32(rom, offset + 0x64, 0x100);
        put32(rom, offset + 0x6c, dma0); put32(rom, offset + 0x70, base + 0x80);
        put32(rom, offset + 0x74, base + 0x100); put32(rom, offset + 0x78, enabled | irq | word | 3);
        put32(rom, offset + 0x80, 0x11223344); put32(rom, offset + 0x84, 0xabcdef01); put32(rom, offset + 0x88, 0x76543210);
    }
    ravenemu::nds::Machine machine;
    for (const auto cpu : {arm9, arm7}) {
        std::vector<std::uint8_t> bios(cpu == arm9 ? 0x1000 : 0x4000);
        put32(bios, 0x18, 0xea000008); // B +0x40
        put32(bios, 0x40, 0xe59f0018); // LDR r0,[pc,#24] -> +0x60, IF
        put32(bios, 0x44, 0xe59f1018); // LDR r1,[pc,#24] -> +0x64, DMA0
        put32(bios, 0x48, 0xe5801000); // STR r1,[r0] : acquitter
        put32(bios, 0x4c, 0xe2833001); // ADD r3,r3,#1
        put32(bios, 0x50, 0xe25ef004); // SUBS pc,lr,#4
        put32(bios, 0x60, flags); put32(bios, 0x64, 0x100);
        machine.memory().load_bios(cpu, bios);
    }
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7}) machine.run_steps(cpu, 13);
    auto copy = machine;
    copy.step(arm9);
    check(copy.memory().read32(arm9, 0x02000100) == 0x11223344 && machine.memory().read32(arm9, 0x02000100) == 0,
          "copie de machine partage les DMA");
    for (const auto cpu : {arm9, arm7}) {
        const auto base = cpu == arm9 ? 0x02000000U : 0x03800000U;
        const auto before = machine.cpu_state(cpu);
        machine.run_steps(cpu, 2);
        check(machine.cpu_state(cpu) == before && machine.memory().read32(cpu, base + 0x104) == 0xabcdef01 &&
              machine.memory().read32(cpu, base + 0x108) == 0, "budget borné des pas DMA");
        machine.step(cpu);
        check(machine.cpu_state(cpu) == before && machine.memory().read32(cpu, base + 0x108) == 0x76543210 &&
              machine.memory().irq_pending(cpu), "fin DMA : copie complète et IRQ réelle");
        machine.step(cpu);
        check(machine.cpu_state(cpu).mode == CpuMode::irq, "entrée IRQ DMA");
        machine.run_steps(cpu, 6);
        machine.step(cpu);
        check(machine.cpu_state(cpu).mode == CpuMode::system && machine.cpu_state(cpu).registers[3] == 1 &&
              machine.cpu_state(cpu).registers[4] == 1 && !machine.memory().irq_pending(cpu), "gestionnaire IRQ DMA et reprise ARM");
    }
    machine.memory().write32(arm9, dma0 + 8, enabled | word | 3);
    auto invalid = rom;
    put32(invalid, 0x34, 0x04000000); put32(invalid, 0x38, 0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { machine.load_cartridge(ravenemu::nds::Cartridge{invalid}); }, "chargement invalide accepté");
    check(machine.memory().dma_pending(arm9), "chargement refusé perd le DMA en attente");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    for (const auto cpu : {arm9, arm7})
        check(!machine.memory().dma_pending(cpu) && machine.memory().read32(cpu, dma0 + 8) == 0, "chargement conserve le DMA précédent");
}

} // namespace

int main() {
    for (const auto cpu : {arm9, arm7}) {
        register_test(cpu);
        transfer_test(cpu);
        address_modes_test(cpu);
        priority_and_latching_test(cpu);
        deferred_and_fill_test(cpu);
        faults_test(cpu);
        copy_reset_test(cpu);
    }
    cpu_stall_test();
    machine_program_test();
}
