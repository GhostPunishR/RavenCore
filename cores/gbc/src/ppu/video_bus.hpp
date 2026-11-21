#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * Portes du bus vidéo : ce que le CPU peut lire et écrire en VRAM, en OAM et
 * en RAM couleur selon le mode du PPU, et ce que le PPU lui-même voit pendant
 * une transition de vitesse (KEY1) de la GBC.
 */

inline int Ppu::read_vram(int address) const noexcept {
    if (cpu_video_bus_locks().vram_read) return 0xff;
    return vram[static_cast<std::size_t>(vram_bank_ * 0x2000 + (address & 0x1fff))];
}

inline void Ppu::write_vram(int address, int value) noexcept {
    if (cpu_video_bus_locks().vram_write) return;
    vram[static_cast<std::size_t>(vram_bank_ * 0x2000 + (address & 0x1fff))] = static_cast<std::uint8_t>(value);
}

inline int Ppu::read_oam(int address) const noexcept {
    if (cpu_video_bus_locks().oam_read) return 0xff;
    return oam[static_cast<std::size_t>(address & 0xff)];
}

inline void Ppu::write_oam(int address, int value) noexcept {
    if (cpu_video_bus_locks().oam_write) return;
    oam[static_cast<std::size_t>(address & 0xff)] = static_cast<std::uint8_t>(value);
}

/**
 * Fige les portes vidéo internes au niveau observé lorsque STOP lance une
 * transition KEY1. Le raster continue d'avancer, mais les arbitres mémoire
 * conservent ce niveau jusqu'à la fin des 2050 M-cycles.
 */
inline void Ppu::begin_speed_switch() noexcept {
    if (!gbc::cgb_features_enabled(hardware_mode_)) return;
    if (mode_ == mode_transfer) {
        speed_switch_video_access_ = speed_switch_access_all;
    } else if (mode_ == mode_oam) {
        speed_switch_video_access_ = speed_switch_access_background;
    } else {
        speed_switch_video_access_ = speed_switch_access_none;
    }
}

inline void Ppu::end_speed_switch() noexcept {
    speed_switch_video_access_ = speed_switch_access_inactive;
}

inline bool Ppu::speed_switch_active() const noexcept {
    return speed_switch_video_access_ != speed_switch_access_inactive;
}

inline void Ppu::validate_speed_switch_state(bool controller_switching) const {
    if (speed_switch_active() != controller_switching) {
        throw SaveStateError("État instantané corrompu (transition KEY1/PPU)");
    }
}

inline bool Ppu::ppu_vram_available() const noexcept {
    return speed_switch_video_access_ == speed_switch_access_inactive ||
        speed_switch_video_access_ >= speed_switch_access_background;
}

inline bool Ppu::ppu_oam_available() const noexcept {
    return speed_switch_video_access_ == speed_switch_access_inactive ||
        speed_switch_video_access_ == speed_switch_access_all;
}

inline bool Ppu::ppu_cram_available() const noexcept {
    return speed_switch_video_access_ == speed_switch_access_inactive ||
        speed_switch_video_access_ >= speed_switch_access_background;
}

inline Ppu::CpuVideoBusLocks Ppu::cpu_video_bus_locks() const noexcept {
    if (!lcd_enabled()) return {};

    const bool transfer = mode_ == mode_transfer;
    const bool oam_scan = mode_ == mode_oam;
    if (hardware_mode_ != gbc::HardwareMode::dmg || lcd_startup_lines_remaining_ == 0) {
        // En régime établi, les portes du bus suivent la phase interne du
        // PPU ; STAT n'est qu'une vue publiée de cette phase.
        return {transfer, transfer, oam_scan || transfer,
                oam_scan || transfer, transfer};
    }

    // Les trois premières lignes après LCDC.7 sur DMG publient certains
    // fronts avec quatre dots de retard. Les portes de lecture et
    // d'écriture ne voient pas toutes le même front, ce qui est observable
    // par lcdon_timing et lcdon_write_timing.
    const bool first_line_transfer_opening = lcd_startup_line_ && transfer &&
        reported_mode_ == mode_hblank;
    return {
        reported_mode_ == mode_transfer || (transfer && !first_line_transfer_opening),
        reported_mode_ == mode_transfer,
        oam_scan || reported_mode_ == mode_oam || reported_mode_ == mode_transfer,
        reported_mode_ == mode_transfer || (reported_mode_ == mode_oam && oam_scan),
        transfer,
    };
}

inline int Ppu::vram_byte(int bank, int address) const noexcept {
    if (!ppu_vram_available()) return 0xff;
    return vram[static_cast<std::size_t>(bank * 0x2000 + (address & 0x1fff))];
}

} // namespace ravenemu::cgb
