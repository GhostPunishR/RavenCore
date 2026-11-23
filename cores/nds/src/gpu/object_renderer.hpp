#pragma once

#include "gpu/vram.hpp"
#include "memory/memory.hpp"

#include <array>
#include <cstdint>

// Objets (sprites) d'un moteur 2D, pour une ligne (GBATEK, « DS Video OBJs »
// et « LCD OBJ - OAM Attributes ») : 128 objets en OAM, normaux ou affines
// (taille doublée comprise), 16 ou 256 couleurs, tuiles en correspondance 1D
// (pas réglé par DISPCNT bits 20-21) ou 2D, retournements. L'objet de plus
// petit numéro l'emporte à priorité égale.
// Pas encore : objets bitmap, fenêtre d'objets, transparence, mosaïque,
// palettes étendues, limite de points par ligne.
namespace ravenemu::nds {

struct ObjectLine {
    static constexpr std::uint16_t transparent = 0x8000;
    std::array<std::uint16_t, 256> color{};
    std::array<std::uint8_t, 256> priority{};
};

void draw_objects(bool main, std::uint32_t display_control, unsigned line, const Vram& vram, const Memory& memory,
                  ObjectLine& out);

} // namespace ravenemu::nds
