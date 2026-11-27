#include "sound/sound.hpp"

#include <algorithm>
#include <utility>

namespace ravenemu::nds {

namespace {

constexpr std::int64_t factor(std::uint32_t setting) noexcept { return setting == 127 ? 128 : setting; }

template <std::size_t... Index>
std::array<SoundChannel, 16> make_channels(std::index_sequence<Index...>) noexcept {
    return {SoundChannel{Index}...};
}

} // namespace

Sound::Sound() : channels_(make_channels(std::make_index_sequence<16>{})) {}

std::uint32_t Sound::read(std::uint32_t offset, Cycles now) {
    run(now);
    if (offset < 0x100) return (offset & 0xfU) == 0 ? channels_[offset / 16].control() : 0U;  // SAD, TMR... en écriture seule
    if (offset == 0x100) return control_;
    if (offset == 0x104) return bias_;
    return 0;
}

void Sound::write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now) {
    run(now);
    if (offset < 0x100) return channels_[offset / 16].write(offset & 0xfU, value, mask);
    if (offset == 0x100) control_ = ((control_ & ~mask) | (value & mask)) & 0xbf7fU;
    if (offset == 0x104) bias_ = ((bias_ & ~mask) | (value & mask)) & 0x3ffU;
}

void Sound::run(Cycles now) {
    while (time_ + cycles_per_sample <= now) {
        time_ += cycles_per_sample;
        mix();
        for (auto& channel : channels_) channel.advance(cycles_per_sample / 2, source_);
    }
}

void Sound::mix() {
    std::array<std::int64_t, 2> mixer{};
    std::array<std::array<std::int64_t, 2>, 2> direct{};  // canaux 1 et 3, gauche et droite
    for (unsigned index = 0; index < channels_.size(); ++index) {
        const auto& channel = channels_[index];
        const auto control = channel.control();
        static constexpr std::array<unsigned, 4> divider{0, 1, 2, 4};
        const auto scaled = ((std::int64_t{channel.sample()} * 16) >> divider[(control >> 8U) & 3U]) * factor(control & 0x7fU);
        const auto pan = factor((control >> 16U) & 0x7fU);
        const std::array<std::int64_t, 2> sides{(scaled * (128 - pan)) >> 10, (scaled * pan) >> 10};
        if (index == 1 || index == 3) direct[index / 2] = sides;
        const auto removed = (index == 1 && (control_ & 0x1000U) != 0) || (index == 3 && (control_ & 0x2000U) != 0);
        if (!removed) {
            mixer[0] += sides[0];
            mixer[1] += sides[1];
        }
    }
    const auto master = (control_ & 0x8000U) != 0 ? factor(control_ & 0x7fU) : 0;
    for (unsigned side = 0; side < 2; ++side) {
        std::int64_t level = mixer[side];
        switch ((control_ >> (8U + side * 2U)) & 3U) {
        case 1: level = direct[0][side]; break;
        case 2: level = direct[1][side]; break;
        case 3: level = direct[0][side] + direct[1][side]; break;
        default: break;
        }
        // × maître / 128 / 64, partie entière, + biais, bornes 0..3FFh.
        const auto value = std::clamp<std::int64_t>(((level * master) >> 21) + (bias_ & 0x3ffU), 0, 0x3ff);
        if (output_.size() < buffer_limit) output_.push_back(static_cast<std::int16_t>((value - 0x200) * 64));
    }
}

} // namespace ravenemu::nds
