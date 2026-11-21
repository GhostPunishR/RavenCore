#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * Séquencement du PPU : lignes de 456 points, modes 2, 3, 0 et 1, allumage
 * et extinction de l'écran (LCDC.7), interruptions STAT et comparaison LYC.
 */

inline void Ppu::write_lcdc(int value) noexcept {
    const bool was_enabled = lcd_enabled();
    const bool window_was_enabled = (lcdc_ & 0x20) != 0;
    lcdc_ = byte(value);
    render_lcdc_ = lcdc_;
    if (gbc::is_cgb_hardware(hardware_mode_) && window_was_enabled && (lcdc_ & 0x20) == 0) {
        window_y_condition_ = false;
    }
    if (was_enabled && !lcd_enabled()) {
        ly_ = 0; line_dot_ = 0; mode_ = mode_hblank; reported_mode_ = mode_hblank;
        reported_mode_target_ = mode_hblank; reported_mode_delay_ = 0; lcd_startup_line_ = false;
        lcd_startup_lines_remaining_ = 0; frame_mode2_delay_ = false;
        window_line_ = 0; window_y_condition_ = false;
        window_wx_glitch_armed_ = false;
        reset_object_pipeline();
        completed_frame.fill(0); frame_ready_ = true;
        stat_line_ = (stat_enable_ & 0x40) != 0 && lyc_equal_;
    } else if (!was_enabled && lcd_enabled()) {
        ly_ = 0; line_dot_ = 0; window_line_ = 0; window_y_condition_ = wy_ == 0;
        window_wx_glitch_armed_ = false;
        reset_object_pipeline();
        lyc_compare_delay_ = 0;
        update_lyc_compare();
        // Toutes les révisions commencent par un mode 0 partiel et
        // sautent le scan OAM de la première ligne. Les particularités
        // de publication des trois premiers modes restent propres au DMG.
        lcd_startup_line_ = true;
        lcd_startup_lines_remaining_ = hardware_mode_ == gbc::HardwareMode::dmg ? 3 : 1;
        mode_ = mode_hblank;
        reported_mode_ = mode_hblank;
        reported_mode_target_ = mode_hblank;
        reported_mode_delay_ = 0;
        check_stat();
    }
}

inline int Ppu::read_stat() const noexcept {
    return 0x80 | stat_enable_ | (lyc_equal_ ? 4 : 0) |
        (lcd_enabled() ? reported_mode_ : 0);
}

/**
 * Fausse interruption du DMG à l'écriture de STAT : pendant l'écriture, le
 * bus interne présente un instant les sources H-Blank, V-Blank et LY=LYC à 1.
 * Elle part en H-Blank, en V-Blank ou si LY=LYC, jamais en mode 2 ni en
 * mode 3, d'après l'état du PPU au début du M-cycle d'écriture : la console
 * ne la produit pas sur le point qui ouvre le H-Blank, mais encore sur celui
 * qui ouvre le mode 2 de la ligne suivante (GBMicrotest,
 * stat_write_glitch_l0, l1, l143 et l154, mesurés sur DMG). Le CGB n'a pas
 * ce défaut.
 */
inline bool Ppu::stat_write_glitch_armed() const noexcept {
    return hardware_mode_ == gbc::HardwareMode::dmg && lcd_enabled() && !stat_line_ &&
        (hblank_signal() || mode_ == mode_vblank || lyc_equal_);
}

/**
 * Source H-Blank de STAT. Le mode 0 partiel qui ouvre la première ligne après
 * l'allumage de l'écran (avant son mode 3, point 76) ne la lève pas : STAT lit
 * bien le mode 0, mais l'interruption attend la fin du mode 3 de cette ligne
 * (GBMicrotest, int_hblank_nops, int_hblank_incs et int_hblank_halt, sur
 * DMG). La GBC suit la même règle, faute de mesure contraire.
 */
inline bool Ppu::hblank_signal() const noexcept {
    return mode_ == mode_hblank && !(lcd_startup_line_ && line_dot_ <= 76);
}

inline void Ppu::write_stat(int value) noexcept { write_stat(value, stat_write_glitch_armed()); }

inline void Ppu::write_stat(int value, bool glitch) noexcept {
    if (glitch) interrupts_.request(Interrupt::stat);
    stat_enable_ = value & 0x78;
    check_stat();
}

inline void Ppu::write_lyc(int value) noexcept {
    lyc_ = byte(value);
    if (lcd_enabled()) {
        lyc_compare_delay_ = 0;
        update_lyc_compare();
    }
    check_stat();
}

inline void Ppu::tick(int cycles) {
    if (!lcd_enabled() || cycles <= 0) return;
    for (int dot = 0; dot < cycles; ++dot) advance_dot();
}

inline void Ppu::advance_dot() {
    // HBlank ou VBlank, rien en attente, entre les points 81 et 447 de la
    // ligne : aucune des règles ci-dessous ne peut s'appliquer (elles
    // visent les points 1, 4, 76, 80, 448, 452 et 456), seul le compteur
    // avance. Près de la moitié des points d'une image passent par là.
    if (reported_mode_delay_ <= 0 && lyc_compare_delay_ <= 0 &&
        (ly_ >= height || mode_ == mode_hblank) && line_dot_ >= 80 && line_dot_ < 446) {
        ++line_dot_;
        return;
    }
    if (reported_mode_delay_ > 0 && --reported_mode_delay_ == 0) {
        reported_mode_ = reported_mode_target_;
    }
    if (lyc_compare_delay_ > 0 && --lyc_compare_delay_ == 0) {
        update_lyc_compare();
        check_stat();
    }
    if (ly_ < height && mode_ == mode_oam) advance_oam_scan();
    if (ly_ < height && mode_ == mode_transfer) advance_transfer();

    ++line_dot_;
    // Même raison que plus haut : de 81 à 447, aucune règle suivante ne
    // vise ce point, et la ligne n'est pas finie.
    if (line_dot_ > 80 && line_dot_ < 448) return;
    if (ly_ == 153 && line_dot_ == 4) {
        update_lyc_compare();
        check_stat();
    }
    // Le signal STAT de mode 2 précède de quatre dots le changement
    // visible de mode/LY. C'est observable depuis le CPU et explique les
    // seuils des tests mode 2 -> modes 0/3.
    const int stat_mode2_lead_dot = lcd_startup_line_ ? 448 : 452;
    if (line_dot_ == stat_mode2_lead_dot &&
        (ly_ < 143 || (gbc::is_cgb_hardware(hardware_mode_) && ly_ == 143))) {
        check_stat();
    }
    if (frame_mode2_delay_ && line_dot_ == 4) {
        frame_mode2_delay_ = false;
        check_stat();
    }
    // Sur DMG, la source mode 2 produit aussi une impulsion au début de
    // VBlank. Elle ne doit pas rester haute pendant toute la ligne 144.
    if (hardware_mode_ == gbc::HardwareMode::dmg && ly_ == 144 && line_dot_ == 1) check_stat();
    if (ly_ < height && ((lcd_startup_line_ && line_dot_ == 76 && mode_ == mode_hblank) ||
                        (!lcd_startup_line_ && line_dot_ == 80 && mode_ == mode_oam))) {
        begin_transfer();
    }

    const int line_length = lcd_startup_line_ ? 452 : 456;
    if (line_dot_ < line_length) return;
    const bool completed_startup_line = lcd_startup_line_;
    line_dot_ = 0;
    lcd_startup_line_ = false;
    if (lcd_startup_lines_remaining_ > 0) --lcd_startup_lines_remaining_;
    ++ly_;
    if (completed_startup_line) {
        // Le premier front LY après LCDC.7 est déphasé dans le cycle
        // CPU. Le comparateur présente d'abord faux, puis publie le
        // nouveau résultat au M-cycle suivant.
        lyc_equal_ = false;
        lyc_compare_delay_ = 4;
    } else {
        update_lyc_compare();
    }
    if (ly_ == height) {
        enter_vblank();
    } else if (ly_ > 153) {
        ly_ = 0;
        update_lyc_compare();
        window_line_ = 0;
        if (wy_ == 0) window_y_condition_ = true;
        // Sur DMG, la source mode 2 de la ligne 0 n'arrive qu'au point 4
        // (Mooneye, intr_1_2_timing-GS, vérifié sur DMG, MGB et SGB, qui
        // échoue sur CGB). Sur CGB, elle arrive au point 0 : quatre points
        // après celle des autres lignes, tirée quatre points avant leur
        // début. Mealybug Tearoom compense exactement ces quatre points.
        frame_mode2_delay_ = hardware_mode_ == gbc::HardwareMode::dmg;
        set_mode(mode_oam);
    } else if (ly_ < height) {
        if (ly_ == wy_) window_y_condition_ = true;
        set_mode(mode_oam);
    }
    check_stat();
}

inline void Ppu::enter_vblank() {
    window_y_condition_ = false;
    set_mode(mode_vblank); interrupts_.request(Interrupt::vblank);
    completed_frame = working_frame_; frame_ready_ = true;
}

inline void Ppu::set_mode(int mode) noexcept {
    mode_ = mode;
    if (mode == mode_vblank) {
        reported_mode_ = mode;
        reported_mode_target_ = mode;
        reported_mode_delay_ = 0;
    } else if (reported_mode_ != mode) {
        reported_mode_target_ = mode;
        if (hardware_mode_ == gbc::HardwareMode::dmg && lcd_startup_lines_remaining_ > 0) {
            reported_mode_delay_ = 4;
        } else {
            reported_mode_ = mode;
            reported_mode_delay_ = 0;
        }
    }
    if (mode == mode_oam) {
        oam_scan_index_ = 0;
        line_sprite_count_ = 0;
    }
    check_stat();
}

inline void Ppu::check_stat() noexcept {
    if (!lcd_enabled()) return;
    const bool line = ((stat_enable_ & 8) != 0 && hblank_signal()) ||
        ((stat_enable_ & 0x10) != 0 && mode_ == mode_vblank) ||
        ((stat_enable_ & 0x20) != 0 && mode2_stat_condition()) ||
        ((stat_enable_ & 0x40) != 0 && lyc_equal_);
    if (line && !stat_line_) interrupts_.request(Interrupt::stat);
    stat_line_ = line;
}

inline bool Ppu::mode2_stat_condition() const noexcept {
    if (mode_ == mode_oam) return !frame_mode2_delay_;
    const int lead_dot = lcd_startup_line_ ? 448 : 452;
    if (line_dot_ >= lead_dot &&
        (ly_ < 143 || (gbc::is_cgb_hardware(hardware_mode_) && ly_ == 143))) {
        return true;
    }
    return hardware_mode_ == gbc::HardwareMode::dmg &&
        mode_ == mode_vblank && ly_ == 144 && line_dot_ == 0;
}

inline void Ppu::update_lyc_compare() noexcept { lyc_equal_ = visible_ly() == lyc_; }

inline int Ppu::visible_ly() const noexcept {
    return ly_ == 153 && line_dot_ >= 4 ? 0 : ly_;
}

} // namespace ravenemu::cgb
