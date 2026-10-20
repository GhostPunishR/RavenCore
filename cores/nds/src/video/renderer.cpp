#include "renderer.hpp"
#include <ravenemu/core.hpp>

#include <algorithm>

namespace ravenemu::nds::detail {
namespace {
int signed16(std::uint16_t value) { return value < 0x8000 ? value : static_cast<int>(value) - 0x10000; }
int floor256(std::int64_t value) {
    return static_cast<int>(value >= 0 ? value / 256 : -((-value + 255) / 256));
}
} // namespace

std::uint16_t Renderer::reg16(unsigned offset) const {
    const auto& bytes = memory_.registers_[engine_];
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<unsigned>(bytes[offset + 1]) << 8U));
}

std::int64_t Renderer::reference(unsigned offset) const {
    const auto value = static_cast<std::uint32_t>(reg16(offset)) | (static_cast<std::uint32_t>(reg16(offset + 2)) << 16U);
    return (value & 0x08000000U) ? static_cast<std::int64_t>(value) - 0x10000000 : value;
}

std::uint8_t Renderer::gpu8(bool object, std::uint32_t offset) const {
    const auto base = (object ? 0x06400000U : 0x06000000U) + engine_ * 0x200000U;
    const auto mask = engine_ ? 0x1ffffU : (object ? 0x3ffffU : 0x7ffffU);
    return memory_.read_byte(Processor::arm9, base + (offset & mask)).value_or(0);
}

std::uint16_t Renderer::gpu16(bool object, std::uint32_t offset) const {
    return static_cast<std::uint16_t>(gpu8(object, offset) | (static_cast<unsigned>(gpu8(object, offset + 1)) << 8U));
}

std::uint16_t Renderer::palette(bool object, unsigned index) const {
    const auto offset = engine_ * 0x400 + (object ? 0x200 : 0) + index * 2;
    return static_cast<std::uint16_t>((memory_.palettes_[offset] | (static_cast<unsigned>(memory_.palettes_[offset + 1]) << 8U)) & 0x7fffU);
}

std::uint16_t Renderer::oam16(unsigned offset) const {
    const auto base = engine_ * 0x400 + offset;
    return static_cast<std::uint16_t>(memory_.oam_[base] | (static_cast<unsigned>(memory_.oam_[base + 1]) << 8U));
}

std::optional<std::uint16_t> Renderer::tile_pixel(bool object, std::uint32_t address, bool depth8,
                                                unsigned x, unsigned y, unsigned bank, int extended_slot) const {
    const auto byte = gpu8(object, address + y * (depth8 ? 8U : 4U) + (depth8 ? x : x / 2));
    const auto index = depth8 ? byte : (byte >> ((x & 1U) * 4U)) & 15U;
    if (index == 0) return std::nullopt;
    if (depth8 && extended_slot >= 0)
        return extended_palette(object, static_cast<unsigned>(extended_slot), bank, index);
    return palette(object, index + (depth8 ? 0 : bank * 16));
}

std::optional<std::uint16_t> Renderer::background_pixel(unsigned bg, Background kind, unsigned x, unsigned y) const {
    const auto cnt = reg16(8 + bg * 2);
    const auto char_base = ((cnt >> 2U) & 15U) * 0x4000U + (engine_ ? 0 : ((control_ >> 24U) & 7U) * 0x10000U);
    const auto map_base = ((cnt >> 8U) & 31U) * 0x800U + (engine_ ? 0 : ((control_ >> 27U) & 7U) * 0x10000U);
    const auto size = static_cast<unsigned>(cnt) >> 14U;
    unsigned width, height, sx, sy;
    if (kind == Background::text) {
        width = (size & 1U) ? 512 : 256; height = (size & 2U) ? 512 : 256;
        sx = (x + reg16(0x10 + bg * 4)) & (width - 1);
        sy = (y + reg16(0x12 + bg * 4)) & (height - 1);
    } else {
        width = height = 128U << size;
        if (kind == Background::large) { width = size == 0 ? 512 : 1024; height = size == 0 ? 1024 : 512; }
        else if (kind == Background::extended && (cnt & 0x80U)) {
            width = size == 0 ? 128 : (size == 1 ? 256 : 512); height = size < 2 ? width : (size == 2 ? 256 : 512);
        }
        const auto matrix = 0x20 + (bg - 2) * 0x10;
        const auto tx = floor256(reference(matrix + 8) + signed16(reg16(matrix)) * static_cast<std::int64_t>(x) +
                                 signed16(reg16(matrix + 2)) * static_cast<std::int64_t>(y));
        const auto ty = floor256(reference(matrix + 12) + signed16(reg16(matrix + 4)) * static_cast<std::int64_t>(x) +
                                 signed16(reg16(matrix + 6)) * static_cast<std::int64_t>(y));
        if (!(cnt & 0x2000U) && (tx < 0 || ty < 0 || tx >= static_cast<int>(width) || ty >= static_cast<int>(height))) return std::nullopt;
        sx = static_cast<unsigned>(tx) & (width - 1); sy = static_cast<unsigned>(ty) & (height - 1);
    }
    if (kind == Background::large || (kind == Background::extended && (cnt & 0x80U))) {
        const auto base = kind == Background::large ? 0 : ((cnt >> 8U) & 31U) * 0x4000U;
        const auto offset = sy * width + sx;
        if (kind == Background::extended && (cnt & 4U)) {
            const auto pixel = gpu16(false, base + offset * 2);
            return pixel & 0x8000U ? std::optional<std::uint16_t>(pixel & 0x7fffU) : std::nullopt;
        }
        const auto index = gpu8(false, base + offset);
        return index ? std::optional<std::uint16_t>(palette(false, index)) : std::nullopt;
    }
    if (kind == Background::affine) {
        const auto tile = gpu8(false, map_base + (sy / 8) * (width / 8) + sx / 8);
        return tile_pixel(false, char_base + tile * 64, true, sx & 7U, sy & 7U, 0);
    }
    const auto entry_index = kind == Background::text ?
        ((sy / 256) * (width / 256) + sx / 256) * 1024 + ((sy / 8) & 31U) * 32 + ((sx / 8) & 31U) :
        (sy / 8) * (width / 8) + sx / 8;
    const auto entry = gpu16(false, map_base + entry_index * 2);
    const auto u = (entry & 0x400U) ? 7 - (sx & 7U) : sx & 7U;
    const auto v = (entry & 0x800U) ? 7 - (sy & 7U) : sy & 7U;
    const bool depth8 = kind == Background::extended || (cnt & 0x80U);
    const auto slot = bg + (bg < 2 && (cnt & 0x2000U) ? 2U : 0U);
    const auto extended_slot = (control_ & 0x40000000U) ? static_cast<int>(slot) : -1;
    return tile_pixel(false, char_base + (entry & 0x3ffU) * (depth8 ? 64 : 32), depth8, u, v, entry >> 12U, extended_slot);
}

void Renderer::backgrounds(std::span<Sample> pixels, unsigned first_line) const {
    using B = Background;
    constexpr std::array<std::array<B, 4>, 7> modes{{
        {B::text,B::text,B::text,B::text}, {B::text,B::text,B::text,B::affine},
        {B::text,B::text,B::affine,B::affine}, {B::text,B::text,B::text,B::extended},
        {B::text,B::text,B::affine,B::extended}, {B::text,B::text,B::extended,B::extended},
        {B::three_d,B::none,B::large,B::none}}};
    const auto mode = control_ & 7U;
    if (mode == 7 || (engine_ && mode == 6)) throw Error("NDS : mode BG réservé");
    for (unsigned bg = 0; bg < 4; ++bg) {
        if (!(control_ & (1U << (8U + bg)))) continue;
        const auto kind = modes[mode][bg];
        if (kind == B::none) continue;
        if (kind == B::three_d || (bg == 0 && !engine_ && (control_ & 8U))) throw Error("NDS : couche BG0 3D non implémentée");
        const auto cnt = reg16(8 + bg * 2);
        const auto mosaic = reg16(0x4c);
        const auto mosaic_width = (cnt & 0x40U) ? (mosaic & 15U) + 1 : 1;
        const auto mosaic_height = (cnt & 0x40U) ? ((mosaic >> 4U) & 15U) + 1 : 1;
        if (kind == B::large && (cnt >> 14U) > 1) throw Error("NDS : taille de grand bitmap réservée");
        if (engine_ && kind == B::extended && (cnt & 0x80U)) {
            const auto size = cnt >> 14U;
            const auto bytes = (size == 0 ? 128U * 128 : (size == 1 ? 256U * 256 : (size == 2 ? 512U * 256 : 512U * 512))) * ((cnt & 4U) ? 2U : 1U);
            if (bytes > 0x20000) throw Error("NDS : bitmap trop grand pour le moteur B");
        }
        const auto rank = (cnt & 3U) * 1024 + 128 + bg;
        for (unsigned row = 0; row < pixels.size() / VideoFrame::width; ++row) for (unsigned x = 0; x < VideoFrame::width; ++x) {
            const auto y = first_line + row;
            auto& pixel = pixels[row * VideoFrame::width + x];
            if (!(pixel.mask & (1U << bg)) || rank >= pixel.below.rank) continue;
            if (const auto color = background_pixel(bg, kind, x - x % mosaic_width, y - y % mosaic_height)) pixel.insert({*color, rank, bg, 0});
        }
    }
}

void Renderer::render(std::span<std::uint16_t> output, std::uint16_t power) const {
    const auto mode = (control_ >> 16U) & 3U;
    if ((control_ & 0x80U) || mode == 0) return;
    if (mode == 2 && engine_ == 0) {
        const auto bank = (control_ >> 18U) & 3U;
        if ((memory_.controls_[bank] & 0x87U) != 0x80U) throw Error("NDS : la banque d'affichage doit être activée en mode LCDC");
        for (std::size_t pixel = 0; pixel < output.size(); ++pixel) {
            const auto start = bank * 0x20000U + pixel * 2;
            output[pixel] = static_cast<std::uint16_t>((memory_.bytes_[start] |
                (static_cast<unsigned>(memory_.bytes_[start + 1]) << 8U)) & 0x7fffU);
        }
        return;
    }
    if (mode != 1) throw Error("NDS : mode d'affichage FIFO ou réservé non implémenté");
    render_graphics(output, 0, power);
}

void Renderer::render_graphics(std::span<std::uint16_t> output, unsigned first_line, std::uint16_t power) const {
    std::fill(output.begin(), output.end(), 0x7fff);
    if ((control_ & 0x80U) || !(power & (engine_ ? 0x200U : 2U))) return;
    std::vector<Sample> pixels(output.size());
    for (auto& pixel : pixels) pixel.top = {palette(false, 0), 0xffffU, 5, 0};
    objects(pixels, first_line);
    mosaic_objects(pixels);
    windows(pixels, first_line);
    backgrounds(pixels, first_line);
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (pixels[i].mask & 0x10U) pixels[i].insert(pixels[i].object);
        output[i] = composite(pixels[i]);
    }
}

} // namespace ravenemu::nds::detail
