#include "gpu/object_renderer.hpp"

#include <algorithm>

namespace ravenemu::nds {

namespace {

// Largeur et hauteur en points, par forme (carré, large, haut) et taille.
constexpr std::array<std::array<std::array<unsigned, 2>, 4>, 3> object_sizes{{
    {{{8, 8}, {16, 16}, {32, 32}, {64, 64}}},
    {{{16, 8}, {32, 8}, {32, 16}, {64, 32}}},
    {{{8, 16}, {8, 32}, {16, 32}, {32, 64}}},
}};

struct Object {
    unsigned index;
    std::uint32_t attribute0;
    std::uint32_t attribute1;
    std::uint32_t attribute2;
    unsigned width;
    unsigned height;
};

class ObjectDrawer {
public:
    ObjectDrawer(bool main, std::uint32_t control, const Vram& vram, const Memory& memory) noexcept
        : main_(main), control_(control), vram_(vram), memory_(memory), oam_(main ? 0U : 0x400U) {}

    std::uint32_t oam16(unsigned offset) const noexcept { return load(&memory_.oam[oam_ + offset], 2); }

    // Indice de couleur du point (x, y) de l'objet, 0 pour transparent.
    unsigned texel(const Object& object, unsigned x, unsigned y) const noexcept {
        const auto colors256 = (object.attribute0 & 0x2000U) != 0;
        const auto tile = object.attribute2 & 0x3ffU;
        const auto tile_bytes = colors256 ? 64U : 32U;
        std::uint32_t address{};
        if ((control_ & 0x10U) != 0) {  // 1D : tuiles à la suite, pas de 32 << limite
            const auto boundary = (control_ >> 20U) & 3U;
            address = tile * (32U << boundary) + ((y / 8U) * (object.width / 8U) + x / 8U) * tile_bytes;
        } else {  // 2D : lignes de 32 tuiles de 32 octets
            address = tile * 32U + (y / 8U) * 1024U + (x / 8U) * tile_bytes;
        }
        address += colors256 ? (y % 8U) * 8U + x % 8U : (y % 8U) * 4U + (x % 8U) / 2U;
        const auto area = main_ ? Vram::obj_a : Vram::obj_b;
        const auto value = vram_.read(area, address & (main_ ? 0x3ffffU : 0x1ffffU), 1);
        if (colors256) return value;
        return (x & 1U) != 0 ? value >> 4U : value & 15U;
    }

    std::uint16_t color(const Object& object, unsigned index) const noexcept {
        const auto colors256 = (object.attribute0 & 0x2000U) != 0;
        const auto entry = colors256 ? index : (object.attribute2 >> 12U) * 16U + index;
        const auto base = (main_ ? 0x200U : 0x600U) + entry * 2U;
        return static_cast<std::uint16_t>(load(&memory_.palette[base & 0x7feU], 2) & 0x7fffU);
    }

private:
    bool main_;
    std::uint32_t control_;
    const Vram& vram_;
    const Memory& memory_;
    unsigned oam_;
};

void plot(ObjectLine& out, int x, std::uint16_t color, unsigned priority, unsigned mode) noexcept {
    if (x < 0 || x >= 256) return;
    const auto at = static_cast<std::size_t>(x);
    if (mode == 2) {  // fenêtre d'objets : rien n'est dessiné
        out.window[at] = true;
        return;
    }
    // Le premier objet dessiné (plus petit numéro) garde sa place à priorité égale.
    if ((out.color[at] & ObjectLine::transparent) == 0 && out.priority[at] <= priority) return;
    out.color[at] = color;
    out.priority[at] = static_cast<std::uint8_t>(priority);
    out.semi_transparent[at] = mode == 1;
}

void draw_object(const ObjectDrawer& drawer, const Object& object, unsigned line, ObjectLine& out) {
    const auto affine = (object.attribute0 & 0x100U) != 0;
    const auto doubled = affine && (object.attribute0 & 0x200U) != 0;
    const auto box_width = object.width << (doubled ? 1U : 0U);
    const auto box_height = object.height << (doubled ? 1U : 0U);
    const auto row = (line - (object.attribute0 & 0xffU)) & 0xffU;
    if (row >= box_height) return;
    auto left = static_cast<int>(object.attribute1 & 0x1ffU);
    if (left >= 256) left -= 512;
    const auto priority = (object.attribute2 >> 10U) & 3U;
    const auto mode = (object.attribute0 >> 10U) & 3U;
    if (!affine) {
        const auto y = (object.attribute1 & 0x2000U) != 0 ? object.height - 1 - row : row;
        for (unsigned column = 0; column < object.width; ++column) {
            const auto x = (object.attribute1 & 0x1000U) != 0 ? object.width - 1 - column : column;
            const auto index = drawer.texel(object, x, y);
            if (index != 0) plot(out, left + static_cast<int>(column), drawer.color(object, index), priority, mode);
        }
        return;
    }
    // Paramètres affines PA, PB, PC, PD (8.8 signés) : attribut 3 des objets 4n à 4n+3.
    const auto group = ((object.attribute1 >> 9U) & 31U) * 32U;
    const auto parameter = [&](unsigned slot) {
        return static_cast<std::int32_t>(static_cast<std::int16_t>(drawer.oam16(group + slot * 8U + 6U)));
    };
    const auto pa = parameter(0), pb = parameter(1), pc = parameter(2), pd = parameter(3);
    const auto dy = static_cast<std::int32_t>(row) - static_cast<std::int32_t>(box_height / 2);
    for (unsigned column = 0; column < box_width; ++column) {
        const auto dx = static_cast<std::int32_t>(column) - static_cast<std::int32_t>(box_width / 2);
        const auto tx = ((pa * dx + pb * dy) >> 8) + static_cast<std::int32_t>(object.width / 2);
        const auto ty = ((pc * dx + pd * dy) >> 8) + static_cast<std::int32_t>(object.height / 2);
        if (tx < 0 || ty < 0 || tx >= static_cast<std::int32_t>(object.width) || ty >= static_cast<std::int32_t>(object.height))
            continue;
        const auto index = drawer.texel(object, static_cast<unsigned>(tx), static_cast<unsigned>(ty));
        if (index != 0) plot(out, left + static_cast<int>(column), drawer.color(object, index), priority, mode);
    }
}

} // namespace

void draw_objects(bool main, std::uint32_t display_control, unsigned line, const Vram& vram, const Memory& memory,
                  ObjectLine& out) {
    std::fill(out.color.begin(), out.color.end(), ObjectLine::transparent);
    std::fill(out.priority.begin(), out.priority.end(), std::uint8_t{4});
    out.semi_transparent.fill(false);
    out.window.fill(false);
    const ObjectDrawer drawer{main, display_control, vram, memory};
    for (unsigned index = 0; index < 128; ++index) {
        const Object probe{index, drawer.oam16(index * 8U), drawer.oam16(index * 8U + 2U), drawer.oam16(index * 8U + 4U), 0, 0};
        const auto affine = (probe.attribute0 & 0x100U) != 0;
        if (!affine && (probe.attribute0 & 0x200U) != 0) continue;  // objet masqué
        const auto mode = (probe.attribute0 >> 10U) & 3U;
        const auto shape = probe.attribute0 >> 14U;
        if (mode == 3 || shape == 3) continue;  // objet bitmap (pas encore), forme interdite
        const auto size = object_sizes[shape][probe.attribute1 >> 14U];
        draw_object(drawer, {index, probe.attribute0, probe.attribute1, probe.attribute2, size[0], size[1]}, line, out);
    }
}

} // namespace ravenemu::nds
