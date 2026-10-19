#include <ravenemu/nds/video.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {

VideoFrame VideoMemory::render_frame(std::uint32_t display_control, std::uint16_t power) const {
    VideoFrame frame;
    frame.upper.fill(0x7fff);
    frame.lower.fill(0x7fff);
    const auto mode = (display_control >> 16U) & 3U;
    if (!(power & 1U) || (display_control & 0x80U) || mode == 0) return frame;
    if (mode != 2) throw Error("NDS : seul l'affichage VRAM direct est pris en charge");
    const auto bank = (display_control >> 18U) & 3U;
    if ((controls_[bank] & 0x87U) != 0x80U) throw Error("NDS : la banque d'affichage doit être activée en mode LCDC");
    auto& output = (power & 0x8000U) ? frame.upper : frame.lower;
    const auto start = bank * 0x20000U;
    for (std::size_t pixel = 0; pixel < output.size(); ++pixel)
        output[pixel] = static_cast<std::uint16_t>((bytes_[start + pixel * 2] |
            (static_cast<unsigned>(bytes_[start + pixel * 2 + 1]) << 8U)) & 0x7fffU);
    return frame;
}

} // namespace ravenemu::nds
