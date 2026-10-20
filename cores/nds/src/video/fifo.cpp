#include <ravenemu/nds/video.hpp>

#include <algorithm>

namespace ravenemu::nds {

void VideoMemory::write_fifo(std::uint32_t value) noexcept {
    fifo_words_[fifo_write_] = value;
    fifo_write_ = (fifo_write_ + 1) % static_cast<unsigned>(fifo_words_.size());
}

bool VideoMemory::wants_fifo(std::uint32_t display_control) const noexcept {
    return ((display_control >> 16U) & 3U) == 3 ||
        ((capture_control_ & 0x82000000U) == 0x82000000U && ((capture_control_ >> 29U) & 3U) != 0);
}

std::array<std::uint16_t, 8> VideoMemory::fifo_group() const noexcept {
    std::array<std::uint16_t, 8> pixels;
    for (unsigned i = 0; i < 4; ++i) {
        const auto word = fifo_words_[(fifo_read_ + i) % fifo_words_.size()];
        pixels[i * 2] = static_cast<std::uint16_t>(word);
        pixels[i * 2 + 1] = static_cast<std::uint16_t>(word >> 16U);
    }
    return pixels;
}

void VideoMemory::sample_fifo_group(unsigned line, unsigned x) noexcept {
    const auto pixels = fifo_group();
    std::copy(pixels.begin(), pixels.end(), fifo_pixels_.begin() + line * VideoFrame::width + x);
    fifo_read_ = (fifo_read_ + 4) % static_cast<unsigned>(fifo_words_.size());
}

void VideoMemory::finish_visible_line(unsigned line, std::uint32_t display_control, std::uint16_t power) {
    if (capture_active_ && line < capture_height()) {
        const auto start = fifo_pixels_.begin() + line * VideoFrame::width;
        std::array<std::uint16_t, VideoFrame::width> source;
        std::copy_n(start, source.size(), source.begin());
        if (fifo_running_) {
            const auto pending = fifo_group();
            std::copy(pending.begin(), pending.end(), source.end() - pending.size());
        }
        // Préparer la source entière sans retirer le dernier groupe. Si la
        // composition échoue, FIFO, VRAM et horloges conservent cette unité.
        capture_line(line, display_control, power, source);
    }
    if (fifo_running_) sample_fifo_group(line, VideoFrame::width - 8);
}

void VideoMemory::finish_frame() noexcept {
    if (capture_active_) { capture_control_ &= 0x7fffffffU; capture_active_ = false; }
    fifo_read_ = fifo_write_ = 0;
    fifo_running_ = false;
}

} // namespace ravenemu::nds
