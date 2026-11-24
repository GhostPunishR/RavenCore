#include "gpu/engine2d.hpp"

#include <algorithm>

// Composition d'une ligne (GBATEK, « LCD I/O Window Feature » et « LCD I/O
// Color Special Effects ») : la fenêtre du point choisit les calques et les
// effets permis ; les deux calques visibles du dessus, dans l'ordre des
// priorités, se mélangent selon BLDCNT, BLDALPHA et BLDY.
namespace ravenemu::nds {

namespace {

constexpr unsigned object_layer = 4;
constexpr unsigned backdrop_layer = 5;

// Intervalle d'une fenêtre sur 8 bits ; un début après la fin passe par le
// bord. Horizontalement, X2 = 0 vaut 256, et X1 = X2 = 0 n'affiche rien.
bool inside(unsigned value, std::uint16_t range, bool horizontal) noexcept {
    const auto first = (range >> 8U) & 0xffU;
    auto last = static_cast<unsigned>(range & 0xffU);
    if (horizontal) {
        if (first == 0 && last == 0) return false;
        if (last == 0) last = 256;
    }
    return first <= last ? value >= first && value < last : value >= first || value < last;
}

std::uint16_t blend(std::uint16_t top, std::uint16_t below, unsigned eva, unsigned evb) noexcept {
    std::uint16_t result = 0;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        const auto a = (std::uint32_t{top} >> shift) & 31U;
        const auto b = (std::uint32_t{below} >> shift) & 31U;
        result = static_cast<std::uint16_t>(result | (std::min(31U, (a * eva + b * evb) >> 4U) << shift));
    }
    return result;
}

std::uint16_t fade(std::uint16_t color, unsigned evy, bool brighter) noexcept {
    std::uint16_t result = 0;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        const auto value = (std::uint32_t{color} >> shift) & 31U;
        const auto faded = brighter ? value + (((31U - value) * evy) >> 4U) : value - ((value * evy) >> 4U);
        result = static_cast<std::uint16_t>(result | (faded << shift));
    }
    return result;
}

} // namespace

// Calques permis au point (bits 0-3 fonds, 4 objets, 5 effets) : WIN0, puis
// WIN1, puis la fenêtre d'objets, sinon l'extérieur ; tout sans fenêtre.
std::uint32_t Engine2d::window_mask(unsigned x, unsigned line, const ObjectLine* objects) const noexcept {
    const auto control = display_control();
    if ((control & 0xe000U) == 0) return 0x3fU;
    const auto y = line & 0xffU;  // seuls 8 bits de la ligne sont comparés
    if ((control & 0x2000U) != 0 && inside(x, half(0x40), true) && inside(y, half(0x44), false)) return half(0x48) & 0x3fU;
    if ((control & 0x4000U) != 0 && inside(x, half(0x42), true) && inside(y, half(0x46), false))
        return (half(0x48) >> 8U) & 0x3fU;
    if ((control & 0x8000U) != 0 && objects != nullptr && objects->window[x]) return (half(0x4a) >> 8U) & 0x3fU;
    return half(0x4a) & 0x3fU;
}

void Engine2d::compose_pixels(unsigned line, const Layers& layers, const ObjectLine* objects, std::uint16_t backdrop,
                              std::span<std::uint16_t, width> out) const {
    const auto effect_control = half(0x50);
    const auto eva = std::min<unsigned>(half(0x52) & 31U, 16U);
    const auto evb = std::min<unsigned>((half(0x52) >> 8U) & 31U, 16U);
    const auto evy = std::min<unsigned>(half(0x54) & 31U, 16U);
    const auto objects_on = objects != nullptr && (display_control() & 0x1000U) != 0;
    for (unsigned x = 0; x < width; ++x) {
        const auto mask = window_mask(x, line, objects);
        std::array<unsigned, 2> id{backdrop_layer, backdrop_layer};
        std::array<std::uint16_t, 2> color{backdrop, backdrop};
        unsigned found = 0;
        const auto take = [&](unsigned layer, std::uint16_t value) {
            if (found < 2) {
                id[found] = layer;
                color[found] = value;
                ++found;
            }
        };
        // Par priorité croissante : l'objet passe devant les fonds de même priorité.
        for (unsigned priority = 0; priority < 4 && found < 2; ++priority) {
            if (objects_on && (mask & 0x10U) != 0 && (objects->color[x] & ObjectLine::transparent) == 0 &&
                objects->priority[x] == priority)
                take(object_layer, objects->color[x]);
            for (unsigned bg = 0; bg < 4; ++bg) {
                if (!layers.present[bg] || (mask & (1U << bg)) == 0 || (bg_control(bg) & 3U) != priority) continue;
                if ((layers.colors[bg][x] & 0x8000U) == 0) take(bg, layers.colors[bg][x]);
            }
        }
        auto result = color[0];
        if ((mask & 0x20U) != 0) {
            const auto first_target = (effect_control >> id[0]) & 1U;
            const auto second_target = (effect_control >> (8U + id[1])) & 1U;
            const auto mode = (effect_control >> 6U) & 3U;
            const auto semi = id[0] == object_layer && objects->semi_transparent[x];
            if ((semi || (mode == 1 && first_target != 0)) && second_target != 0) result = blend(color[0], color[1], eva, evb);
            else if (mode >= 2 && first_target != 0) result = fade(color[0], evy, mode == 2);
        }
        out[x] = result;
    }
}

} // namespace ravenemu::nds
