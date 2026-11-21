#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * Port OAM du PPU : scan des quarante objets en mode 2, mot que le DMA OAM
 * présente au fetcher d'objets, et bug de corruption de l'OAM du DMG
 * (ppu/oam_corruption.hpp).
 */

/** Bug de corruption de l'OAM du DMG pendant le mode 2 (voir ppu/oam_corruption.hpp). */
inline void Ppu::corrupt_oam(OamCorruption kind) noexcept {
    if (hardware_mode_ != gbc::HardwareMode::dmg || !lcd_enabled() || mode_ != mode_oam) return;
    // Le PPU lit une ligne par M-cycle : au moment de l'accès CPU, c'est
    // la ligne line_dot_ / 4 (Blargg, oam_bug 4 et 5 : la corruption
    // commence au début de la première ligne visible).
    corrupt_oam_row(oam, line_dot_ / 4, kind);
}

/** Prend le port OAM pour un nouveau DMA. Le mot vu par le fetcher OBJ
 * reste indéfini jusqu'au premier octet effectivement transféré. */
inline void Ppu::begin_oam_dma() noexcept {
    oam_dma_active_ = true;
    oam_dma_bus_low_ = 0xff;
    oam_dma_bus_high_ = 0xff;
}

inline void Ppu::write_oam_dma_byte(int index, int value) noexcept {
    if (index < 0 || index >= static_cast<int>(oam.size())) return;
    oam[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value);
    const int word_index = index & ~1;
    oam_dma_bus_low_ = oam[static_cast<std::size_t>(word_index)];
    oam_dma_bus_high_ = oam[static_cast<std::size_t>(word_index + 1)];
}

inline void Ppu::restore_oam_dma_bus(int transferred_bytes) noexcept {
    if (transferred_bytes <= 0) {
        begin_oam_dma();
        return;
    }
    const int last_index = std::clamp(transferred_bytes - 1, 0,
                                      static_cast<int>(oam.size()) - 1);
    oam_dma_active_ = true;
    const int word_index = last_index & ~1;
    oam_dma_bus_low_ = oam[static_cast<std::size_t>(word_index)];
    oam_dma_bus_high_ = oam[static_cast<std::size_t>(word_index + 1)];
}

inline void Ppu::end_oam_dma() noexcept {
    oam_dma_active_ = false;
    oam_dma_bus_low_ = 0xff;
    oam_dma_bus_high_ = 0xff;
}

inline void Ppu::advance_oam_scan() noexcept {
    // Mode 2 inspecte quarante entrées sur 80 dots. Les coordonnées Y/X
    // des dix premières entrées retenues sont latched dans le tampon
    // interne ; un OAM DMA ultérieur ne peut donc plus les réécrire.
    if ((line_dot_ & 1) == 0 || oam_scan_index_ >= 40) return;
    const int sprite = oam_scan_index_++;
    const bool port_available = ppu_oam_available() && !oam_dma_active_;
    const int raw_y = port_available ? oam[static_cast<std::size_t>(sprite * 4)] : 0xff;
    const int raw_x = port_available ? oam[static_cast<std::size_t>(sprite * 4 + 1)] : 0xff;
    const int y = raw_y - 16;
    const int sprite_height = (lcdc_ & 4) != 0 ? 16 : 8;
    if (line_sprite_count_ >= 10 || ly_ < y || ly_ >= y + sprite_height) return;
    const auto slot = static_cast<std::size_t>(line_sprite_count_++);
    sprite_indices_[slot] = sprite;
    sprite_y_[slot] = raw_y;
    sprite_x_[slot] = raw_x;
}

} // namespace ravenemu::cgb
