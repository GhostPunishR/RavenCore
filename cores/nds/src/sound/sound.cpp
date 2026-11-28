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

// Partie entière ramenée à 16 bits signés, partie fractionnaire gardée :
// le débordement de l'addition vers la capture.
constexpr std::int64_t wrap16(std::int64_t value, unsigned fraction) noexcept {
    const auto integer = static_cast<std::int16_t>(static_cast<std::uint16_t>((value >> fraction) & 0xffff));
    return std::int64_t{integer} * (std::int64_t{1} << fraction) + (value & ((std::int64_t{1} << fraction) - 1));
}

constexpr unsigned volume_fraction = 11;  // 16.11 : après diviseur et volume
constexpr unsigned mixer_fraction = 8;    // 16.8 : après panoramique

} // namespace

Sound::Sound() : channels_(make_channels(std::make_index_sequence<16>{})) {}

std::uint32_t Sound::read(std::uint32_t offset, Cycles now) {
    run(now);
    if (offset < 0x100) return (offset & 0xfU) == 0 ? channels_[offset / 16].control() : 0U;  // SAD, TMR... en écriture seule
    switch (offset) {
    case 0x100: return control_;
    case 0x104: return bias(now);
    case 0x108: return captures_[0].control() | (captures_[1].control() << 8U);
    case 0x110: return captures_[0].destination();
    case 0x118: return captures_[1].destination();
    default: return 0;  // SNDCAPxLEN : en écriture seule
    }
}

void Sound::write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now) {
    run(now);
    if (offset < 0x100) return channels_[offset / 16].write(offset & 0xfU, value, mask);
    switch (offset) {
    case 0x100: control_ = ((control_ & ~mask) | (value & mask)) & 0xbf7fU; break;
    case 0x104:
        bias_ = ((bias(now) & ~mask) | (value & mask)) & 0x3ffU;
        ramp_.active = false;
        break;
    case 0x108:
        for (unsigned index = 0; index < 2; ++index) {
            const auto shift = 8U * index;
            const auto byte_mask = (mask >> shift) & 0xffU;
            if (byte_mask == 0) continue;
            auto& capture = captures_[index];
            capture.write_control((capture.control() & ~byte_mask) | ((value >> shift) & byte_mask), channels_[2 * index + 1].timer());
        }
        break;
    case 0x110:
    case 0x118: captures_[(offset - 0x110) / 8].write_destination(value, mask); break;
    case 0x114:
    case 0x11c: captures_[(offset - 0x114) / 8].write_length(value, mask); break;
    default: break;
    }
}

void Sound::set_bias_level(bool high) noexcept {
    bias_ = high ? 0x200U : 0U;
    ramp_.active = false;
}

Cycles Sound::ramp_bias(bool high, Cycles step, Cycles now) {
    run(now);
    const auto from = bias(now);
    const auto to = high ? 0x200U : 0U;
    const auto steps = from > to ? from - to : to - from;
    if (steps == 0) return 0;
    ramp_ = {from, to, now, std::max<Cycles>(step, 1), true};
    return steps * ramp_.step;
}

// Biais à `now`, rampe comprise : un pas au départ, puis un par période.
std::uint32_t Sound::bias(Cycles now) noexcept {
    if (!ramp_.active) return bias_;
    const auto steps = ramp_.from > ramp_.to ? ramp_.from - ramp_.to : ramp_.to - ramp_.from;
    const auto done = now < ramp_.start ? Cycles{0} : 1 + (now - ramp_.start) / ramp_.step;
    if (done >= steps) {
        bias_ = ramp_.to;
        ramp_.active = false;
        return bias_;
    }
    const auto moved = static_cast<std::uint32_t>(done);
    return ramp_.from > ramp_.to ? ramp_.from - moved : ramp_.from + moved;
}

void Sound::run(Cycles now) {
    while (time_ + cycles_per_sample <= now) {
        time_ += cycles_per_sample;
        mix();
        for (auto& channel : channels_) channel.advance(cycles_per_sample / 2, source_);
        for (unsigned index = 0; index < 2; ++index)
            captures_[index].advance(cycles_per_sample / 2, channels_[2 * index + 1].timer(), capture_input_[index], sink_);
    }
}

// Canaux a = 2x et b = 2x + 1, autour de la capture x : addition, et
// entrée de la capture quand elle lit le canal a.
void Sound::add_channels(std::array<std::int64_t, 16>& volume) {
    for (unsigned index = 0; index < 2; ++index) {
        const auto a = 2 * index;
        const auto b = a + 1;
        const auto a_enabled = channels_[a].enabled();
        if (captures_[index].adds()) {
            capture_input_[index] = {wrap16((a_enabled ? volume[a] : 0) + volume[b], volume_fraction), volume_fraction};
            const auto excluded = (control_ & (index == 0 ? 0x1000U : 0x2000U)) != 0;
            volume[a] = a_enabled ? volume[a] + (excluded ? 0 : volume[b]) : 0;
            volume[b] = 0;
        } else {
            const auto both_negative = volume[a] < 0 && volume[b] < 0;
            capture_input_[index] = {both_negative ? -(std::int64_t{0x8000} << volume_fraction) : volume[a], volume_fraction};
        }
    }
}

void Sound::mix() {
    std::array<std::int64_t, 16> volume{};
    for (unsigned index = 0; index < channels_.size(); ++index) {
        const auto control = channels_[index].control();
        static constexpr std::array<unsigned, 4> divider{0, 1, 2, 4};
        volume[index] = ((std::int64_t{channels_[index].sample()} * 16) >> divider[(control >> 8U) & 3U]) * factor(control & 0x7fU);
    }
    add_channels(volume);
    std::array<std::int64_t, 2> mixer{};
    std::array<std::array<std::int64_t, 2>, 2> direct{};  // canaux 1 et 3, gauche et droite
    for (unsigned index = 0; index < channels_.size(); ++index) {
        const auto pan = factor((channels_[index].control() >> 16U) & 0x7fU);
        const std::array<std::int64_t, 2> sides{(volume[index] * (128 - pan)) >> 10, (volume[index] * pan) >> 10};
        if (index == 1 || index == 3) direct[index / 2] = sides;
        const auto removed = (index == 1 && (control_ & 0x1000U) != 0) || (index == 3 && (control_ & 0x2000U) != 0);
        if (!removed) {
            mixer[0] += sides[0];
            mixer[1] += sides[1];
        }
    }
    for (unsigned index = 0; index < 2; ++index)
        if (!captures_[index].from_channel())
            capture_input_[index] = {std::clamp<std::int64_t>(mixer[index], -0x80'0000, 0x7f'ffff), mixer_fraction};
    const auto master = (control_ & 0x8000U) != 0 ? factor(control_ & 0x7fU) : 0;
    const auto level_bias = bias(time_);
    for (unsigned side = 0; side < 2; ++side) {
        std::int64_t level = mixer[side];
        switch ((control_ >> (8U + side * 2U)) & 3U) {
        case 1: level = direct[0][side]; break;
        case 2: level = direct[1][side]; break;
        case 3: level = direct[0][side] + direct[1][side]; break;
        default: break;
        }
        // × maître / 128 / 64, partie entière, + biais, bornes 0..3FFh.
        const auto value = std::clamp<std::int64_t>(((level * master) >> 21) + level_bias, 0, 0x3ff);
        if (output_.size() < buffer_limit) output_.push_back(static_cast<std::int16_t>((value - 0x200) * 64));
    }
}

} // namespace ravenemu::nds
