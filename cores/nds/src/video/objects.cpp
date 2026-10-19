#include "renderer.hpp"
#include <ravenemu/core.hpp>

namespace ravenemu::nds::detail {
namespace {
int signed16(std::uint16_t value) { return value < 0x8000 ? value : static_cast<int>(value) - 0x10000; }
int floor256(int value) { return value >= 0 ? value / 256 : -((-value + 255) / 256); }
} // namespace

void Renderer::objects(std::span<Sample> pixels) const {
    if (!(control_ & 0x1000U)) return;
    const bool one_d = (control_ & 0x10U) != 0;
    const auto boundary = (control_ >> 20U) & 3U;
    constexpr unsigned widths[3][4]{{8,16,32,64},{16,32,32,64},{8,8,16,32}};
    constexpr unsigned heights[3][4]{{8,16,32,64},{8,8,16,32},{16,32,32,64}};
    for (unsigned obj = 0; obj < 128; ++obj) {
        const auto attr0 = oam16(obj * 8), attr1 = oam16(obj * 8 + 2), attr2 = oam16(obj * 8 + 4);
        const bool affine = (attr0 & 0x100U) != 0;
        if (!affine && (attr0 & 0x200U)) continue;
        const auto mode = (attr0 >> 10U) & 3U;
        const bool bitmap = mode == 3, window = mode == 2;
        if (!bitmap && engine_ && one_d && boundary == 3) throw Error("NDS : frontière OBJ 256 Kio réservée au moteur A");
        if (window && !(control_ & 0x8000U)) continue;
        if (bitmap && !(attr2 & 0xf000U)) continue;
        if (attr0 & 0x1000U) throw Error("NDS : mosaïque OBJ non implémentée");
        const bool bitmap_1d = (control_ & 0x40U) != 0;
        if (bitmap && bitmap_1d && (control_ & 0x20U)) throw Error("NDS : mapping bitmap OBJ réservé");
        const auto shape = attr0 >> 14U, size = attr1 >> 14U;
        if (shape == 3) throw Error("NDS : forme OBJ réservée");
        const bool depth8 = (attr0 & 0x2000U) != 0;
        const int extended_slot = !window && (control_ & 0x80000000U) ? 0 : -1;
        const auto width = widths[shape][size], height = heights[shape][size];
        const auto bound_width = width * (affine && (attr0 & 0x200U) ? 2U : 1U);
        const auto bound_height = height * (affine && (attr0 & 0x200U) ? 2U : 1U);
        const auto matrix = ((attr1 >> 9U) & 31U) * 32;
        auto tile = attr2 & 0x3ffU;
        if (!bitmap && !one_d && depth8) tile &= ~1U;
        const auto bitmap_mask = (control_ & 0x20U) ? 31U : 15U;
        const auto base = bitmap ? (bitmap_1d ? tile * ((!engine_ && (control_ & 0x400000U)) ? 256U : 128U) :
            (tile & bitmap_mask) * 0x10U + (tile & ~bitmap_mask) * 0x80U) : tile * (one_d ? 32U << boundary : 32U);
        const auto rank = ((attr2 >> 10U) & 3U) * 1024 + obj;
        for (unsigned v = 0; v < bound_height; ++v) for (unsigned u = 0; u < bound_width; ++u) {
            const auto x = ((attr1 & 0x1ffU) + u) & 0x1ffU, y = ((attr0 & 0xffU) + v) & 0xffU;
            if (x >= VideoFrame::width || y >= VideoFrame::height) continue;
            int sx, sy;
            if (affine) {
                const auto dx = static_cast<int>(u) - static_cast<int>(bound_width / 2), dy = static_cast<int>(v) - static_cast<int>(bound_height / 2);
                sx = floor256(signed16(oam16(matrix + 6)) * dx + signed16(oam16(matrix + 14)) * dy) + static_cast<int>(width / 2);
                sy = floor256(signed16(oam16(matrix + 22)) * dx + signed16(oam16(matrix + 30)) * dy) + static_cast<int>(height / 2);
            } else {
                sx = static_cast<int>((attr1 & 0x1000U) ? width - 1 - u : u);
                sy = static_cast<int>((attr1 & 0x2000U) ? height - 1 - v : v);
            }
            if (sx < 0 || sy < 0 || sx >= static_cast<int>(width) || sy >= static_cast<int>(height)) continue;
            auto& pixel = pixels[y * VideoFrame::width + x];
            if (!window && rank >= pixel.object.rank) continue;
            if (bitmap) {
                const auto stride = bitmap_1d ? width : ((control_ & 0x20U) ? 256U : 128U);
                const auto color = gpu16(true, base + (static_cast<unsigned>(sy) * stride + static_cast<unsigned>(sx)) * 2);
                if (color & 0x8000U) pixel.object = {static_cast<std::uint16_t>(color & 0x7fffU), rank, 4,
                    static_cast<unsigned>(attr2 >> 12U) + 1};
                continue;
            }
            const auto tile_bytes = depth8 ? 64U : 32U;
            const auto address = base + (static_cast<unsigned>(sy) / 8) * (one_d ? width / 8 * tile_bytes : 1024U) +
                (static_cast<unsigned>(sx) / 8) * tile_bytes;
            if (const auto color = tile_pixel(true, address, depth8, static_cast<unsigned>(sx) & 7U, static_cast<unsigned>(sy) & 7U, attr2 >> 12U, extended_slot)) {
                if (window) pixel.object_window = true;
                else pixel.object = {*color, rank, 4, mode == 1 ? 17U : 0U};
            }
        }
    }
}

} // namespace ravenemu::nds::detail
