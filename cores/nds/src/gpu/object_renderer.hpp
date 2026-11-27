#pragma once

#include "gpu/vram.hpp"
#include "memory/memory.hpp"

#include <array>
#include <cstdint>

// Objets (sprites) d'un moteur 2D, pour une ligne (GBATEK, « DS Video OBJs »
// et « LCD OBJ - OAM Attributes ») : 128 objets en OAM, normaux ou affines
// (taille doublée comprise), 16 ou 256 couleurs, tuiles en correspondance 1D
// (pas réglé par DISPCNT bits 20-21) ou 2D, retournements. L'objet de plus
// petit numéro l'emporte à priorité égale. Les objets du mode 1 sont marqués
// semi-transparents ; ceux du mode 2 forment la fenêtre d'objets ; ceux du
// mode 3 sont des bitmaps en couleurs directes (bit 15 : point visible), en
// correspondance 2D (largeur 128 ou 256 points) ou 1D (pas de 128 ou 256
// octets). Palettes étendues (DISPCNT bit 31) des objets de 256 couleurs,
// mosaïque (attribut 0 bit 12).
// Pas encore : valeur alpha de l'OAM des objets bitmap (GBATEK n'en décrit pas
// l'effet), limite de points par ligne.
namespace ravenemu::nds {

struct ObjectLine {
    static constexpr std::uint16_t transparent = 0x8000;
    std::array<std::uint16_t, 256> color{};
    std::array<std::uint8_t, 256> priority{};
    std::array<bool, 256> semi_transparent{};  // objet du mode 1, toujours mélangé
    std::array<bool, 256> window{};            // points des objets du mode 2 (fenêtre d'objets)
};

// `mosaic` : registre MOSAIC (bits 8-15 pour les objets).
void draw_objects(bool main, std::uint32_t display_control, std::uint32_t mosaic, unsigned line, const Vram& vram,
                  const Memory& memory, ObjectLine& out);

} // namespace ravenemu::nds
