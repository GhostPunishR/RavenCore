#pragma once

#include "cartridge/cartridge.hpp"
#include "ppu/ppu.hpp"
#include <ravenemu/gbc/hardware_mode.hpp>

#include <array>
#include <cstdint>

namespace ravenemu::cgb {

/**
 * VRAM que la boot ROM laisse au jeu quand elle n'est pas exécutée.
 *
 * La boot ROM dessine le logo de l'en-tête de la cartouche ($0104-$0133) et le
 * glyphe « ® », puis rend la main sans effacer la VRAM. Des jeux et des tests
 * s'en servent : Mealybug Tearoom affiche la tuile $19 (le « ® ») en sprite.
 * Sans boot ROM, RavenCore reproduit cet état :
 *
 * - tuiles 1 à 24 ($8010-$818F) : chaque quartet du logo, pixels doublés en
 *   largeur et en hauteur, sur le premier plan de bits ; deux octets du logo
 *   par tuile, les 24 premiers pour la rangée du haut ;
 * - tuile 25 ($8190) : le glyphe « ® », même plan de bits ;
 * - sur Game Boy, carte BG $9904-$990F (tuiles 1 à 12), $9910 (« ® ») et
 *   $9924-$992F (tuiles 13 à 24) : le logo reste à l'écran tant que le jeu
 *   ne l'efface pas.
 *
 * Sur la GBC, en mode de compatibilité, les captures de référence de Mealybug
 * Tearoom (CPU CGB D) montrent les mêmes tuiles ; la carte BG n'y est pas
 * reprise, faute de mesure, ni le mode GBC natif.
 */
inline void install_post_boot_logo(Ppu& ppu, const Cartridge& cartridge, gbc::HardwareMode mode) {
    if (mode != gbc::HardwareMode::dmg && mode != gbc::HardwareMode::cgb_compatibility) return;

    constexpr int first_tile = 0x0010;
    for (int index = 0; index < 48; ++index) {
        const int value = cartridge.read_rom(0x0104 + index);
        for (int half = 0; half < 2; ++half) {
            const int nibble = half == 0 ? value >> 4 : value & 0x0f;
            int doubled = 0;
            for (int bit = 0; bit < 4; ++bit) {
                if ((nibble & (1 << bit)) != 0) doubled |= 3 << (bit * 2);
            }
            const int row = (index * 2 + half) * 2;
            for (int repeat = 0; repeat < 2; ++repeat) {
                ppu.vram[static_cast<std::size_t>(first_tile + (row + repeat) * 2)] =
                    static_cast<std::uint8_t>(doubled);
            }
        }
    }

    // Glyphe « ® » de la boot ROM, une ligne de huit pixels par octet. Seule
    // donnée reprise de la boot ROM, avec l'accord du propriétaire du dépôt.
    constexpr std::array<std::uint8_t, 8> registered_glyph{0x3c, 0x42, 0xb9, 0xa5, 0xb9, 0xa5, 0x42, 0x3c};
    constexpr int glyph_tile = 0x19;
    for (std::size_t row = 0; row < registered_glyph.size(); ++row) {
        ppu.vram[static_cast<std::size_t>(glyph_tile * 16) + row * 2] = registered_glyph[row];
    }

    if (mode != gbc::HardwareMode::dmg) return;
    constexpr int top_row = 0x1904;
    constexpr int bottom_row = 0x1924;
    for (int tile = 0; tile < 12; ++tile) {
        ppu.vram[static_cast<std::size_t>(top_row + tile)] = static_cast<std::uint8_t>(tile + 1);
        ppu.vram[static_cast<std::size_t>(bottom_row + tile)] = static_cast<std::uint8_t>(tile + 13);
    }
    ppu.vram[static_cast<std::size_t>(top_row + 12)] = glyph_tile;
}

} // namespace ravenemu::cgb
