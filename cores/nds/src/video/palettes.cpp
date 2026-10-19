#include "renderer.hpp"
#include "vram_layout.hpp"

namespace ravenemu::nds::detail {

std::uint16_t Renderer::extended_palette(bool object, unsigned slot, unsigned bank, unsigned index) const {
    const auto address = (object ? 0 : slot * 0x2000U) + (bank * 256 + index) * 2;
    unsigned color = 0;
    const auto enabled = [&](unsigned number, unsigned mode) {
        return (memory_.controls_[number] & 0x87U) == (0x80U | mode);
    };
    const auto merge = [&](unsigned number, unsigned base, unsigned length) {
        if (address < base || address - base >= length) return;
        const auto physical = vram_starts[number] + address - base;
        color |= static_cast<unsigned>(memory_.bytes_[physical]) | (static_cast<unsigned>(memory_.bytes_[physical + 1]) << 8U);
    };
    if (engine_ == 0) {
        if (!object && enabled(4, 4)) merge(4, 0, 0x8000); // E : slots BG 0..3, moitié basse seulement
        for (unsigned number = 5; number <= 6; ++number) {
            if (object) {
                if (enabled(number, 5)) merge(number, 0, 0x2000); // F/G : palette OBJ, OFS ignoré
            } else if (enabled(number, 4)) {
                const auto offset = (memory_.controls_[number] >> 3U) & 3U;
                if (offset < 2) merge(number, offset * 0x4000U, 0x4000);
            }
        }
    } else if (object) {
        if (enabled(8, 3)) merge(8, 0, 0x2000); // I : moitié basse, 16 palettes de 256 couleurs
    } else if (enabled(7, 2)) merge(7, 0, 0x8000); // H : les quatre slots BG
    // Comme les vues VRAM ordinaires : banques superposées en OU, absence à zéro.
    return static_cast<std::uint16_t>(color & 0x7fffU);
}

} // namespace ravenemu::nds::detail
