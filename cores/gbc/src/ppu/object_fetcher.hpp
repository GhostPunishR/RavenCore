#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * Fetcher des objets : choix de l'objet à la position courante, pénalité
 * du mode 3, lecture de ses octets, fusion dans la FIFO des objets selon leur
 * priorité, et annulation sur DMG quand LCDC.1 masque les objets.
 */

inline void Ppu::reset_object_fetch() noexcept {
    obj_fetch_phase_ = obj_fetch_idle;
    obj_fetch_phase_dots_remaining_ = 0;
    obj_fetch_slot_ = -1;
    obj_fetch_sprite_ = -1;
    obj_fetch_screen_x_ = 0;
    obj_fetch_raw_x_ = 0;
    obj_fetch_tile_ = 0;
    obj_fetch_attributes_ = 0;
    obj_fetch_row_ = 0;
    obj_fetch_address_ = 0;
    obj_fetch_data_low_ = 0;
    obj_fetch_data_high_ = 0;
    obj_fetch_output_overlap_remaining_ = 0;
}

inline void Ppu::reset_object_pipeline() noexcept {
    obj_fifo_.fill(ObjPixel{});
    obj_fifo_head_ = 0;
    obj_fifo_size_ = 0;
    reset_object_fetch();
}

inline bool Ppu::object_fetch_may_be_canceled() const noexcept {
    return hardware_mode_ == gbc::HardwareMode::dmg;
}

inline int Ppu::next_object_at_current_x() const noexcept {
    int best_slot = -1;
    int best_raw_x = 256;
    int best_oam_index = 40;
    for (int slot = 0; slot < line_sprite_count_; ++slot) {
        if (sprite_fetch_consumed_[static_cast<std::size_t>(slot)]) continue;
        const int raw_x = sprite_x_[static_cast<std::size_t>(slot)];
        const int screen_x = raw_x - 8;
        // Un fetch ne peut recouvrir que ses trois premiers dots. Éviter
        // le calcul de priorité de tuile tant que l'OBJ est hors de cette
        // petite fenêtre garde le chemin par pixel borné et sans travail
        // quadratique inutile.
        if (transfer_x_ < std::max(0, screen_x - 3) ||
            transfer_x_ > std::max(0, screen_x)) {
            continue;
        }
        const auto timing = object_timing(slot, screen_x);
        const int overlapped_dots = std::max(0, 6 - timing.penalty_dots);
        const int trigger_x = std::max(0, screen_x - overlapped_dots);
        if (trigger_x != transfer_x_) continue;
        const int oam_index = sprite_indices_[static_cast<std::size_t>(slot)];
        if (raw_x < best_raw_x || (raw_x == best_raw_x && oam_index < best_oam_index)) {
            best_slot = slot;
            best_raw_x = raw_x;
            best_oam_index = oam_index;
        }
    }
    return best_slot;
}

inline void Ppu::begin_object_fetch(int sprite_slot) noexcept {
    obj_fetch_slot_ = sprite_slot;
    obj_fetch_sprite_ = sprite_indices_[static_cast<std::size_t>(sprite_slot)];
    obj_fetch_raw_x_ = sprite_x_[static_cast<std::size_t>(sprite_slot)];
    obj_fetch_screen_x_ = obj_fetch_raw_x_ - 8;
    obj_fetch_tile_ = 0;
    obj_fetch_attributes_ = 0;
    obj_fetch_row_ = 0;
    obj_fetch_address_ = 0;
    obj_fetch_data_low_ = 0;
    obj_fetch_data_high_ = 0;

    const auto timing = object_timing(sprite_slot, obj_fetch_screen_x_);
    if (timing.first_for_tile &&
        considered_sprite_tile_count_ < static_cast<int>(considered_sprite_tiles_.size())) {
        considered_sprite_tiles_[static_cast<std::size_t>(considered_sprite_tile_count_++)] =
            timing.tile_key;
    }
    const int wait_dots = std::max(0, timing.penalty_dots - 6);
    obj_fetch_output_overlap_remaining_ = std::max(0, 6 - timing.penalty_dots);
    obj_fetch_phase_ = wait_dots > 0 ? obj_fetch_wait_bg : obj_fetch_oam;
    obj_fetch_phase_dots_remaining_ = wait_dots > 0 ? wait_dots : 2;
}

inline void Ppu::cancel_object_fetch() noexcept {
    // Le sprite reste marqué comme considéré. Le fetcher BG reprend au
    // point atteint avant l'interruption et le FIFO OBJ déjà présent est
    // conservé pour les pixels suivants.
    reset_object_fetch();
}

inline void Ppu::advance_object_fetch() noexcept {
    if (obj_fetch_phase_ == obj_fetch_wait_bg) tick_bg_fetcher();
    if (--obj_fetch_phase_dots_remaining_ > 0) return;

    switch (obj_fetch_phase_) {
    case obj_fetch_wait_bg:
        obj_fetch_phase_ = obj_fetch_oam;
        obj_fetch_phase_dots_remaining_ = 2;
        break;
    case obj_fetch_oam:
        latch_object_parameters();
        obj_fetch_phase_ = obj_fetch_low;
        obj_fetch_phase_dots_remaining_ = 2;
        break;
    case obj_fetch_low:
        obj_fetch_data_low_ = vram_byte(object_fetch_bank(), obj_fetch_address_);
        obj_fetch_phase_ = obj_fetch_high;
        obj_fetch_phase_dots_remaining_ = 2;
        break;
    case obj_fetch_high:
        obj_fetch_data_high_ = vram_byte(object_fetch_bank(), obj_fetch_address_ + 1);
        merge_fetched_object();
        reset_object_fetch();
        break;
    default:
        reset_object_fetch();
        break;
    }
}

inline void Ppu::latch_object_parameters() noexcept {
    if (!ppu_oam_available()) {
        obj_fetch_tile_ = 0xff;
        obj_fetch_attributes_ = 0xff;
    } else if (oam_dma_active_) {
        // Pendant le mode 3, le fetcher OBJ reçoit le mot 16 bits que le
        // DMA présente au port OAM, indépendamment de l'entrée demandée.
        obj_fetch_tile_ = oam_dma_bus_low_;
        obj_fetch_attributes_ = oam_dma_bus_high_;
    } else {
        const int index = obj_fetch_sprite_ * 4;
        obj_fetch_tile_ = oam[static_cast<std::size_t>(index + 2)];
        obj_fetch_attributes_ = oam[static_cast<std::size_t>(index + 3)];
    }
    const int sprite_height = (lcdc_ & 4) != 0 ? 16 : 8;
    if (sprite_height == 16) obj_fetch_tile_ &= 0xfe;
    const int sprite_y = sprite_y_[static_cast<std::size_t>(obj_fetch_slot_)] - 16;
    obj_fetch_row_ = ly_ - sprite_y;
    if ((obj_fetch_attributes_ & 0x40) != 0) {
        obj_fetch_row_ = sprite_height - 1 - obj_fetch_row_;
    }
    obj_fetch_address_ = (obj_fetch_tile_ * 16 + obj_fetch_row_ * 2) & 0x1fff;
}

inline int Ppu::object_fetch_bank() const noexcept {
    return gbc::cgb_features_enabled(hardware_mode_) ? (obj_fetch_attributes_ >> 3) & 1 : 0;
}

inline void Ppu::ensure_object_fifo_size(int size) noexcept {
    while (obj_fifo_size_ < size && obj_fifo_size_ < static_cast<int>(obj_fifo_.size())) {
        const int tail = (obj_fifo_head_ + obj_fifo_size_) & 15;
        obj_fifo_[static_cast<std::size_t>(tail)] = ObjPixel{};
        ++obj_fifo_size_;
    }
}

inline bool Ppu::object_pixel_has_priority(const ObjPixel& candidate, const ObjPixel& current) const noexcept {
    if (current.color == 0) return true;
    if (!opri_dmg_priority_) return candidate.oam_index < current.oam_index;
    return candidate.raw_x < current.raw_x ||
        (candidate.raw_x == current.raw_x && candidate.oam_index < current.oam_index);
}

inline void Ppu::merge_fetched_object() noexcept {
    ensure_object_fifo_size(8);
    const bool flipped = (obj_fetch_attributes_ & 0x20) != 0;
    const std::uint8_t palette = static_cast<std::uint8_t>(
        gbc::cgb_features_enabled(hardware_mode_)
            ? obj_fetch_attributes_ & 7
            : (obj_fetch_attributes_ >> 4) & 1);
    for (int source_x = 0; source_x < 8; ++source_x) {
        const int fifo_offset = obj_fetch_screen_x_ + source_x - transfer_x_;
        if (fifo_offset < 0 || fifo_offset >= obj_fifo_size_) continue;
        const int bit = flipped ? source_x : 7 - source_x;
        const int color = (((obj_fetch_data_high_ >> bit) & 1) << 1) |
            ((obj_fetch_data_low_ >> bit) & 1);
        if (color == 0) continue;
        ObjPixel candidate{
            static_cast<std::uint8_t>(color),
            palette,
            (obj_fetch_attributes_ & 0x80) != 0,
            static_cast<std::uint8_t>(obj_fetch_sprite_),
            static_cast<std::uint8_t>(obj_fetch_raw_x_),
        };
        const int index = (obj_fifo_head_ + fifo_offset) & 15;
        auto& current = obj_fifo_[static_cast<std::size_t>(index)];
        if (object_pixel_has_priority(candidate, current)) current = candidate;
    }
}

inline ObjPixel Ppu::pop_obj_pixel() noexcept {
    if (obj_fifo_size_ <= 0) return {};
    const auto result = obj_fifo_[static_cast<std::size_t>(obj_fifo_head_)];
    obj_fifo_[static_cast<std::size_t>(obj_fifo_head_)] = ObjPixel{};
    obj_fifo_head_ = (obj_fifo_head_ + 1) & 15;
    --obj_fifo_size_;
    return result;
}

inline Ppu::ObjectTiming Ppu::object_timing(int sprite_slot, int sprite_x) const noexcept {
    const int raw_x = sprite_x_[static_cast<std::size_t>(sprite_slot)];
    const int coordinate = fetcher_window_ ? sprite_x - (wx_ - 7) : sprite_x + scx_latched_;
    const int wrapped = coordinate & 0xff;
    const int tile_key = (fetcher_window_ ? 0x100 : 0) | (wrapped >> 3);
    bool first_for_tile = true;
    for (int i = 0; i < considered_sprite_tile_count_; ++i) {
        if (considered_sprite_tiles_[static_cast<std::size_t>(i)] == tile_key) {
            first_for_tile = false;
            break;
        }
    }
    if (raw_x == 0) {
        // Un OBJ à X=0 est rencontré pendant l'amorçage du pipeline :
        // le premier fetch recouvre quatre dots du fetch BG initial.
        // Les OBJ suivants sur la même tuile ne paient ensuite que les
        // six dots propres au fetch OBJ. Cette superposition est visible
        // dans le timing mode 2 -> mode 0, même si l'OBJ est entièrement
        // hors écran.
        if (!first_for_tile) return {6, tile_key, false};
        return {considered_sprite_tile_count_ > 0 ? 11 : 7, tile_key, true};
    }
    if (!first_for_tile) return {6, tile_key, false};
    const int fine = wrapped & 7;
    const int fetch_wait = std::max(0, 7 - fine - 2);
    if (considered_sprite_tile_count_ > 0) return {6 + fetch_wait, tile_key, true};
    // Au moment où le premier OBJ d'une tuile atteint le bord du FIFO,
    // trois dots de son attente (quatre exactement à la frontière de
    // tuile) recouvrent le fetch BG déjà en cours.
    return {6 + fetch_wait - (fine == 0 ? 4 : 3), tile_key, true};
}

} // namespace ravenemu::cgb
