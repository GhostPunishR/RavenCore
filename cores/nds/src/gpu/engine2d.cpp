#include "gpu/engine2d.hpp"
#include "gpu/object_renderer.hpp"

#include <algorithm>

namespace ravenemu::nds {

namespace {

constexpr std::uint16_t white = 0x7fff;
constexpr std::uint16_t transparent = 0x8000;  // bit 15 : point vide d'un calque

std::uint16_t palette_color(const Memory& memory, std::uint32_t offset) noexcept {
    return static_cast<std::uint16_t>(load(&memory.palette[offset & 0x7feU], 2) & 0x7fffU);
}

} // namespace

std::uint32_t Engine2d::read(std::uint32_t offset) const noexcept {
    // Les défilements (0x10-0x1F) ne se relisent pas.
    if (offset >= 0x10 && offset < 0x20) return 0;
    return offset < 0x70 ? registers_[offset / 4] : 0U;
}

void Engine2d::write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask) noexcept {
    if (offset >= 0x70) return;
    auto& word = registers_[offset / 4];
    word = (word & ~mask) | (value & mask);
}

// Fonds en mode texte selon le mode de fond (DISPCNT bits 0-2) ; BG0 passe au
// calque 3D quand DISPCNT bit 3 le demande (moteur A seul).
bool Engine2d::text_layer(unsigned bg) const noexcept {
    const auto control = display_control();
    const auto mode = control & 7U;
    switch (bg) {
    case 0: return mode != 6 && !(main_ && (control & 8U) != 0);
    case 1: return mode <= 5;
    case 2: return mode == 0 || mode == 1 || mode == 3;
    default: return mode == 0;
    }
}

void Engine2d::render_line(unsigned line, const Vram& vram, const Memory& memory,
                           std::span<std::uint16_t, width> out) const {
    const auto control = display_control();
    const auto display_mode = (control >> 16U) & (main_ ? 3U : 1U);
    if (display_mode == 0 || (control & 0x80U) != 0) {  // éteint ou blanc forcé
        std::fill(out.begin(), out.end(), white);
        return;
    }
    if (display_mode == 2) {  // VRAM affichée telle quelle, banque A à D
        const auto bank = (control >> 18U) & 3U;
        for (unsigned x = 0; x < width; ++x) out[x] = vram.bank_halfword(bank, (line * width + x) * 2U) & 0x7fffU;
        return;
    }
    if (display_mode == 3) {  // RAM principale par la FIFO d'affichage : pas encore
        std::fill(out.begin(), out.end(), std::uint16_t{0});
        return;
    }
    const auto palette = main_ ? 0x000U : 0x400U;
    std::fill(out.begin(), out.end(), palette_color(memory, palette));
    // Priorité du point visible : 4 pour la couleur de fond.
    std::array<std::uint8_t, width> shown{};
    shown.fill(4);
    // Du moins prioritaire au plus prioritaire : priorité 3 à 0, BG3 à BG0.
    for (unsigned priority = 4; priority-- > 0;) {
        for (unsigned bg = 4; bg-- > 0;) {
            if ((control & (0x100U << bg)) == 0 || (bg_control(bg) & 3U) != priority || !text_layer(bg)) continue;
            std::array<std::uint16_t, width> layer{};
            draw_text_layer(bg, line, vram, memory, layer);
            for (unsigned x = 0; x < width; ++x) {
                if ((layer[x] & transparent) != 0) continue;
                out[x] = layer[x];
                shown[x] = static_cast<std::uint8_t>(priority);
            }
        }
    }
    // Un objet passe devant les fonds de priorité égale ou moindre.
    if ((control & 0x1000U) == 0) return;
    ObjectLine objects;
    draw_objects(main_, control, line, vram, memory, objects);
    for (unsigned x = 0; x < width; ++x)
        if ((objects.color[x] & ObjectLine::transparent) == 0 && objects.priority[x] <= shown[x]) out[x] = objects.color[x];
}

// GBATEK, « LCD VRAM BG Screen Data Format » : blocs de 32 × 32 entrées de
// 16 bits (tuile, retournements, palette), tuiles de 8 × 8 points.
void Engine2d::draw_text_layer(unsigned bg, unsigned line, const Vram& vram, const Memory& memory,
                               std::span<std::uint16_t, width> colors) const {
    const auto control = bg_control(bg);
    const auto display = display_control();
    const auto area = main_ ? Vram::bg_a : Vram::bg_b;
    const auto area_mask = main_ ? 0x7ffffU : 0x1ffffU;
    const auto char_base = ((control >> 2U) & 15U) * 0x4000U + (main_ ? ((display >> 24U) & 7U) * 0x10000U : 0U);
    const auto screen_base = ((control >> 8U) & 31U) * 0x800U + (main_ ? ((display >> 27U) & 7U) * 0x10000U : 0U);
    const auto size = (control >> 14U) & 3U;
    const auto wide = (size & 1U) != 0;
    const auto tall = (size & 2U) != 0;
    const auto colors256 = (control & 0x80U) != 0;
    const auto palette = main_ ? 0x000U : 0x400U;
    const auto y = (line + bg_offset(bg, true)) & (tall ? 511U : 255U);
    const auto hofs = bg_offset(bg, false);
    for (unsigned x = 0; x < width; ++x) {
        const auto px = (x + hofs) & (wide ? 511U : 255U);
        const auto block = (px >> 8U) + (y >> 8U) * (wide ? 2U : 1U);
        const auto entry_address = screen_base + block * 0x800U + ((y & 255U) >> 3U) * 64U + ((px & 255U) >> 3U) * 2U;
        const auto entry = vram.read(area, entry_address & area_mask, 2);
        const auto tx = (entry & 0x400U) != 0 ? 7U - (px & 7U) : px & 7U;
        const auto ty = (entry & 0x800U) != 0 ? 7U - (y & 7U) : y & 7U;
        const auto tile = entry & 0x3ffU;
        std::uint32_t index{};
        if (colors256) {
            index = vram.read(area, (char_base + tile * 64U + ty * 8U + tx) & area_mask, 1);
        } else {
            const auto pair = vram.read(area, (char_base + tile * 32U + ty * 4U + tx / 2U) & area_mask, 1);
            index = (tx & 1U) != 0 ? pair >> 4U : pair & 15U;
            if (index != 0) index += (entry >> 12U) * 16U;
        }
        colors[x] = index == 0 ? transparent : palette_color(memory, palette + index * 2U);
    }
}

std::uint32_t to_argb(std::uint16_t color, std::uint32_t master_brightness) noexcept {
    const auto factor = std::min<std::uint32_t>(master_brightness & 31U, 16U);
    const auto brightness_mode = (master_brightness >> 14U) & 3U;
    std::uint32_t argb = 0xff00'0000U;
    for (unsigned channel = 0; channel < 3; ++channel) {
        const auto five = (std::uint32_t{color} >> (channel * 5U)) & 31U;
        auto six = (five << 1U) | (five >> 4U);
        if (brightness_mode == 1) six += (63U - six) * factor / 16U;
        else if (brightness_mode == 2) six -= six * factor / 16U;
        const auto eight = (six << 2U) | (six >> 4U);
        argb |= eight << (16U - channel * 8U);  // R, G, B
    }
    return argb;
}

} // namespace ravenemu::nds
