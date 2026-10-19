#include <ravenemu/nds/video.hpp>
#include <ravenemu/core.hpp>
#include "renderer.hpp"

namespace ravenemu::nds {

VideoFrame VideoMemory::render_frame(std::uint32_t display_control, std::uint16_t power, std::uint32_t display_control_b) const {
    VideoFrame frame;
    frame.upper.fill(0x7fff);
    frame.lower.fill(0x7fff);
    if (!(power & 1U)) return frame;
    auto& output_a = (power & 0x8000U) ? frame.upper : frame.lower;
    auto& output_b = (power & 0x8000U) ? frame.lower : frame.upper;
    detail::Renderer{*this, 0, display_control}.render(output_a, power);
    detail::Renderer{*this, 1, display_control_b}.render(output_b, power);
    return frame;
}

} // namespace ravenemu::nds
