#include <ravenemu/nds/memory.hpp>
#include <ravenemu/core.hpp>

#include <array>
#include <algorithm>

namespace ravenemu::nds {
namespace {
constexpr std::size_t main_size = 0x400000;
constexpr std::size_t shared_size = 0x8000;
constexpr std::size_t private_size = 0x10000;
} // namespace

Memory::Memory() : ram_(main_size + shared_size + private_size) {}

void Memory::clear_ram() noexcept {
    std::fill(ram_.begin(), ram_.end(), 0);
    wram_control_ = 0;
    arm9_interrupts_.reset();
    arm7_interrupts_.reset();
    arm9_timers_ = {};
    arm7_timers_ = {};
    ipc_.reset();
    arm9_dma_.reset();
    arm7_dma_.reset();
    dma_fill_ = {};
    lcd_.reset();
    video_.reset();
    display_control_ = 0;
    display_control_b_ = 0;
    graphics_power_ = 0;
}

std::size_t Memory::index(Processor cpu, std::uint32_t address) const {
    // GBATEK : DS Memory Maps / WRAMCNT. Les miroirs désignent les mêmes octets.
    if (address >= 0x02000000 && address < 0x03000000)
        return address & 0x3fffffU;
    if (address >= 0x03000000 && address < 0x04000000) {
        if (cpu == Processor::arm7 && (address >= 0x03800000 || wram_control_ == 0))
            return main_size + shared_size + (address & 0xffffU);
        if (wram_control_ == 0 && cpu == Processor::arm9)
            return main_size + (address & 0x7fffU);
        if (wram_control_ == 3 && cpu == Processor::arm7)
            return main_size + (address & 0x7fffU);
        if (wram_control_ == 1 || wram_control_ == 2) {
            const bool upper = (cpu == Processor::arm9) == (wram_control_ == 1);
            return main_size + (upper ? 0x4000U : 0U) + (address & 0x3fffU);
        }
    }
    throw Error("NDS : accès RAM non mappé");
}

std::uint32_t Memory::peek(Processor cpu, std::uint32_t address, unsigned width) const {
    if ((address & (width - 1U)) != 0) throw Error("NDS : accès RAM non aligné");
    if (address >= 0x04100000 && address <= 0x04100003) {
        if (width != 4) throw Error("NDS : FIFO IPC limitée aux accès 32 bits");
        return ipc_.peek(cpu);
    }
    std::uint32_t value{};
    for (unsigned i = 0; i < width; ++i) {
        const auto io = io_byte(cpu, address + i);
        if (io) { value |= static_cast<std::uint32_t>(*io) << (8U * i); continue; }
        const auto bios = bios_byte(cpu, address + i);
        const auto video = video_.read_byte(cpu, address + i);
        const auto byte = bios ? *bios : (video ? *video : ram_[index(cpu, address + i)]);
        value |= static_cast<std::uint32_t>(byte) << (8U * i);
    }
    return value;
}

std::uint32_t Memory::read(Processor cpu, std::uint32_t address, unsigned width) {
    const auto value = peek(cpu, address, width); // valider avant tout effet
    if (address == 0x04100000) {
        const auto received = ipc_.receive(cpu);
        apply_ipc_interrupts(received.interrupts);
        return received.value;
    }
    return value;
}

void Memory::validate_write(Processor cpu, std::uint32_t address, unsigned width) const {
    if ((address & (width - 1U)) != 0) throw Error("NDS : accès RAM non aligné");
    if (address == 0x04000188 && width == 4) return;
    if (cpu == Processor::arm9 && address >= 0x04000068 && address <= 0x0400006b && width != 4)
        throw Error("NDS : FIFO vidéo limitée aux écritures 32 bits");
    for (unsigned i = 0; i < width; ++i)
        if (!io_byte(cpu, address + i) && !video_.read_byte(cpu, address + i)) static_cast<void>(index(cpu, address + i));
}

void Memory::write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width) {
    validate_write(cpu, address, width);
    // STRB ARM9 vers VRAM/palettes/OAM est ignoré ; la VRAM ARM7 accepte les octets.
    if (width == 1 && cpu == Processor::arm9 && video_.read_byte(cpu, address)) return;
    if (cpu == Processor::arm9 && address == 0x04000068) { video_.write_fifo(value); return; }
    if (address == 0x04000188) {
        apply_ipc_interrupts(ipc_.send(cpu, value));
        return;
    }
    for (unsigned i = 0; i < width; ++i) {
        const auto byte = static_cast<std::uint8_t>(value >> (8U * i));
        if (io_byte(cpu, address + i)) write_io_byte(cpu, address + i, byte);
        else if (video_.read_byte(cpu, address + i)) video_.write_byte(cpu, address + i, byte);
        else ram_[index(cpu, address + i)] = byte;
    }
}

std::uint8_t Memory::read8(Processor cpu, std::uint32_t address) {
    return static_cast<std::uint8_t>(read(cpu, address, 1));
}
std::uint16_t Memory::read16(Processor cpu, std::uint32_t address) {
    return static_cast<std::uint16_t>(read(cpu, address, 2));
}
std::uint32_t Memory::read32(Processor cpu, std::uint32_t address) { return read(cpu, address, 4); }
std::uint8_t Memory::peek8(Processor cpu, std::uint32_t address) const {
    return static_cast<std::uint8_t>(peek(cpu, address, 1));
}
std::uint16_t Memory::peek16(Processor cpu, std::uint32_t address) const {
    return static_cast<std::uint16_t>(peek(cpu, address, 2));
}
std::uint32_t Memory::peek32(Processor cpu, std::uint32_t address) const { return peek(cpu, address, 4); }
void Memory::write8(Processor cpu, std::uint32_t address, std::uint8_t value) { write(cpu, address, value, 1); }
void Memory::write16(Processor cpu, std::uint32_t address, std::uint16_t value) { write(cpu, address, value, 2); }
void Memory::write32(Processor cpu, std::uint32_t address, std::uint32_t value) { write(cpu, address, value, 4); }

void Memory::write_words(Processor cpu, std::uint32_t address, std::span<const std::uint32_t> values) {
    if ((address & 3U) != 0) throw Error("NDS : accès RAM non aligné");
    const auto available = (std::uint64_t{0x100000000} - address) / 4U;
    if (values.size() > available) throw Error("NDS : plage RAM débordante");
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto current = address + static_cast<std::uint32_t>(i * 4);
        validate_write(cpu, current, 4);
    }
    for (std::size_t i = 0; i < values.size(); ++i)
        write32(cpu, address + static_cast<std::uint32_t>(i * 4), values[i]);
}

void Memory::read_words(Processor cpu, std::uint32_t address, std::span<std::uint32_t> values) {
    if ((address & 3U) != 0) throw Error("NDS : accès RAM non aligné");
    const auto available = (std::uint64_t{0x100000000} - address) / 4U;
    if (values.size() > available) throw Error("NDS : plage RAM débordante");
    for (std::size_t i = 0; i < values.size(); ++i)
        static_cast<void>(peek32(cpu, address + static_cast<std::uint32_t>(i * 4)));
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = read32(cpu, address + static_cast<std::uint32_t>(i * 4));
}

} // namespace ravenemu::nds
