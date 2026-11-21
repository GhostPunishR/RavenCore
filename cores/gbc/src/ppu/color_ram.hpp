#pragma once

#include "ppu/ppu.hpp"

namespace ravenemu::cgb {

/**
 * RAM couleur de la GBC (BCPS/BCPD, OCPS/OCPD) : huit palettes de fond et
 * huit d'objets, leur cache ARGB, et les palettes de compatibilité qu'installe
 * la boot ROM pour un jeu Game Boy.
 */

inline void Ppu::write_bcps(int value) noexcept { bcps_index_ = value & 0x3f; bcps_auto_ = (value & 0x80) != 0; }

inline int Ppu::read_bcps() const noexcept { return bcps_index_ | (bcps_auto_ ? 0x80 : 0) | 0x40; }

inline void Ppu::write_bcpd(int value) noexcept {
    if (!cpu_video_bus_locks().cram) {
        bg_cram[static_cast<std::size_t>(bcps_index_)] = static_cast<std::uint8_t>(value);
        recompute_argb(bg_cram, bg_argb_, bcps_index_);
    }
    // Le compteur d'adresse appartient au registre d'index, pas à la RAM
    // couleur : il avance même si le mode 3 rejette l'écriture de données.
    if (bcps_auto_) bcps_index_ = (bcps_index_ + 1) & 0x3f;
}

inline int Ppu::read_bcpd() const noexcept {
    if (cpu_video_bus_locks().cram) return 0xff;
    return bg_cram[static_cast<std::size_t>(bcps_index_)];
}

inline void Ppu::write_ocps(int value) noexcept { ocps_index_ = value & 0x3f; ocps_auto_ = (value & 0x80) != 0; }

inline int Ppu::read_ocps() const noexcept { return ocps_index_ | (ocps_auto_ ? 0x80 : 0) | 0x40; }

inline void Ppu::write_ocpd(int value) noexcept {
    if (!cpu_video_bus_locks().cram) {
        obj_cram[static_cast<std::size_t>(ocps_index_)] = static_cast<std::uint8_t>(value);
        recompute_argb(obj_cram, obj_argb_, ocps_index_);
    }
    if (ocps_auto_) ocps_index_ = (ocps_index_ + 1) & 0x3f;
}

inline int Ppu::read_ocpd() const noexcept {
    if (cpu_video_bus_locks().cram) return 0xff;
    return obj_cram[static_cast<std::size_t>(ocps_index_)];
}

/** État HLE déterministe lorsque le firmware CGB n'est pas exécuté. */
inline void Ppu::initialize_hle_post_boot() noexcept {
    if (!gbc::is_cgb_hardware(hardware_mode_)) return;
    bcps_index_ = 0x08;
    bcps_auto_ = true;
    ocps_index_ = 0x10;
    ocps_auto_ = true;
}

inline void Ppu::set_compatibility_palettes(const CompatibilityPalettes& palettes) noexcept {
    const auto store = [](auto& cram, int palette, const std::array<int, 4>& colors) {
        for (int color = 0; color < 4; ++color) {
            const int value = colors[static_cast<std::size_t>(color)];
            const auto index = static_cast<std::size_t>((palette * 4 + color) * 2);
            cram[index] = static_cast<std::uint8_t>(value);
            cram[index + 1] = static_cast<std::uint8_t>(value >> 8);
        }
    };
    store(bg_cram, 0, palettes.background);
    store(obj_cram, 0, palettes.object0);
    store(obj_cram, 1, palettes.object1);
    rebuild_color_cache();
}

/** Mode de compatibilité sans boot ROM : les palettes qu'elle donne à un jeu inconnu. */
inline void Ppu::initialize_hle_compatibility_palettes() noexcept {
    if (hardware_mode_ != gbc::HardwareMode::cgb_compatibility) return;
    set_compatibility_palettes(boot_default_palettes);
}

inline void Ppu::recompute_argb(const std::array<std::uint8_t, 64>& cram, std::array<std::int32_t, 32>& colors, int byte_index) noexcept {
    const int color = byte_index / 2;
    colors[static_cast<std::size_t>(color)] = bgr555_to_argb(
        cram[static_cast<std::size_t>(color * 2)] | (cram[static_cast<std::size_t>(color * 2 + 1)] << 8)
    );
}

inline void Ppu::rebuild_color_cache() noexcept {
    for (int i = 0; i < 32; ++i) { recompute_argb(bg_cram, bg_argb_, i * 2); recompute_argb(obj_cram, obj_argb_, i * 2); }
}

} // namespace ravenemu::cgb
