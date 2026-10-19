#pragma once

#include <ravenemu/nds/hardware.hpp>

#include <array>
#include <cstdint>

namespace ravenemu::nds {

// État commun aux deux CPU, sans références persistantes au bus ou aux IRQ.
class Ipc {
public:
    struct Interrupts {
        std::uint32_t arm9{}, arm7{}; // masques IF, pas des lignes de niveau
    };
    struct Received {
        std::uint32_t value{};
        Interrupts interrupts;
    };

    void reset() noexcept;
    [[nodiscard]] std::uint16_t sync(Processor cpu) const noexcept;
    [[nodiscard]] std::uint16_t control(Processor cpu) const noexcept;
    [[nodiscard]] Interrupts write_sync(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept;
    [[nodiscard]] Interrupts write_control(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept;
    [[nodiscard]] Interrupts send(Processor cpu, std::uint32_t value) noexcept;
    [[nodiscard]] Received receive(Processor cpu) noexcept;
    // Inspection sans retirer de mot, sans erreur et sans interruption.
    [[nodiscard]] std::uint32_t peek(Processor cpu) const noexcept;

private:
    struct Fifo {
        std::array<std::uint32_t, 16> words{};
        unsigned head{}, size{};
        std::uint32_t last{};
    };
    struct Endpoint {
        std::uint16_t sync{}, control{};
        bool error{};
        Fifo outgoing;
    };
    std::array<Endpoint, 2> endpoints_{};
    [[nodiscard]] Endpoint& endpoint(Processor cpu) noexcept;
    [[nodiscard]] const Endpoint& endpoint(Processor cpu) const noexcept;
    [[nodiscard]] Interrupts conditions() const noexcept;
    [[nodiscard]] Interrupts rising(Interrupts before) const noexcept;
};

} // namespace ravenemu::nds
