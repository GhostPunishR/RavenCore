#include "renderer.hpp"

#include <algorithm>

namespace ravenemu::nds::detail {

void Renderer::mosaic_objects(std::span<Sample> pixels) const {
    const auto width = ((reg16(0x4c) >> 8U) & 15U) + 1;
    if (width == 1 || !(control_ & 0x1000U)) return;
    for (unsigned y = 0; y < VideoFrame::height; ++y) {
        auto line = pixels.subspan(y * VideoFrame::width, VideoFrame::width);
        for (unsigned block = 0; block < line.size(); block += width) {
            const auto end = std::min(block + width, VideoFrame::width);
            // Un changement d'activation ou une priorité BG plus haute ouvre un
            // nouveau segment dans le bloc. La couleur et l'alpha voyagent ensemble.
            for (auto x = block; x < end;) {
                const auto origin = line[x++];
                if (!origin.object_mosaic) continue;
                while (x < end && line[x].object_mosaic && line[x].object_priority >= origin.object_priority)
                    line[x++].object = origin.object;
            }
        }
    }
}

} // namespace ravenemu::nds::detail
