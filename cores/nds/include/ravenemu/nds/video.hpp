#pragma once

#include <ravenemu/nds/hardware.hpp>

#include <array>
#include <optional>
#include <vector>

namespace ravenemu::nds {

enum class VramBank { a, b, c, d, e, f, g, h, i };

struct VideoFrame {
    static constexpr unsigned width = 256, height = 192;
    // RGB555 : rouge bits 0..4, vert 5..9, bleu 10..14 ; bit 15 à zéro.
    std::array<std::uint16_t, width * height> upper{};
    std::array<std::uint16_t, width * height> lower{};
};

class Memory;
namespace detail { class Renderer; }

// Les neuf banques possèdent leurs octets, indépendamment de leur affectation.
class VideoMemory {
public:
    VideoMemory();
    void reset() noexcept;
    [[nodiscard]] std::uint8_t control(VramBank bank) const noexcept;
    void set_control(VramBank bank, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t arm7_status() const noexcept;
    // Instantané des deux moteurs, sans progression du LCD.
    [[nodiscard]] VideoFrame render_frame(std::uint32_t display_control, std::uint16_t power,
                                         std::uint32_t display_control_b = 0) const;

private:
    friend class Memory;
    friend class detail::Renderer;
    std::vector<std::uint8_t> bytes_;
    std::array<std::uint8_t, 9> controls_{};
    std::array<std::uint8_t, 0x800> palettes_{}, oam_{};
    std::array<std::array<std::uint8_t, 0x58>, 2> registers_{};
    std::uint16_t power_{};
    void write_register(unsigned engine, unsigned offset, std::uint8_t value) noexcept;
    [[nodiscard]] std::optional<std::uint32_t> index(unsigned bank, Processor cpu, std::uint32_t address) const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> read_byte(Processor cpu, std::uint32_t address) const noexcept;
    void write_byte(Processor cpu, std::uint32_t address, std::uint8_t value) noexcept;
};

} // namespace ravenemu::nds
