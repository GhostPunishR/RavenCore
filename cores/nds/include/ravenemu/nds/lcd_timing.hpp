#pragma once

#include <ravenemu/nds/hardware.hpp>

#include <array>

namespace ravenemu::nds {

// Horloge de balayage commune aux deux écrans, sans rendu de pixels.
class LcdTiming {
public:
    static constexpr std::uint32_t line_cycles = 2130;
    static constexpr std::uint32_t visible_cycles = 256 * 6;
    static constexpr std::uint32_t visible_lines = 192;
    static constexpr std::uint32_t total_lines = 263;
    static constexpr std::uint32_t frame_cycles = line_cycles * total_lines;

    struct Events {
        std::uint32_t arm9{}, arm7{}; // sources IF LCD 0..2
        bool vblank{}, hblank{}; // DMA : HBlank uniquement dans les lignes visibles
    };

    void reset() noexcept;
    [[nodiscard]] std::uint16_t vcount() const noexcept;
    [[nodiscard]] std::uint32_t cycle_in_line() const noexcept { return phase_ % line_cycles; }
    [[nodiscard]] std::uint16_t status(Processor cpu) const noexcept;
    void write_status(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept;
    // Cycles à timer_clock_hz ; événements coalescés, sans boucle par ligne/image.
    [[nodiscard]] Events advance_cycles(std::uint64_t cycles) noexcept;

private:
    std::uint32_t phase_{};
    std::array<std::uint16_t, 2> controls_{};
    [[nodiscard]] static std::uint16_t compare(std::uint16_t control) noexcept;
};

} // namespace ravenemu::nds
