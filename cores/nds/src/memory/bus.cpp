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

std::uint32_t Memory::read(Processor cpu, std::uint32_t address, unsigned width) const {
    if ((address & (width - 1U)) != 0) throw Error("NDS : accès RAM non aligné");
    std::uint32_t value{};
    for (unsigned i = 0; i < width; ++i) {
        const auto io = io_byte(cpu, address + i);
        if (io) { value |= static_cast<std::uint32_t>(*io) << (8U * i); continue; }
        const auto bios = bios_byte(cpu, address + i);
        const auto byte = bios ? *bios : ram_[index(cpu, address + i)];
        value |= static_cast<std::uint32_t>(byte) << (8U * i);
    }
    return value;
}

void Memory::write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width) {
    if ((address & (width - 1U)) != 0) throw Error("NDS : accès RAM non aligné");
    std::array<std::size_t, 4> destinations{};
    std::array<bool, 4> io{};
    for (unsigned i = 0; i < width; ++i) {
        io[i] = io_byte(cpu, address + i).has_value();
        if (!io[i]) destinations[i] = index(cpu, address + i);
    }
    for (unsigned i = 0; i < width; ++i) {
        const auto byte = static_cast<std::uint8_t>(value >> (8U * i));
        if (io[i]) write_io_byte(cpu, address + i, byte);
        else ram_[destinations[i]] = byte;
    }
}

std::uint8_t Memory::read8(Processor cpu, std::uint32_t address) const {
    return static_cast<std::uint8_t>(read(cpu, address, 1));
}
std::uint16_t Memory::read16(Processor cpu, std::uint32_t address) const {
    return static_cast<std::uint16_t>(read(cpu, address, 2));
}
std::uint32_t Memory::read32(Processor cpu, std::uint32_t address) const { return read(cpu, address, 4); }
void Memory::write8(Processor cpu, std::uint32_t address, std::uint8_t value) { write(cpu, address, value, 1); }
void Memory::write16(Processor cpu, std::uint32_t address, std::uint16_t value) { write(cpu, address, value, 2); }
void Memory::write32(Processor cpu, std::uint32_t address, std::uint32_t value) { write(cpu, address, value, 4); }

void Memory::write_words(Processor cpu, std::uint32_t address, std::span<const std::uint32_t> values) {
    if ((address & 3U) != 0) throw Error("NDS : accès RAM non aligné");
    const auto available = (std::uint64_t{0x100000000} - address) / 4U;
    if (values.size() > available) throw Error("NDS : plage RAM débordante");
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto current = address + static_cast<std::uint32_t>(i * 4);
        for (unsigned byte = 0; byte < 4; ++byte)
            if (!io_byte(cpu, current + byte)) static_cast<void>(index(cpu, current + byte));
    }
    for (std::size_t i = 0; i < values.size(); ++i)
        write32(cpu, address + static_cast<std::uint32_t>(i * 4), values[i]);
}

} // namespace ravenemu::nds
