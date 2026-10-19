#include "renderer.hpp"
#include <ravenemu/core.hpp>

#include <algorithm>

namespace ravenemu::nds::detail {
namespace {
bool inside(unsigned coordinate, std::uint16_t bounds) {
    const auto start = static_cast<unsigned>(bounds) >> 8U, end = bounds & 0xffU;
    return start <= end ? coordinate >= start && coordinate < end : coordinate >= start || coordinate < end;
}

std::uint16_t blend(std::uint16_t first, std::uint16_t second, unsigned a, unsigned b) {
    unsigned result = 0;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        const auto first_channel = (static_cast<unsigned>(first) >> shift) & 31U;
        const auto second_channel = (static_cast<unsigned>(second) >> shift) & 31U;
        result |= std::min(31U, (first_channel * a + second_channel * b) / 16) << shift;
    }
    return static_cast<std::uint16_t>(result);
}

std::uint16_t brightness(std::uint16_t color, unsigned amount, bool increase) {
    unsigned result = 0;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        const auto channel = (static_cast<unsigned>(color) >> shift) & 31U;
        result |= (increase ? channel + (31 - channel) * amount / 16 : channel - channel * amount / 16) << shift;
    }
    return static_cast<std::uint16_t>(result);
}
} // namespace

void Renderer::windows(std::span<Sample> pixels) const {
    if (!(control_ & 0xe000U)) return;
    const auto winin = reg16(0x48), winout = reg16(0x4a);
    for (unsigned y = 0; y < VideoFrame::height; ++y) for (unsigned x = 0; x < VideoFrame::width; ++x) {
        auto& pixel = pixels[y * VideoFrame::width + x];
        unsigned mask = winout;
        if ((control_ & 0x8000U) && pixel.object_window) mask = winout >> 8U;
        if ((control_ & 0x4000U) && inside(x, reg16(0x42)) && inside(y, reg16(0x46))) mask = winin >> 8U;
        if ((control_ & 0x2000U) && inside(x, reg16(0x40)) && inside(y, reg16(0x44))) mask = winin;
        pixel.mask = static_cast<std::uint8_t>(mask & 0x3fU);
    }
}

void Renderer::apply_master_brightness(std::span<std::uint16_t> output) const {
    const auto control = reg16(0x6c);
    const auto mode = control >> 14U;
    if (mode == 3) throw Error("NDS : mode de luminosité maître réservé");
    const auto factor = std::min(16U, control & 31U);
    if (mode == 0 || factor == 0) return;
    for (auto& color : output) color = brightness(color, factor, mode == 1);
}

std::uint16_t Renderer::composite(const Sample& sample) const {
    const auto& top = sample.top;
    if (!(sample.mask & 0x20U)) return top.color;
    const auto control = reg16(0x50);
    const auto effect = (control >> 6U) & 3U;
    const bool first = (control & (1U << top.layer)) != 0;
    const bool second = (control & (0x100U << sample.below.layer)) != 0;
    // La couche OBJ ne contient que son gagnant : aucun mélange OBJ vers OBJ.
    if (second && (top.alpha || (effect == 1 && first))) {
        const auto a = top.alpha && top.alpha <= 16 ? top.alpha : std::min(16U, reg16(0x52) & 31U);
        const auto b = top.alpha && top.alpha <= 16 ? 16 - a : std::min(16U, (reg16(0x52) >> 8U) & 31U);
        return blend(top.color, sample.below.color, a, b);
    }
    if (first && effect >= 2) return brightness(top.color, std::min(16U, reg16(0x54) & 31U), effect == 2);
    return top.color;
}

} // namespace ravenemu::nds::detail
