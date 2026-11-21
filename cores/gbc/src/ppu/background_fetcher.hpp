#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * Fetcher du fond et de la window : lecture de l'index, des attributs et des
 * deux octets de chaque tuile, FIFO du fond, démarrage de la window
 * (Pan Docs, Pixel FIFO).
 */

inline bool Ppu::window_enabled_for_rendering() const noexcept {
    if ((lcdc_ & 0x20) == 0) return false;
    return gbc::cgb_features_enabled(hardware_mode_) || (lcdc_ & 1) != 0;
}

inline bool Ppu::window_should_start() const noexcept {
    if (window_started_line_ || !window_y_condition_ || !window_enabled_for_rendering() || wx_ > 166) return false;
    return transfer_x_ >= std::max(0, wx_ - 7);
}

inline void Ppu::start_window_fetch() noexcept {
    window_started_line_ = true;
    reset_bg_fetcher(true);
    const bool wx_zero_shortening = wx_ == 0 && (scx_latched_ & 7) != 0;
    window_restart_dots_remaining_ = wx_zero_shortening ? 5 : 6;
    window_initial_skip_ = std::max(0, 7 - wx_);
    if (wx_ == 0) window_initial_skip_ += scx_latched_ & 7;
}

inline void Ppu::reset_bg_fetcher(bool window) noexcept {
    bg_fifo_head_ = 0;
    bg_fifo_size_ = 0;
    fetcher_window_ = window;
    fetcher_tile_x_ = 0;
    fetcher_phase_ = 0;
    fetcher_phase_dots_ = 2;
    fetcher_tile_ = 0;
    fetcher_attributes_ = 0;
    fetcher_row_ = 0;
    fetcher_data_low_ = 0;
    fetcher_data_high_ = 0;
}

inline void Ppu::tick_bg_fetcher() noexcept {
    // Pan Docs, Pixel FIFO : les pixels ne sont poussés que dans une FIFO
    // BG vide. Le fetcher ne prend donc qu'une tuile d'avance sur la
    // sortie : il lit les registres (SCY, SCX, LCDC) et la VRAM d'une
    // tuile pendant la sortie de la précédente. Mealybug Tearoom
    // (m3_scy_change, m3_lcdc_bg_map_change, m3_scx_high_5_bits) et AGE
    // (m3-bg-lcdc) le mesurent ; la durée du mode 3 ne change pas.
    if (fetcher_phase_ == 3) {
        if (bg_fifo_size_ == 0) push_fetched_tile();
        return;
    }
    if (--fetcher_phase_dots_ > 0) return;

    switch (fetcher_phase_) {
    case 0: fetch_tile_index(); fetcher_phase_ = 1; fetcher_phase_dots_ = 2; break;
    case 1: fetch_tile_low(); fetcher_phase_ = 2; fetcher_phase_dots_ = 2; break;
    default:
        fetch_tile_high();
        if (bg_fifo_size_ == 0) push_fetched_tile(); else fetcher_phase_ = 3;
        break;
    }
}

inline void Ppu::fetch_tile_index() noexcept {
    const int y = fetcher_window_ ? window_line_ : ((ly_ + scy_) & 0xff);
    // Les trois bits fins de SCX ne servent qu'au discard initial, mais
    // le fetcher relit les bits de tuile à chaque étape Get Tile. Une
    // écriture en mode 3 peut donc changer les tuiles encore non fetchées.
    const int tile_x = fetcher_window_ ? fetcher_tile_x_ : ((scx_ >> 3) + fetcher_tile_x_) & 31;
    const int map = fetcher_window_ ? ((render_lcdc_ & 0x40) != 0 ? 0x1c00 : 0x1800)
                                    : ((lcdc_ & 8) != 0 ? 0x1c00 : 0x1800);
    const int map_address = map + ((y >> 3) & 31) * 32 + tile_x;
    fetcher_tile_ = vram_byte(0, map_address);
    fetcher_attributes_ = gbc::cgb_features_enabled(hardware_mode_) ? vram_byte(1, map_address) : 0;
    fetcher_row_ = y & 7;
    if ((fetcher_attributes_ & 0x40) != 0) fetcher_row_ = 7 - fetcher_row_;
}

inline int Ppu::fetcher_bank() const noexcept {
    return gbc::cgb_features_enabled(hardware_mode_) ? (fetcher_attributes_ >> 3) & 1 : 0;
}

/**
 * Ligne de tuile du BG. Sur DMG, elle suit SCY jusqu'à chacune des deux
 * lectures de données ; sur CGB, elle reste celle de la lecture de l'index
 * et des attributs (retournement vertical compris). Mealybug Tearoom,
 * m3_scy_change, le mesure sur DMG et CGB-D. La window a sa propre ligne,
 * indépendante de SCY.
 */
inline void Ppu::refresh_fetcher_row() noexcept {
    if (!fetcher_window_ && hardware_mode_ == gbc::HardwareMode::dmg) fetcher_row_ = (ly_ + scy_) & 7;
}

inline void Ppu::fetch_tile_low() noexcept {
    refresh_fetcher_row();
    const int base = (lcdc_ & 0x10) != 0
        ? fetcher_tile_ * 16
        : 0x1000 + static_cast<std::int8_t>(fetcher_tile_) * 16;
    fetcher_tile_data_address_ = base + fetcher_row_ * 2;
    fetcher_data_low_ = vram_byte(fetcher_bank(), fetcher_tile_data_address_);
}

// La zone de tuile (LCDC.4) reste celle de l'octet bas.
inline void Ppu::fetch_tile_high() noexcept {
    refresh_fetcher_row();
    fetcher_tile_data_address_ = (fetcher_tile_data_address_ & ~0x0f) + fetcher_row_ * 2;
    fetcher_data_high_ = vram_byte(fetcher_bank(), fetcher_tile_data_address_ + 1);
}

inline void Ppu::push_fetched_tile() noexcept {
    const bool flipped = gbc::cgb_features_enabled(hardware_mode_) && (fetcher_attributes_ & 0x20) != 0;
    for (int pixel = 0; pixel < 8 && bg_fifo_size_ < 16; ++pixel) {
        const int bit = flipped ? pixel : 7 - pixel;
        const int color = (((fetcher_data_high_ >> bit) & 1) << 1) | ((fetcher_data_low_ >> bit) & 1);
        const int tail = (bg_fifo_head_ + bg_fifo_size_) & 15;
        bg_fifo_[static_cast<std::size_t>(tail)] = BgPixel{
            static_cast<std::uint8_t>(color),
            static_cast<std::uint8_t>(fetcher_attributes_ & 7),
            (fetcher_attributes_ & 0x80) != 0,
        };
        ++bg_fifo_size_;
    }
    ++fetcher_tile_x_;
    fetcher_phase_ = 0;
    fetcher_phase_dots_ = 2;
}

inline BgPixel Ppu::pop_bg_pixel() noexcept {
    const auto result = bg_fifo_[static_cast<std::size_t>(bg_fifo_head_)];
    bg_fifo_head_ = (bg_fifo_head_ + 1) & 15;
    --bg_fifo_size_;
    return result;
}

inline void Ppu::discard_window_prefix() noexcept {
    while (window_initial_skip_ > 0 && bg_fifo_size_ > 0) {
        static_cast<void>(pop_bg_pixel());
        --window_initial_skip_;
    }
}

} // namespace ravenemu::cgb
