#pragma once

#include <ravenemu/nds/memory.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds::detail {

struct ProtectionFault {};

/** Vue des accès de données d'une seule instruction, avec privilège stable.
 * La validation précède tout effet MMIO. Les transferts multiples restent
 * transactionnels sur faute MPU, comme le contrat de diagnostic du bus.
 */
class DataBus {
public:
    DataBus(Memory& memory, Processor cpu, bool privileged)
        : memory_(memory), cpu_(cpu), privileged_(privileged) {}
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) {
        check(address, false);
        return memory_.read8(cpu_, address);
    }
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) {
        check(address, false);
        return memory_.read16(cpu_, address);
    }
    [[nodiscard]] std::uint32_t read32(std::uint32_t address) {
        check(address, false);
        return memory_.read32(cpu_, address);
    }
    void write8(std::uint32_t address, std::uint8_t value) {
        check(address, true);
        memory_.write8(cpu_, address, value);
    }
    void write16(std::uint32_t address, std::uint16_t value) {
        check(address, true);
        memory_.write16(cpu_, address, value);
    }
    void write32(std::uint32_t address, std::uint32_t value) {
        check(address, true);
        memory_.write32(cpu_, address, value);
    }
    void read_words(std::uint32_t address, std::span<std::uint32_t> values) {
        check_words(address, values.size(), false);
        memory_.read_words(cpu_, address, values);
    }
    void write_words(std::uint32_t address, std::span<const std::uint32_t> values) {
        check_words(address, values.size(), true);
        memory_.write_words(cpu_, address, values);
    }

private:
    void check(std::uint32_t address, bool write) const {
        if (cpu_ == Processor::arm9 && !memory_.cp15().permits(address,
                write ? Cp15::Access::write : Cp15::Access::read, privileged_)) throw ProtectionFault{};
    }
    void check_words(std::uint32_t address, std::size_t count, bool write) const {
        if (count > (std::uint64_t{0x100000000} - address) / 4U)
            throw Error("NDS : plage RAM débordante");
        for (std::size_t i = 0; i < count; ++i) check(address + static_cast<std::uint32_t>(i * 4U), write);
    }
    Memory& memory_;
    Processor cpu_;
    bool privileged_;
};

} // namespace ravenemu::nds::detail
