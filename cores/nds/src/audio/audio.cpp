#include <ravenemu/nds/audio.hpp>
#include <ravenemu/nds/hardware.hpp>

#include <algorithm>
#include <bit>

namespace ravenemu::nds {
namespace {
constexpr std::uint32_t busy = 0x80000000U;
constexpr std::array<int,89> steps{
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
    73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
    408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,
    1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,
    6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,
    20350,22385,24623,27086,29794,32767};
constexpr std::array<int,8> index_change{-1,-1,-1,-1,2,4,6,8};
constexpr unsigned gain(unsigned value) noexcept { return value == 127 ? 128 : value; }
constexpr unsigned period(std::uint16_t timer) noexcept { return 2U * (65536U - timer); }
} // namespace

void Audio::reset() noexcept { *this = Audio{}; }

std::uint8_t Audio::read_byte(unsigned offset) const noexcept {
    if (offset < 0x100)
        return (offset & 15U) < 4 ? static_cast<std::uint8_t>(channels_[offset / 16].control >> ((offset & 3U) * 8U)) : std::uint8_t{0};
    const auto value = offset < 0x104 ? control_ : bias_;
    return offset < 0x108 ? static_cast<std::uint8_t>(static_cast<unsigned>(value) >> ((offset & 3U) * 8U)) : std::uint8_t{0};
}

void Audio::synchronize() noexcept {
    sample_phase_ += pending_ * sample_rate;
    for (unsigned n = 0; n < 16; ++n) if (running_ & (1U << n)) channels_[n].remaining -= pending_;
    pending_ = 0;
}

void Audio::schedule() noexcept {
    next_event_ = (timer_clock_hz - sample_phase_ + sample_rate - 1U) / sample_rate;
    for (unsigned n = 0; n < 16; ++n) if (running_ & (1U << n))
        next_event_ = std::min(next_event_, channels_[n].remaining);
}

void Audio::start(unsigned number) noexcept {
    auto& ch = channels_[number];
    ch.position = ch.phase = ch.index = ch.loop_index = 0;
    ch.sample = ch.predictor = ch.loop_predictor = 0;
    ch.header = ch.loop_saved = false; ch.noise = 0x7fff;
    const auto format = (ch.control >> 29U) & 3U;
    ch.delay = format == 3 ? 1 : format == 2 ? 11 : 3;
    ch.remaining = period(ch.timer);
    // Les buffers trop courts restent occupés, sans produire de son.
    const bool valid = format == 3 ? number >= 8 : ch.loop + ch.length >= 4;
    if (valid) running_ |= static_cast<std::uint16_t>(1U << number);
    else running_ &= static_cast<std::uint16_t>(~(1U << number));
}

void Audio::write_byte(unsigned offset, std::uint8_t value) noexcept {
    synchronize();
    const auto shift = (offset & 3U) * 8U;
    const auto merge = [=](std::uint32_t old) { return (old & ~(0xffU << shift)) | (std::uint32_t{value} << shift); };
    if (offset < 0x100) {
        const auto number = offset / 16;
        auto& ch = channels_[number];
        switch (offset & 12U) {
        case 0: {
            const auto old = ch.control;
            ch.control = merge(old) & 0xff7f837fU;
            if (!(old & busy) && (ch.control & busy)) start(number);
            else if ((old & busy) && !(ch.control & busy)) {
                running_ &= static_cast<std::uint16_t>(~(1U << number)); ch.sample = 0;
            } else if (!(ch.control & 0x8000U) && !(ch.control & busy) && !(running_ & (1U << number))) ch.sample = 0;
            break;
        }
        case 4: ch.source = merge(ch.source) & 0x07fffffcU; break;
        case 8: {
            const auto combined = merge(ch.timer | (std::uint32_t{ch.loop} << 16U));
            ch.timer = static_cast<std::uint16_t>(combined); ch.loop = static_cast<std::uint16_t>(combined >> 16U); break;
        }
        case 12: ch.length = merge(ch.length) & 0x3fffffU; break;
        }
    } else if (offset < 0x104) control_ = static_cast<std::uint16_t>(merge(control_) & 0xbf7fU);
    else if (offset < 0x108) bias_ = static_cast<std::uint16_t>(merge(bias_) & 0x3ffU);
    schedule();
}

void Audio::tick(unsigned number, Reader read, const void* context) noexcept {
    auto& ch = channels_[number];
    ch.remaining = period(ch.timer);
    if (!(ch.control & busy)) {
        if (!(ch.control & 0x8000U)) ch.sample = 0;
        running_ &= static_cast<std::uint16_t>(~(1U << number)); return;
    }
    const auto format = (ch.control >> 29U) & 3U;
    const auto byte = [&](std::uint32_t offset) { return read(context,(ch.source + offset) & 0x07ffffffU); };
    if (format == 2 && !ch.header) {
        ch.predictor = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(byte(0) | (unsigned{byte(1)} << 8U)));
        ch.index = std::min(unsigned{byte(2)} & 0x7fU,88U);
        ch.position = 8; ch.header = true;
    }
    if (ch.delay && --ch.delay) return;
    if (format == 3) {
        if (number < 8) { ch.sample = 0; return; }
        if (number < 14) {
            const auto duty = (ch.control >> 24U) & 7U;
            ch.sample = duty != 7 && ch.phase >= 7U - duty ? 32767 : -32767;
            ch.phase = (ch.phase + 1U) & 7U;
        } else {
            const bool low = (ch.noise & 1U) != 0;
            ch.noise = static_cast<std::uint16_t>((ch.noise >> 1U) ^ (low ? 0x6000U : 0U));
            ch.sample = low ? -32767 : 32767;
        }
        return;
    }
    // Position en octets PCM, en demi-octets ADPCM (en-tête compris).
    const auto scale = format == 2 ? 8U : 4U;
    const auto loop = ch.loop * scale, end = (ch.loop + ch.length) * scale;
    if (format == 2) {
        if (ch.position == loop && !ch.loop_saved) {
            ch.loop_predictor = ch.predictor; ch.loop_index = ch.index; ch.loop_saved = true;
        }
        const auto code = (unsigned{byte(ch.position / 2)} >> ((ch.position & 1U) * 4U)) & 15U;
        const auto step = steps[ch.index];
        auto delta = step / 8;
        if (code & 1U) delta += step / 4;
        if (code & 2U) delta += step / 2;
        if (code & 4U) delta += step;
        ch.predictor = code & 8U ? std::max(ch.predictor - delta,-32767) : std::min(ch.predictor + delta,32767);
        ch.index = static_cast<unsigned>(std::clamp(static_cast<int>(ch.index) + index_change[code & 7U],0,88));
        ch.sample = ch.predictor; ++ch.position;
    } else if (format == 0) {
        ch.sample = std::bit_cast<std::int8_t>(byte(ch.position)) * 256; ++ch.position;
    } else {
        ch.sample = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(byte(ch.position) | (unsigned{byte(ch.position + 1)} << 8U)));
        ch.position += 2;
    }
    const auto repeat = (ch.control >> 27U) & 3U;
    if (ch.position >= end && repeat == 1) {
        ch.position = loop;
        if (format == 2 && ch.loop_saved) { ch.predictor = ch.loop_predictor; ch.index = ch.loop_index; }
    } else if (ch.position >= end && repeat == 2) ch.control &= ~busy;
}

std::array<std::int16_t,2> Audio::mix() const noexcept {
    if (!speakers_enabled_) return {};
    // Fixed point : 8 bits fractionnaires conservés jusqu'au mélange.
    std::array<std::int64_t,2> mixed{}, one{}, three{};
    if (control_ & 0x8000U) for (unsigned n = 0; n < 16; ++n) {
        const auto& ch = channels_[n];
        const auto div = (ch.control >> 8U) & 3U;
        const auto volume = gain(ch.control & 127U), pan = gain((ch.control >> 16U) & 127U);
        const auto scaled = std::int64_t{ch.sample} * volume * 16 / (div == 3 ? 16 : 1U << div);
        const std::array<std::int64_t,2> stereo{(scaled * (128U - pan)) >> 7, (scaled * pan) >> 7};
        // scaled a 11 bits fractionnaires ; ramener à 8 après le panoramique.
        const std::array<std::int64_t,2> signal{stereo[0] >> 3,stereo[1] >> 3};
        if (n == 1) one = signal;
        if (n == 3) three = signal;
        if ((n != 1 || !(control_ & 0x1000U)) && (n != 3 || !(control_ & 0x2000U)))
            for (unsigned side = 0; side < 2; ++side) mixed[side] += signal[side];
    }
    std::array<std::int16_t,2> result{};
    for (unsigned side = 0; side < 2; ++side) {
        const auto select = (control_ >> (8U + side * 2U)) & 3U;
        const auto selected = select == 0 ? mixed[side] : select == 1 ? one[side] : select == 2 ? three[side] : one[side] + three[side];
        const auto dac = std::clamp<std::int64_t>(((selected * gain(control_ & 127U)) >> 21) + bias_,0,1023);
        result[side] = static_cast<std::int16_t>((dac - 512) * 64);
    }
    return result;
}

void Audio::enqueue(std::array<std::int16_t,2> frame) noexcept {
    if (size_ == buffer_frames) { head_ = (head_ + 1) % buffer_frames; --size_; }
    const auto offset = (head_ + size_) % buffer_frames * 2;
    buffer_[offset] = frame[0]; buffer_[offset + 1] = frame[1]; ++size_;
}

void Audio::advance_cycles(std::uint64_t cycles, Reader read, const void* context) noexcept {
    while (cycles) {
        const auto distance = next_event_ - pending_;
        if (cycles < distance) { pending_ += static_cast<std::uint32_t>(cycles); return; }
        if (!running_) {
            synchronize();
            // Une avance silencieuse de UINT64_MAX reste bornée et sans débordement.
            const auto fraction = (cycles % timer_clock_hz) * sample_rate + sample_phase_;
            const auto count = cycles / timer_clock_hz >= 1 ? buffer_frames :
                std::min<std::uint64_t>(fraction / timer_clock_hz,buffer_frames);
            sample_phase_ = static_cast<std::uint32_t>(fraction % timer_clock_hz);
            const auto frame = mix();
            for (std::size_t i = 0; i < count; ++i) enqueue(frame);
            schedule(); return;
        }
        cycles -= distance; pending_ = next_event_; synchronize();
        for (unsigned n = 0; n < 16; ++n) if ((running_ & (1U << n)) && !channels_[n].remaining) tick(n,read,context);
        if (sample_phase_ >= timer_clock_hz) { sample_phase_ -= timer_clock_hz; enqueue(mix()); }
        schedule();
    }
}

std::size_t Audio::read_samples(std::span<std::int16_t> output) noexcept {
    const auto frames = std::min(output.size() / 2,size_);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto offset = (head_ + i) % buffer_frames * 2;
        output[i * 2] = buffer_[offset]; output[i * 2 + 1] = buffer_[offset + 1];
    }
    head_ = (head_ + frames) % buffer_frames; size_ -= frames; return frames * 2;
}

} // namespace ravenemu::nds
