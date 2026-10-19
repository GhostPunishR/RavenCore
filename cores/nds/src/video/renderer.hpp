#pragma once

#include <ravenemu/nds/video.hpp>

#include <span>

namespace ravenemu::nds::detail {

class Renderer {
public:
    Renderer(const VideoMemory& memory, unsigned engine, std::uint32_t control)
        : memory_(memory), engine_(engine), control_(control) {}
    void render(std::span<std::uint16_t> output, std::uint16_t power) const;
    void apply_master_brightness(std::span<std::uint16_t> output) const;

private:
    enum class Background { none, text, affine, extended, large, three_d };
    struct Pixel {
        std::uint16_t color{};
        unsigned rank{0x10000U}, layer{6}, alpha{}; // alpha : 0=normal, 1..16=bitmap, 17=semi-transparent
    };
    struct Sample {
        Pixel top, below, object;
        bool object_window{};
        std::uint8_t mask{0x3f};
        void insert(Pixel pixel) {
            if (pixel.rank < top.rank) { below = top; top = pixel; }
            else if (pixel.rank < below.rank) below = pixel;
        }
    };
    const VideoMemory& memory_;
    unsigned engine_;
    std::uint32_t control_;
    [[nodiscard]] std::uint16_t reg16(unsigned offset) const;
    [[nodiscard]] std::int64_t reference(unsigned offset) const;
    [[nodiscard]] std::uint8_t gpu8(bool object, std::uint32_t offset) const;
    [[nodiscard]] std::uint16_t gpu16(bool object, std::uint32_t offset) const;
    [[nodiscard]] std::uint16_t palette(bool object, unsigned index) const;
    [[nodiscard]] std::uint16_t extended_palette(bool object, unsigned slot, unsigned bank, unsigned index) const;
    [[nodiscard]] std::uint16_t oam16(unsigned offset) const;
    [[nodiscard]] std::optional<std::uint16_t> tile_pixel(bool object, std::uint32_t address, bool depth8,
                                                       unsigned x, unsigned y, unsigned bank, int extended_slot = -1) const;
    [[nodiscard]] std::optional<std::uint16_t> background_pixel(unsigned bg, Background kind, unsigned x, unsigned y) const;
    void backgrounds(std::span<Sample> pixels) const;
    void objects(std::span<Sample> pixels) const;
    void windows(std::span<Sample> pixels) const;
    [[nodiscard]] std::uint16_t composite(const Sample& sample) const;
};

} // namespace ravenemu::nds::detail
