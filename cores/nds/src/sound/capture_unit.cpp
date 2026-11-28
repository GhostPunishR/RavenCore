#include "sound/capture_unit.hpp"

namespace ravenemu::nds {

namespace {

constexpr std::uint32_t busy = 0x80U;
constexpr std::uint32_t one_shot = 0x04U;
constexpr std::uint32_t pcm8 = 0x08U;

} // namespace

void CaptureUnit::write_control(std::uint32_t value, std::uint16_t timer) noexcept {
    const auto was_busy = (control_ & busy) != 0;
    control_ = value & 0x8fU;
    if (!was_busy && (control_ & busy) != 0) {
        counter_ = timer;
        position_ = 0;
    }
}

void CaptureUnit::write_destination(std::uint32_t value, std::uint32_t mask) noexcept {
    destination_ = ((destination_ & ~mask) | (value & mask)) & 0x07ff'fffcU;
}

void CaptureUnit::write_length(std::uint32_t value, std::uint32_t mask) noexcept {
    length_ = static_cast<std::uint16_t>((length_ & ~mask) | (value & mask));
}

void CaptureUnit::advance(std::uint32_t ticks, std::uint16_t timer, CaptureInput input, const SampleSink& sink) {
    while ((control_ & busy) != 0) {
        const auto until_overflow = 0x10000U - counter_;
        if (ticks < until_overflow) {
            counter_ += ticks;
            return;
        }
        ticks -= until_overflow;
        counter_ = timer;
        capture(input, sink);
    }
}

std::int32_t CaptureUnit::round(CaptureInput input) noexcept {
    auto result = input.value >> input.fraction;
    if (input.fraction > 0 && input.value < 0 && ((input.value >> (input.fraction - 1U)) & 1) != 0) ++result;
    return static_cast<std::int32_t>(result);
}

void CaptureUnit::capture(CaptureInput input, const SampleSink& sink) {
    const auto bytes = (control_ & pcm8) != 0;
    if (bytes) input.fraction += 8;  // ÷ 100h
    const auto sample = static_cast<std::uint32_t>(round(input));
    const auto width = bytes ? 1U : 2U;
    sink(destination_ + position_, sample & (bytes ? 0xffU : 0xffffU), width);
    position_ += width;
    const auto end = std::uint32_t{length_ == 0 ? std::uint16_t{1} : length_} * 4U;
    if (position_ < end) return;
    position_ = 0;
    if ((control_ & one_shot) != 0) control_ &= ~busy;
}

} // namespace ravenemu::nds
