#include "gpu/engine2d.hpp"

// Fonds affines de BG2 et BG3 (GBATEK, « DS Video BG Modes / Control » et
// « LCD I/O BG Rotation/Scaling ») : chaque point de la ligne lit la texture
// en (X + PA × x, Y + PC × x), en 8 bits de fraction ; X et Y avancent de PB
// et PD à chaque ligne.
namespace ravenemu::nds {

namespace {

constexpr std::uint16_t transparent = 0x8000;

std::uint16_t standard_color(const Memory& memory, std::uint32_t offset) noexcept {
    return static_cast<std::uint16_t>(load(&memory.palette[offset & 0x7feU], 2) & 0x7fffU);
}

// Valeur signée sur 28 bits (BGxX, BGxY : 20 bits entiers, 8 de fraction).
std::int32_t signed28(std::uint32_t value) noexcept {
    return static_cast<std::int32_t>(value << 4U) >> 4;
}

} // namespace

std::int32_t Engine2d::affine_parameter(unsigned bg, unsigned index) const noexcept {
    const auto offset = (bg == 2 ? 0x20U : 0x30U) + index * 2U;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(registers_[offset / 4] >> ((offset & 2U) * 8U)));
}

void Engine2d::reload_reference(unsigned bg) noexcept {
    const auto base = bg == 2 ? 0x28U : 0x38U;
    reference_x_[bg - 2] = signed28(registers_[base / 4]);
    reference_y_[bg - 2] = signed28(registers_[base / 4 + 1]);
}

void Engine2d::advance_references() noexcept {
    for (unsigned bg = 2; bg < 4; ++bg) {
        reference_x_[bg - 2] += affine_parameter(bg, 1);
        reference_y_[bg - 2] += affine_parameter(bg, 3);
    }
}

void Engine2d::draw_affine_layer(unsigned bg, Layer kind, const Vram& vram, const Memory& memory,
                                 std::span<std::uint16_t, width> colors) const {
    const auto control = bg_control(bg);
    const auto display = display_control();
    const auto area = main_ ? Vram::bg_a : Vram::bg_b;
    const auto area_mask = main_ ? 0x7ffffU : 0x1ffffU;
    const auto size = (control >> 14U) & 3U;
    const auto bitmap = kind == Layer::extended && (control & 0x80U) != 0;
    const auto direct = bitmap && (control & 0x04U) != 0;
    // Dimensions : carrées de 128 << taille pour les cartes ; tables de
    // GBATEK pour les bitmaps et le grand bitmap.
    static constexpr std::array<std::array<unsigned, 2>, 4> bitmap_sizes{{{128, 128}, {256, 256}, {512, 256}, {512, 512}}};
    unsigned map_width = 128U << size;
    unsigned map_height = map_width;
    if (bitmap) {
        map_width = bitmap_sizes[size][0];
        map_height = bitmap_sizes[size][1];
    } else if (kind == Layer::large) {
        map_width = (size & 1U) != 0 ? 1024U : 512U;
        map_height = (size & 1U) != 0 ? 512U : 1024U;
    }
    const auto wrap = (control & 0x2000U) != 0;
    const auto char_base = ((control >> 2U) & 15U) * 0x4000U + (main_ ? ((display >> 24U) & 7U) * 0x10000U : 0U);
    const auto screen_base = ((control >> 8U) & 31U) * 0x800U + (main_ ? ((display >> 27U) & 7U) * 0x10000U : 0U);
    const auto bitmap_base = ((control >> 8U) & 31U) * 0x4000U;
    const auto palette = main_ ? 0x000U : 0x400U;
    const auto pa = affine_parameter(bg, 0);
    const auto pc = affine_parameter(bg, 2);
    for (unsigned x = 0; x < width; ++x) {
        const auto step = static_cast<std::int32_t>(x);
        auto tx = (reference_x_[bg - 2] + pa * step) >> 8;
        auto ty = (reference_y_[bg - 2] + pc * step) >> 8;
        const auto inside = tx >= 0 && ty >= 0 && tx < static_cast<std::int32_t>(map_width) &&
                            ty < static_cast<std::int32_t>(map_height);
        if (!inside && !wrap) {
            colors[x] = transparent;
            continue;
        }
        const auto u = static_cast<std::uint32_t>(tx) & (map_width - 1U);
        const auto v = static_cast<std::uint32_t>(ty) & (map_height - 1U);
        std::uint32_t index{};
        if (direct) {  // couleurs directes : bit 15 = point visible
            const auto value = vram.read(area, (bitmap_base + (v * map_width + u) * 2U) & area_mask, 2);
            colors[x] = (value & 0x8000U) != 0 ? static_cast<std::uint16_t>(value & 0x7fffU) : transparent;
            continue;
        }
        if (bitmap || kind == Layer::large) {
            const auto base = kind == Layer::large ? 0U : bitmap_base;
            index = vram.read(area, (base + v * map_width + u) & area_mask, 1);
        } else if (kind == Layer::affine) {  // carte de 8 bits, tuiles de 256 couleurs
            const auto tile = vram.read(area, (screen_base + (v / 8U) * (map_width / 8U) + u / 8U) & area_mask, 1);
            index = vram.read(area, (char_base + tile * 64U + (v % 8U) * 8U + u % 8U) & area_mask, 1);
        } else {  // carte de 16 bits : tuile, retournements (palettes étendues pas encore)
            const auto entry = vram.read(area, (screen_base + ((v / 8U) * (map_width / 8U) + u / 8U) * 2U) & area_mask, 2);
            const auto px = (entry & 0x400U) != 0 ? 7U - u % 8U : u % 8U;
            const auto py = (entry & 0x800U) != 0 ? 7U - v % 8U : v % 8U;
            index = vram.read(area, (char_base + (entry & 0x3ffU) * 64U + py * 8U + px) & area_mask, 1);
        }
        colors[x] = index == 0 ? transparent : standard_color(memory, palette + index * 2U);
    }
}

} // namespace ravenemu::nds
