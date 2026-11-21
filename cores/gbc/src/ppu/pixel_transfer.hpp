#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * Mode 3 : démarrage du transfert, arbitrage entre le fetcher du fond et
 * celui des objets, rejet des pixels fins de SCX et sortie d'un pixel par
 * point (Pan Docs, Pixel FIFO).
 */

inline void Ppu::begin_transfer() noexcept {
    transfer_x_ = 0;
    scx_latched_ = scx_;
    startup_dots_remaining_ = 12;
    discard_pixels_remaining_ = scx_latched_ & 7;
    window_restart_dots_remaining_ = 0;
    window_initial_skip_ = 0;
    window_started_line_ = false;
    window_wx_glitch_armed_ = false;
    window_wx_glitch_x_ = 0;
    sprite_fetch_consumed_.fill(false);
    considered_sprite_tiles_.fill(-1);
    considered_sprite_tile_count_ = 0;
    reset_object_pipeline();
    reset_bg_fetcher(false);
    set_mode(mode_transfer);
}

inline void Ppu::advance_transfer() {
    if (obj_fetch_phase_ != obj_fetch_idle) {
        if (object_fetch_may_be_canceled() && (lcdc_ & 2) == 0) {
            cancel_object_fetch();
        } else {
            const bool output_continues = obj_fetch_output_overlap_remaining_ > 0;
            if (output_continues) --obj_fetch_output_overlap_remaining_;
            advance_object_fetch();
            if (output_continues) advance_background_output();
            return;
        }
    }

    if (window_should_start()) {
        start_window_fetch();
    }

    for (;;) {
        const int sprite_slot = next_object_at_current_x();
        if (sprite_slot < 0) break;
        sprite_fetch_consumed_[static_cast<std::size_t>(sprite_slot)] = true;
        // Sur matériel CGB, le fetch OBJ continue même lorsque LCDC.1
        // masque sa sortie. Sur DMG, un OBJ rencontré alors que ce bit est
        // à zéro est simplement consommé par le séquenceur.
        if (object_fetch_may_be_canceled() && (lcdc_ & 2) == 0) continue;
        begin_object_fetch(sprite_slot);
        const bool output_continues = obj_fetch_output_overlap_remaining_ > 0;
        if (output_continues) --obj_fetch_output_overlap_remaining_;
        advance_object_fetch();
        if (output_continues) advance_background_output();
        return;
    }

    advance_background_output();
}

inline void Ppu::advance_background_output() {
    tick_bg_fetcher();
    if (startup_dots_remaining_ > 0) {
        --startup_dots_remaining_;
        return;
    }
    if (window_restart_dots_remaining_ > 0) {
        --window_restart_dots_remaining_;
        if (window_restart_dots_remaining_ == 0) discard_window_prefix();
        return;
    }
    if (window_initial_skip_ > 0) {
        discard_window_prefix();
        if (window_initial_skip_ > 0) return;
    }
    if (discard_pixels_remaining_ > 0) {
        if (bg_fifo_size_ > 0) {
            // SCX décale uniquement le BG. Si la window a déjà remplacé
            // la FIFO (WX <= 7), conserver ses pixels tout en terminant
            // la pénalité de démarrage SCX. Le cas WX=0 est traité
            // séparément par window_initial_skip_. Pan Docs, Pixel FIFO :
            // https://gbdev.io/pandocs/pixel_fifo.html
            if (!fetcher_window_) static_cast<void>(pop_bg_pixel());
            --discard_pixels_remaining_;
        }
        return;
    }
    if (bg_fifo_size_ <= 0) return;

    const bool inject_window_glitch = window_wx_glitch_armed_ &&
        transfer_x_ == window_wx_glitch_x_;
    render_fifo_pixel(transfer_x_, pop_bg_pixel(), pop_obj_pixel());
    if (inject_window_glitch) {
        const int tail = (bg_fifo_head_ + bg_fifo_size_) & 15;
        bg_fifo_[static_cast<std::size_t>(tail)] = BgPixel{};
        ++bg_fifo_size_;
        window_wx_glitch_armed_ = false;
    }
    ++transfer_x_;
    if (transfer_x_ >= width) {
        if (window_started_line_) ++window_line_;
        set_mode(mode_hblank);
        entered_hblank_ = true;
    }
}

} // namespace ravenemu::cgb
