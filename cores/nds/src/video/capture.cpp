#include <ravenemu/nds/video.hpp>
#include <ravenemu/core.hpp>
#include "renderer.hpp"
#include "vram_layout.hpp"

#include <algorithm>

namespace ravenemu::nds {

void VideoMemory::write_capture(unsigned offset, std::uint8_t value) noexcept {
    const auto shift = offset * 8U;
    capture_control_ = ((capture_control_ & ~(0xffU << shift)) |
        (static_cast<std::uint32_t>(value) << shift)) & 0xef3f1f1fU;
    if (!(capture_control_ & 0x80000000U)) capture_active_ = false;
}

unsigned VideoMemory::capture_height() const noexcept {
    constexpr std::array<unsigned, 4> heights{128, 64, 128, 192};
    return heights[(capture_control_ >> 20U) & 3U];
}

void VideoMemory::capture_line(unsigned line, std::uint32_t display_control, std::uint16_t power) {
    const auto control = capture_control_;
    const auto destination = (control >> 16U) & 3U;
    if ((controls_[destination] & 0x87U) != 0x80U || line >= capture_height()) return;
    const auto mode = (control >> 29U) & 3U;
    const bool use_a = mode != 1, use_b = mode != 0;
    if (use_a && (control & 0x01000000U)) throw Error("NDS : source de capture 3D non implémentée");
    if (use_b && (control & 0x02000000U)) throw Error("NDS : source de capture FIFO non implémentée");
    const auto width = ((control >> 20U) & 3U) == 0 ? 128U : 256U;
    std::array<std::uint16_t, VideoFrame::width> source_a{}, source_b{};
    if (use_a) {
        detail::Renderer{*this, 0, display_control}.render_graphics(source_a, line, power);
        for (auto& color : source_a) color |= 0x8000U; // sortie composée 2D opaque, backdrop compris
    }
    if (use_b) {
        const auto bank = (display_control >> 18U) & 3U;
        if ((controls_[bank] & 0x87U) == 0x80U) {
            const auto offset = ((display_control >> 16U) & 3U) == 2 ? 0 : ((control >> 26U) & 3U) * 0x8000U;
            // La source conserve son stride écran de 256 pixels, même pour une capture 128x128.
            for (unsigned x = 0; x < width; ++x) {
                const auto physical = detail::vram_starts[bank] + ((offset + (line * 256 + x) * 2) & 0x1ffffU);
                source_b[x] = static_cast<std::uint16_t>(bytes_[physical] | (static_cast<unsigned>(bytes_[physical + 1]) << 8U));
            }
        }
    }
    const auto eva = std::min(16U, control & 31U), evb = std::min(16U, (control >> 8U) & 31U);
    const auto start = ((control >> 18U) & 3U) * 0x8000U + line * width * 2;
    // Les deux sources sont échantillonnées avant l'écriture, y compris si elles partagent une banque.
    for (unsigned x = 0; x < width; ++x) {
        unsigned value = mode == 0 ? source_a[x] : source_b[x];
        if (mode >= 2) {
            const auto a = (source_a[x] & 0x8000U) ? eva : 0, b = (source_b[x] & 0x8000U) ? evb : 0;
            value = (a || b) ? 0x8000U : 0;
            for (unsigned shift = 0; shift < 15; shift += 5) {
                const auto channel_a = (static_cast<unsigned>(source_a[x]) >> shift) & 31U;
                const auto channel_b = (static_cast<unsigned>(source_b[x]) >> shift) & 31U;
                value |= std::min(31U, (channel_a * a + channel_b * b) / 16) << shift;
            }
        }
        const auto physical = detail::vram_starts[destination] + ((start + x * 2) & 0x1ffffU);
        bytes_[physical] = static_cast<std::uint8_t>(value);
        bytes_[physical + 1] = static_cast<std::uint8_t>(value >> 8U);
    }
}

} // namespace ravenemu::nds
