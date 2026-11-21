#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * États instantanés du PPU : 72 champs dans un ordre fixe, FIFO, objets
 * retenus, deux images (ppu/frame_codec.hpp), VRAM, OAM et RAM couleur, puis
 * validation de tout ce qu'un état corrompu pourrait rendre incohérent.
 */

inline void Ppu::save(BinaryWriter& out, bool tagged_frames) const {
    constexpr int field_count = 72;
    out.i32(field_count);
    const std::array fields{
        lcdc_, stat_enable_, scy_, scx_, ly_, lyc_, bgp_, obp0_, obp1_, wy_, wx_,
        mode_, reported_mode_, reported_mode_target_, reported_mode_delay_,
        lcd_startup_line_ ? 1 : 0, lcd_startup_lines_remaining_, lyc_compare_delay_,
        frame_mode2_delay_ ? 1 : 0, speed_switch_video_access_, line_dot_, window_line_,
        lyc_equal_ ? 1 : 0,
        stat_line_ ? 1 : 0, vram_bank_, bcps_index_,
        bcps_auto_ ? 1 : 0, ocps_index_, ocps_auto_ ? 1 : 0,
        transfer_x_, window_started_line_ ? 1 : 0, window_y_condition_ ? 1 : 0,
        window_wx_glitch_armed_ ? 1 : 0, window_wx_glitch_x_,
        opri_dmg_priority_ ? 1 : 0, scx_latched_, startup_dots_remaining_,
        discard_pixels_remaining_, window_restart_dots_remaining_, window_initial_skip_,
        bg_fifo_head_, bg_fifo_size_, fetcher_window_ ? 1 : 0, fetcher_tile_x_,
        fetcher_phase_, fetcher_phase_dots_, fetcher_tile_, fetcher_attributes_,
        fetcher_row_, fetcher_tile_data_address_, fetcher_data_low_, fetcher_data_high_,
        obj_fifo_head_, obj_fifo_size_, obj_fetch_phase_, obj_fetch_phase_dots_remaining_,
        obj_fetch_output_overlap_remaining_,
        obj_fetch_slot_, obj_fetch_sprite_, obj_fetch_screen_x_, obj_fetch_raw_x_,
        obj_fetch_tile_, obj_fetch_attributes_, obj_fetch_row_, obj_fetch_address_,
        obj_fetch_data_low_, obj_fetch_data_high_,
        line_sprite_count_, considered_sprite_tile_count_,
        oam_scan_index_, entered_hblank_ ? 1 : 0, frame_ready_ ? 1 : 0,
    };
    static_assert(fields.size() == field_count);
    for (const int field : fields) out.i32(field);
    for (const auto& pixel : bg_fifo_) {
        out.u8(pixel.color); out.u8(pixel.palette); out.boolean(pixel.priority);
    }
    for (const auto& pixel : obj_fifo_) {
        out.u8(pixel.color); out.u8(pixel.palette); out.boolean(pixel.behind_background);
        out.u8(pixel.oam_index); out.u8(pixel.raw_x);
    }
    for (const bool used : sprite_fetch_consumed_) out.boolean(used);
    for (const int tile : considered_sprite_tiles_) out.i32(tile);
    for (const int sprite : sprite_indices_) out.i32(sprite);
    for (const int y : sprite_y_) out.i32(y);
    for (const int x : sprite_x_) out.i32(x);
    FrameCodec::save(out, working_frame_, tagged_frames);
    FrameCodec::save(out, completed_frame, tagged_frames);
    out.raw(vram); out.raw(oam); out.raw(bg_cram); out.raw(obj_cram);
}

/** `tagged_frames` vaut false pour un état au format 10 (images en i32). */
inline void Ppu::load(BinaryReader& in, bool tagged_frames) {
    if (in.i32() != 72) throw SaveStateError("État instantané corrompu (PPU)");
    lcdc_ = in.i32(); render_lcdc_ = lcdc_; stat_enable_ = in.i32(); scy_ = in.i32(); scx_ = in.i32();
    ly_ = in.i32(); lyc_ = in.i32(); bgp_ = in.i32(); obp0_ = in.i32(); obp1_ = in.i32();
    wy_ = in.i32(); wx_ = in.i32(); mode_ = in.i32(); reported_mode_ = in.i32();
    reported_mode_target_ = in.i32(); reported_mode_delay_ = in.i32();
    lcd_startup_line_ = in.i32() != 0;
    lcd_startup_lines_remaining_ = in.i32();
    lyc_compare_delay_ = in.i32();
    frame_mode2_delay_ = in.i32() != 0; speed_switch_video_access_ = in.i32();
    line_dot_ = in.i32();
    window_line_ = in.i32(); lyc_equal_ = in.i32() != 0; stat_line_ = in.i32() != 0;
    vram_bank_ = in.i32();
    bcps_index_ = in.i32(); bcps_auto_ = in.i32() != 0; ocps_index_ = in.i32();
    ocps_auto_ = in.i32() != 0; transfer_x_ = in.i32(); window_started_line_ = in.i32() != 0;
    window_y_condition_ = in.i32() != 0; window_wx_glitch_armed_ = in.i32() != 0;
    window_wx_glitch_x_ = in.i32();
    opri_dmg_priority_ = in.i32() != 0;
    scx_latched_ = in.i32(); startup_dots_remaining_ = in.i32(); discard_pixels_remaining_ = in.i32();
    window_restart_dots_remaining_ = in.i32(); window_initial_skip_ = in.i32();
    bg_fifo_head_ = std::clamp(in.i32(), 0, 15); bg_fifo_size_ = std::clamp(in.i32(), 0, 16);
    fetcher_window_ = in.i32() != 0; fetcher_tile_x_ = in.i32(); fetcher_phase_ = std::clamp(in.i32(), 0, 3);
    fetcher_phase_dots_ = std::max(0, in.i32()); fetcher_tile_ = in.i32(); fetcher_attributes_ = in.i32();
    fetcher_row_ = in.i32(); fetcher_tile_data_address_ = in.i32(); fetcher_data_low_ = in.i32();
    fetcher_data_high_ = in.i32();
    obj_fifo_head_ = in.i32(); obj_fifo_size_ = in.i32(); obj_fetch_phase_ = in.i32();
    obj_fetch_phase_dots_remaining_ = in.i32();
    obj_fetch_output_overlap_remaining_ = in.i32(); obj_fetch_slot_ = in.i32();
    obj_fetch_sprite_ = in.i32(); obj_fetch_screen_x_ = in.i32(); obj_fetch_raw_x_ = in.i32();
    obj_fetch_tile_ = in.i32(); obj_fetch_attributes_ = in.i32(); obj_fetch_row_ = in.i32();
    obj_fetch_address_ = in.i32(); obj_fetch_data_low_ = in.i32(); obj_fetch_data_high_ = in.i32();
    line_sprite_count_ = std::clamp(in.i32(), 0, 10);
    considered_sprite_tile_count_ = std::clamp(in.i32(), 0, 10);
    oam_scan_index_ = std::clamp(in.i32(), 0, 40);
    entered_hblank_ = in.i32() != 0; frame_ready_ = in.i32() != 0;
    // Aucun de ces champs n'indexe un tableau, mais des valeurs extrêmes
    // feraient déborder l'arithmétique signée du fetcher et des décomptes.
    // Les bornes couvrent toutes les valeurs que le PPU produit.
    const std::array byte_registers{
        lcdc_, stat_enable_, scy_, scx_, lyc_, bgp_, obp0_, obp1_, wy_, wx_, scx_latched_,
        fetcher_tile_, fetcher_attributes_, fetcher_data_low_, fetcher_data_high_,
    };
    const std::array counters{
        window_line_, startup_dots_remaining_, discard_pixels_remaining_,
        window_restart_dots_remaining_, window_initial_skip_, fetcher_tile_x_,
        fetcher_phase_dots_, fetcher_row_, fetcher_tile_data_address_,
    };
    if (std::any_of(byte_registers.begin(), byte_registers.end(),
                    [](int value) { return value < 0 || value > 0xff; }) ||
        std::any_of(counters.begin(), counters.end(),
                    [](int value) { return value < 0 || value > 0xffff; })) {
        throw SaveStateError("État instantané corrompu (registres PPU)");
    }
    validate_timing_state();
    if (window_wx_glitch_x_ < 0 || window_wx_glitch_x_ >= width ||
        (window_wx_glitch_armed_ &&
         (mode_ != mode_transfer || !window_started_line_ ||
          window_wx_glitch_x_ < transfer_x_))) {
        throw SaveStateError("État instantané corrompu (glitch WX PPU)");
    }
    for (auto& pixel : bg_fifo_) {
        pixel.color = in.u8(); pixel.palette = in.u8(); pixel.priority = in.boolean();
        if (pixel.color > 3 || pixel.palette > 7) {
            throw SaveStateError("État instantané corrompu (FIFO BG PPU)");
        }
    }
    for (auto& pixel : obj_fifo_) {
        pixel.color = in.u8(); pixel.palette = in.u8(); pixel.behind_background = in.boolean();
        pixel.oam_index = in.u8(); pixel.raw_x = in.u8();
    }
    validate_object_pipeline_state();
    for (auto&& used : sprite_fetch_consumed_) used = in.boolean();
    for (auto& tile : considered_sprite_tiles_) tile = in.i32();
    for (auto& sprite : sprite_indices_) sprite = in.i32();
    for (auto& y : sprite_y_) y = in.i32();
    for (auto& x : sprite_x_) x = in.i32();
    // Les entrées retenues par le scan OAM indexent `oam` lors du fetch
    // OBJ du mode 3 : un index hors des quarante entrées lirait hors du
    // tableau. Y/X sont des octets OAM bruts.
    for (std::size_t slot = 0; slot < sprite_indices_.size(); ++slot) {
        if (sprite_indices_[slot] < 0 || sprite_indices_[slot] >= 40 ||
            sprite_y_[slot] < 0 || sprite_y_[slot] > 0xff ||
            sprite_x_[slot] < 0 || sprite_x_[slot] > 0xff) {
            throw SaveStateError("État instantané corrompu (sprites retenus PPU)");
        }
    }
    FrameCodec::load(in, working_frame_, tagged_frames);
    FrameCodec::load(in, completed_frame, tagged_frames);
    in.raw(vram); in.raw(oam); in.raw(bg_cram); in.raw(obj_cram); rebuild_color_cache();
}

inline void Ppu::validate_timing_state() const {
    const auto valid_mode = [](int mode) { return mode >= mode_hblank && mode <= mode_transfer; };
    const int maximum_line_dot = lcd_startup_line_ ? 451 : 455;
    if (!valid_mode(mode_) || !valid_mode(reported_mode_) || !valid_mode(reported_mode_target_) ||
        reported_mode_delay_ < 0 || reported_mode_delay_ > 4 ||
        lcd_startup_lines_remaining_ < 0 || lcd_startup_lines_remaining_ > 3 ||
        lyc_compare_delay_ < 0 || lyc_compare_delay_ > 4 ||
        speed_switch_video_access_ < speed_switch_access_inactive ||
        speed_switch_video_access_ > speed_switch_access_all ||
        (speed_switch_active() && !gbc::cgb_features_enabled(hardware_mode_)) ||
        ly_ < 0 || ly_ > 153 || line_dot_ < 0 || line_dot_ > maximum_line_dot ||
        transfer_x_ < 0 || transfer_x_ > width || vram_bank_ < 0 || vram_bank_ > 1 ||
        bcps_index_ < 0 || bcps_index_ > 0x3f || ocps_index_ < 0 || ocps_index_ > 0x3f) {
        throw SaveStateError("État instantané corrompu (timing PPU)");
    }
    if ((lcd_startup_line_ && (ly_ != 0 || lcd_startup_lines_remaining_ == 0)) ||
        (reported_mode_delay_ > 0 &&
         (hardware_mode_ != gbc::HardwareMode::dmg || lcd_startup_lines_remaining_ == 0 ||
          reported_mode_ == reported_mode_target_)) ||
        (ly_ >= height && mode_ != mode_vblank) ||
        (ly_ < height && mode_ == mode_vblank) ||
        // Le mode 3 passe en H-Blank dès son 160e pixel : un état qui
        // le garderait au bout de la ligne écrirait hors de l'image.
        (mode_ == mode_transfer && transfer_x_ >= width)) {
        throw SaveStateError("État instantané incohérent (phase PPU)");
    }
    if (!lcd_enabled() &&
        (ly_ != 0 || line_dot_ != 0 || mode_ != mode_hblank ||
         reported_mode_ != mode_hblank || reported_mode_target_ != mode_hblank ||
         reported_mode_delay_ != 0 || lcd_startup_line_ || lcd_startup_lines_remaining_ != 0)) {
        throw SaveStateError("État instantané incohérent (LCD éteint)");
    }
}

inline void Ppu::validate_object_pipeline_state() const {
    if (obj_fifo_head_ < 0 || obj_fifo_head_ >= static_cast<int>(obj_fifo_.size()) ||
        obj_fifo_size_ < 0 || obj_fifo_size_ > static_cast<int>(obj_fifo_.size()) ||
        obj_fetch_phase_ < obj_fetch_idle || obj_fetch_phase_ > obj_fetch_high) {
        throw SaveStateError("État instantané corrompu (pipeline OBJ PPU)");
    }
    for (const auto& pixel : obj_fifo_) {
        if (pixel.color > 3 || pixel.palette > 7 ||
            (pixel.color != 0 && pixel.oam_index >= 40)) {
            throw SaveStateError("État instantané corrompu (FIFO OBJ PPU)");
        }
    }
    if (obj_fetch_phase_ == obj_fetch_idle) {
        if (obj_fetch_phase_dots_remaining_ != 0 || obj_fetch_slot_ != -1 ||
            obj_fetch_sprite_ != -1 || obj_fetch_output_overlap_remaining_ != 0) {
            throw SaveStateError("État instantané corrompu (fetch OBJ PPU inactif)");
        }
        return;
    }
    if (obj_fetch_phase_dots_remaining_ <= 0 || obj_fetch_phase_dots_remaining_ > 11 ||
        obj_fetch_output_overlap_remaining_ < 0 || obj_fetch_output_overlap_remaining_ > 3 ||
        (obj_fetch_output_overlap_remaining_ > 0 &&
         obj_fetch_phase_ != obj_fetch_oam && obj_fetch_phase_ != obj_fetch_low) ||
        obj_fetch_slot_ < 0 || obj_fetch_slot_ >= line_sprite_count_ ||
        obj_fetch_sprite_ < 0 || obj_fetch_sprite_ >= 40 ||
        obj_fetch_screen_x_ < -8 || obj_fetch_screen_x_ > 247 ||
        obj_fetch_raw_x_ < 0 || obj_fetch_raw_x_ > 255 ||
        obj_fetch_tile_ < 0 || obj_fetch_tile_ > 255 ||
        obj_fetch_attributes_ < 0 || obj_fetch_attributes_ > 255 ||
        obj_fetch_row_ < 0 || obj_fetch_row_ > 15 ||
        obj_fetch_address_ < 0 || obj_fetch_address_ >= 0x2000 ||
        obj_fetch_data_low_ < 0 || obj_fetch_data_low_ > 255 ||
        obj_fetch_data_high_ < 0 || obj_fetch_data_high_ > 255) {
        throw SaveStateError("État instantané corrompu (fetch OBJ PPU)");
    }
}

} // namespace ravenemu::cgb
