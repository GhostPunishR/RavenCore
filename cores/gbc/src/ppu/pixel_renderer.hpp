#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * Rendu d'un pixel sorti des FIFO : mélange du fond et de l'objet selon
 * leurs priorités, puis couleur, par les palettes du DMG, la compatibilité de
 * la GBC ou la RAM couleur.
 */

inline void Ppu::render_fifo_pixel(int x, const BgPixel& pixel, const ObjPixel& object) {
    const int base = ly_ * width;
    if (!ppu_cram_available() && gbc::is_cgb_hardware(hardware_mode_)) {
        working_frame_[static_cast<std::size_t>(base + x)] =
            static_cast<std::int32_t>(0xff000000U);
        return;
    }
    if (hardware_mode_ == gbc::HardwareMode::cgb_native) {
        bg_color_line_[static_cast<std::size_t>(x)] = pixel.color;
        bg_priority_line_[static_cast<std::size_t>(x)] = pixel.priority;
        working_frame_[static_cast<std::size_t>(base + x)] =
            bg_argb_[static_cast<std::size_t>(pixel.palette * 4 + pixel.color)];
        render_object_cgb(base, x, object);
        return;
    }

    const int color = (render_lcdc_ & 1) != 0 ? pixel.color : 0;
    bg_color_line_[static_cast<std::size_t>(x)] = color;
    bg_priority_line_[static_cast<std::size_t>(x)] = false;
    const int shade = (bgp_ >> (color * 2)) & 3;
    working_frame_[static_cast<std::size_t>(base + x)] =
        hardware_mode_ == gbc::HardwareMode::cgb_compatibility
            ? bg_argb_[static_cast<std::size_t>(shade)]
            : shade;
    render_object_dmg(base, x, object);
}

inline void Ppu::render_object_dmg(int base, int x, const ObjPixel& object) noexcept {
    if ((render_lcdc_ & 2) == 0 || object.color == 0) return;
    if (object.behind_background && bg_color_line_[static_cast<std::size_t>(x)] != 0) return;
    const int palette = object.palette != 0 ? obp1_ : obp0_;
    const int shade = (palette >> (object.color * 2)) & 3;
    if (hardware_mode_ == gbc::HardwareMode::cgb_compatibility) {
        working_frame_[static_cast<std::size_t>(base + x)] =
            obj_argb_[static_cast<std::size_t>(object.palette * 4 + shade)];
    } else {
        working_frame_[static_cast<std::size_t>(base + x)] = shade;
    }
}

inline void Ppu::render_object_cgb(int base, int x, const ObjPixel& object) noexcept {
    if ((render_lcdc_ & 2) == 0 || object.color == 0) return;
    const bool bg_visible = bg_color_line_[static_cast<std::size_t>(x)] != 0;
    if (bg_visible && (render_lcdc_ & 1) != 0 &&
        (bg_priority_line_[static_cast<std::size_t>(x)] || object.behind_background)) {
        return;
    }
    working_frame_[static_cast<std::size_t>(base + x)] =
        obj_argb_[static_cast<std::size_t>(object.palette * 4 + object.color)];
}

} // namespace ravenemu::cgb
