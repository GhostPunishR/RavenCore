#pragma once

#include <ravenemu/core.hpp>
#include <ravenemu/nds/memory.hpp>

#include <array>
#include <optional>
#include <string>

namespace ravenemu::nds {

struct CpuState {
    // r15 est l'adresse de la prochaine instruction à lire. Les lectures
    // architecturales du PC ajoutent 8 en ARM, 4 en Thumb.
    std::array<std::uint32_t, 16> registers{};
    bool negative{};
    bool zero{};
    bool carry{};
    bool overflow{};
    bool thumb{};
    std::uint64_t instructions{}; // instructions terminées, y compris condition fausse
    bool operator==(const CpuState&) const = default;
};

class CpuExecutionError final : public Error {
public:
    CpuExecutionError(Processor processor, std::uint32_t address, bool thumb,
                      std::optional<std::uint32_t> instruction, const std::string& reason);
    [[nodiscard]] Processor processor() const noexcept { return processor_; }
    [[nodiscard]] std::uint32_t address() const noexcept { return address_; }
    [[nodiscard]] bool thumb() const noexcept { return thumb_; }
    // Absente si la lecture de l'instruction elle-même a échoué.
    [[nodiscard]] std::optional<std::uint32_t> instruction() const noexcept { return instruction_; }

private:
    Processor processor_;
    std::uint32_t address_;
    bool thumb_;
    std::optional<std::uint32_t> instruction_;
};

/** Interpréteur expérimental ARM/Thumb, avec quelques extensions ARMv5 sur ARM9.
 * Ni cycles, pipeline préchargé, modes privilégiés, exceptions, ni CP15.
 * Une instruction non implémentée ou un accès mémoire refusé lève
 * CpuExecutionError, sans avancer le CPU ni modifier ses registres.
 */
class Cpu {
public:
    explicit Cpu(Processor processor) : processor_(processor) {}
    void reset(std::uint32_t address, bool thumb);
    void set_register(std::size_t index, std::uint32_t value);
    [[nodiscard]] const CpuState& state() const noexcept { return state_; }
    void step(Memory& memory);

private:
    Processor processor_;
    CpuState state_;
};

} // namespace ravenemu::nds
