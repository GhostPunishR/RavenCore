#include <ravenemu/nds/machine.hpp>
#include <ravenemu/core.hpp>

#include <utility>

namespace ravenemu::nds {
namespace {

bool within(const Executable& image, std::uint32_t base, std::uint32_t size) {
    return image.load_address >= base && image.load_address - base < size &&
           image.size <= size - (image.load_address - base);
}

void validate(const Executable& image, bool arm7, std::span<const std::uint8_t> bytes) {
    // Limiter ce premier chargeur aux fenêtres canoniques, sans franchir de
    // miroir ni écraser un programme par alias physique.
    if (bytes.empty() || bytes.size() != image.size ||
        !(within(image, 0x02000000, 0x400000) ||
          (arm7 && (within(image, 0x03000000, 0x8000) || within(image, 0x03800000, 0x10000)))))
        throw RomLoadError("NDS : destination de chargement RAM non prise en charge");
    const auto alignment = (image.entry_address & 1U) != 0 ? 2U : 4U;
    const auto entry = image.entry_address & ~std::uint32_t{1};
    if (image.load_address % alignment != 0 || entry % alignment != 0 ||
        entry < image.load_address || entry - image.load_address >= image.size ||
        image.size - (entry - image.load_address) < alignment)
        throw RomLoadError("NDS : entrée non alignée ou instruction tronquée");
}

ProgramEntry entry(const Executable& image) {
    return {image.entry_address & ~std::uint32_t{1}, (image.entry_address & 1U) != 0};
}

} // namespace

void Machine::load_cartridge(const Cartridge& cartridge) {
    const auto& header = cartridge.header();
    const auto arm9 = cartridge.arm9_image();
    const auto arm7 = cartridge.arm7_image();
    validate(header.arm9, false, arm9);
    validate(header.arm7, true, arm7);
    // Les plages validées ne débordent pas : l'addition est ici sûre.
    if (header.arm9.load_address < header.arm7.load_address + header.arm7.size &&
        header.arm7.load_address < header.arm9.load_address + header.arm9.size)
        throw RomLoadError("NDS : les exécutables se chevauchent en RAM");

    Memory prepared = memory_;
    prepared.clear_ram();
    prepared.set_wram_control(3); // toute la WRAM partagée à l'ARM7
    for (std::uint32_t i = 0; i < header.arm9.size; ++i)
        prepared.write8(Processor::arm9, header.arm9.load_address + i, arm9[i]);
    for (std::uint32_t i = 0; i < header.arm7.size; ++i)
        prepared.write8(Processor::arm7, header.arm7.load_address + i, arm7[i]);
    const BootEntries entries{entry(header.arm9), entry(header.arm7)};
    Cpu arm9_cpu{Processor::arm9};
    Cpu arm7_cpu{Processor::arm7};
    arm9_cpu.reset(entries.arm9.address, entries.arm9.thumb);
    arm7_cpu.reset(entries.arm7.address, entries.arm7.thumb);
    memory_ = std::move(prepared);
    entries_ = entries;
    arm9_ = arm9_cpu;
    arm7_ = arm7_cpu;
}

const CpuState& Machine::cpu_state(Processor cpu) const noexcept {
    return cpu == Processor::arm9 ? arm9_.state() : arm7_.state();
}

void Machine::set_interrupt_lines(Processor cpu, bool irq, bool fiq) noexcept {
    (cpu == Processor::arm9 ? arm9_ : arm7_).set_interrupt_lines(irq, fiq);
}

void Machine::step(Processor cpu) {
    if (!entries_) throw Error("NDS : aucune cartouche chargée");
    (cpu == Processor::arm9 ? arm9_ : arm7_).step(memory_);
}

void Machine::run_steps(Processor cpu, std::size_t instructions) {
    if (!entries_) throw Error("NDS : aucune cartouche chargée");
    for (std::size_t i = 0; i < instructions; ++i) step(cpu);
}

} // namespace ravenemu::nds
