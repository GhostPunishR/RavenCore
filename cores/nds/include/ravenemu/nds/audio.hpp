#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ravenemu::nds {

// Générateur sonore ARM7. Implémentation indépendante d'après GBATEK,
// DS Sound Channels/Control/Notes ; capture et arbitrage du bus non modélisés.
class Audio {
public:
    static constexpr unsigned sample_rate = 32768;
    static constexpr std::size_t buffer_frames = 4096;
    using Reader = std::uint8_t (*)(const void*, std::uint32_t) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint8_t read_byte(unsigned offset) const noexcept;
    void write_byte(unsigned offset, std::uint8_t value) noexcept;
    void advance_cycles(std::uint64_t cycles, Reader read, const void* context) noexcept;
    void set_speakers_enabled(bool enabled) noexcept { speakers_enabled_ = enabled; }
    // Nombre de valeurs int16, toujours pair. En surcharge, les plus anciennes
    // trames sont abandonnées ; les horloges continuent sans dépendre du frontend.
    std::size_t read_samples(std::span<std::int16_t> output) noexcept;

private:
    struct Channel {
        std::uint32_t control{}, source{}, length{}, position{}, remaining{};
        std::uint16_t timer{}, loop{}, noise{0x7fff};
        unsigned delay{}, phase{}, index{}, loop_index{};
        std::int32_t sample{}, predictor{}, loop_predictor{};
        bool header{}, loop_saved{};
    };
    void synchronize() noexcept;
    void schedule() noexcept;
    void tick(unsigned number, Reader read, const void* context) noexcept;
    void start(unsigned number) noexcept;
    [[nodiscard]] std::array<std::int16_t,2> mix() const noexcept;
    void enqueue(std::array<std::int16_t,2> frame) noexcept;
    std::array<Channel,16> channels_{};
    std::uint16_t control_{}, bias_{0x200}, running_{};
    std::uint32_t sample_phase_{}, pending_{}, next_event_{1023};
    std::array<std::int16_t,buffer_frames * 2> buffer_{};
    std::size_t head_{}, size_{};
    bool speakers_enabled_{true};
};

} // namespace ravenemu::nds
