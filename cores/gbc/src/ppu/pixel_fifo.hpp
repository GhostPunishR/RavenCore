#pragma once

#include <cstdint>

namespace ravenemu::cgb {

/**
 * Pixels des deux FIFO du mode 3 (Pan Docs, Pixel FIFO). Chacune tient
 * seize pixels dans un anneau ; le PPU les remplit huit par huit.
 */
struct BgPixel {
    std::uint8_t color{};
    std::uint8_t palette{};
    bool priority{};
};

struct ObjPixel {
    std::uint8_t color{};
    std::uint8_t palette{};
    bool behind_background{};
    std::uint8_t oam_index{0xff};
    std::uint8_t raw_x{0xff};
};

} // namespace ravenemu::cgb
