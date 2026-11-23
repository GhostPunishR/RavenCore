#pragma once

#include "gpu/vram.hpp"
#include "memory/memory.hpp"

#include <array>
#include <cstdint>
#include <span>

// Un moteur 2D du DS, A (écran principal) ou B (GBATEK, « DS Video »). Ses
// registres sont à 0x04000000 (A) ou 0x04001000 (B). Rendu par ligne, en
// BGR555 :
// - modes d'affichage 0 (blanc), 1 (normal), 2 (VRAM, moteur A) ;
// - fonds en mode texte, 16 ou 256 couleurs, avec retournements et
//   défilement, priorités et couleur de fond.
// Pas encore : fonds affines, étendus et bitmap, objets, fenêtres, effets,
// mosaïque, palettes étendues, calque 3D, affichage depuis la RAM principale
// (mode 3, rendu noir).
namespace ravenemu::nds {

class Engine2d {
public:
    static constexpr unsigned width = 256;

    explicit Engine2d(bool main) noexcept : main_(main) {}

    // Mots d'E/S, `offset` de 0x00 à 0x6C depuis la base du moteur.
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const noexcept;
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask) noexcept;

    void render_line(unsigned line, const Vram& vram, const Memory& memory, std::span<std::uint16_t, width> out) const;

    [[nodiscard]] std::uint32_t display_control() const noexcept { return registers_[0]; }
    // MASTER_BRIGHT (0x6C) : facteur (bits 0-4), mode (bits 14-15).
    [[nodiscard]] std::uint32_t master_brightness() const noexcept { return registers_[0x6c / 4] & 0xc01fU; }

private:
    [[nodiscard]] std::uint16_t bg_control(unsigned bg) const noexcept {
        return static_cast<std::uint16_t>(registers_[(0x08 + bg * 2) / 4] >> ((bg & 1U) * 16U));
    }
    [[nodiscard]] std::uint32_t bg_offset(unsigned bg, bool vertical) const noexcept {
        const auto offset = 0x10 + bg * 4 + (vertical ? 2U : 0U);
        return (registers_[offset / 4] >> ((offset & 2U) * 8U)) & 0x1ffU;
    }
    [[nodiscard]] bool text_layer(unsigned bg) const noexcept;
    void draw_text_layer(unsigned bg, unsigned line, const Vram& vram, const Memory& memory,
                         std::span<std::uint16_t, width> colors) const;

    bool main_;
    std::array<std::uint32_t, 0x70 / 4> registers_{};
};

// BGR555 vers ARGB8888, après la luminosité maîtresse (GBATEK : sur des
// intensités de 6 bits, « up » vers le blanc, « down » vers le noir).
std::uint32_t to_argb(std::uint16_t color, std::uint32_t master_brightness) noexcept;

} // namespace ravenemu::nds
