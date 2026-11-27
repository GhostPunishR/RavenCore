#include "sound/sound_channel.hpp"

namespace ravenemu::nds {

namespace {

constexpr unsigned pcm8 = 0;
constexpr unsigned pcm16 = 1;
constexpr unsigned ima_adpcm = 2;
constexpr unsigned psg = 3;
constexpr std::uint32_t busy = 0x8000'0000U;

} // namespace

void SoundChannel::write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask) noexcept {
    const auto merge = [&](std::uint32_t old) { return (old & ~mask) | (value & mask); };
    switch (offset) {
    case 0x0: {
        const auto was_busy = (control_ & busy) != 0;
        control_ = merge(control_) & 0xff7f'837fU;
        if (!was_busy && (control_ & busy) != 0) start();
        if ((control_ & busy) == 0) playing_ = false;
        break;
    }
    case 0x4: source_ = merge(source_) & 0x07ff'fffcU; break;
    case 0x8: {
        const auto word = merge(std::uint32_t{timer_} | (std::uint32_t{loop_} << 16U));
        timer_ = static_cast<std::uint16_t>(word);
        loop_ = static_cast<std::uint16_t>(word >> 16U);
        break;
    }
    case 0xc: length_ = merge(length_) & 0x003f'ffffU; break;
    default: break;
    }
}

std::uint32_t SoundChannel::end() const noexcept {
    const auto bytes = (std::uint32_t{loop_} + length_) * 4U;
    return format() == ima_adpcm ? bytes * 2U : bytes;
}

std::uint32_t SoundChannel::loop_start() const noexcept {
    const auto bytes = std::uint32_t{loop_} * 4U;
    return format() == ima_adpcm ? bytes * 2U : bytes;
}

void SoundChannel::start() noexcept {
    playing_ = true;
    finished_ = false;
    counter_ = timer_;
    position_ = 0;
    step_ = 0;
    noise_ = 0x7fff;
    loop_saved_ = false;
    delay_ = format() == psg ? 1U : (format() == ima_adpcm ? 11U : 3U);
    start_delay_ = delay_;
}

void SoundChannel::advance(std::uint32_t ticks, const SampleSource& source) {
    while (playing_ || finished_) {
        const auto until_overflow = 0x10000U - counter_;
        if (ticks < until_overflow) {
            counter_ += ticks;
            return;
        }
        ticks -= until_overflow;
        counter_ = timer_;
        next_sample(source);
    }
}

void SoundChannel::next_sample(const SampleSource& source) {
    if (finished_) {  // fin de la période du dernier échantillon
        finished_ = false;
        if ((control_ & 0x8000U) == 0) output_ = 0;  // sans Hold, la sortie retombe
        return;
    }
    if (delay_ > 0) {
        const auto first = delay_ == start_delay_;
        --delay_;
        if (delay_ == 0 && format() == ima_adpcm) {  // en-tête de l'ADPCM
            const auto header = source(source_, 4);
            adpcm_.pcm = static_cast<std::int16_t>(header & 0xffffU);
            adpcm_.index = static_cast<int>((header >> 16U) & 0x7fU) > 88 ? 88 : static_cast<int>((header >> 16U) & 0x7fU);
            position_ = 8;  // les 8 quartets de l'en-tête
        }
        if (!first || (control_ & 0x8000U) == 0) output_ = 0;
        return;
    }
    output_ = fetch(source);
}

std::int32_t SoundChannel::fetch(const SampleSource& source) {
    constexpr std::int32_t high = 0x7fff;
    constexpr std::int32_t low = -0x7fff;
    switch (format()) {
    case psg: {
        if (index_ >= 14) {  // bruit
            const auto carry = (noise_ & 1U) != 0;
            noise_ >>= 1U;
            if (carry) noise_ ^= 0x6000U;
            return carry ? low : high;
        }
        if (index_ < 8) return 0;
        const auto duty = (control_ >> 24U) & 7U;
        const auto highs = duty == 7 ? 0U : duty + 1U;
        const auto value = (step_ % 8U) >= 8U - highs ? high : low;
        step_ = (step_ + 1U) % 8U;
        return value;
    }
    case pcm8: {
        const auto value = static_cast<std::int8_t>(source(source_ + position_, 1)) * 256;
        position_ += 1;
        reach_end();
        return value;
    }
    case pcm16: {
        const auto value = static_cast<std::int16_t>(source(source_ + position_, 2));
        position_ += 2;
        reach_end();
        return value;
    }
    default: {  // IMA-ADPCM
        if (position_ == loop_start() && !loop_saved_) {
            loop_state_ = adpcm_;
            loop_saved_ = true;
        }
        const auto byte = source(source_ + position_ / 2U, 1);
        adpcm::decode(adpcm_, (position_ & 1U) != 0 ? byte >> 4U : byte & 15U);
        position_ += 1;
        const auto value = adpcm_.pcm;
        reach_end();
        return value;
    }
    }
}

// Fin des données : boucle, coup unique, ou lecture continue (manuel).
void SoundChannel::reach_end() noexcept {
    if (position_ < end()) return;
    switch ((control_ >> 27U) & 3U) {
    case 1:
        position_ = loop_start();
        if (format() == ima_adpcm && loop_saved_) adpcm_ = loop_state_;
        break;
    case 2:
        control_ &= ~busy;
        playing_ = false;
        finished_ = true;
        break;
    default: break;
    }
}

} // namespace ravenemu::nds
