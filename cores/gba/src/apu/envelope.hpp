#pragma once

#include "support/bits.hpp"

namespace ravenemu::gba {

class Envelope {
public:
    void trigger() noexcept { volume = initial_volume; timer_ = period; }
    void clock() noexcept {
        if (period == 0) return;
        if (timer_ > 0) --timer_;
        if (timer_ != 0) return;
        timer_ = period;
        if (increasing && volume < 15) ++volume;
        else if (!increasing && volume > 0) --volume;
    }
    void write(int value) noexcept {
        initial_volume = (value >> 4) & 15;
        increasing = (value & 8) != 0;
        period = value & 7;
        volume = initial_volume;
    }
    [[nodiscard]] bool dac_enabled() const noexcept { return initial_volume != 0 || increasing; }
    int initial_volume{};
    bool increasing{};
    int period{};
    int volume{};
    void save(BinaryWriter& out) const {
        out.i32(initial_volume);
        out.boolean(increasing);
        out.i32(period);
        out.i32(volume);
        out.i32(timer_);
    }
    void load(BinaryReader& in) {
        initial_volume = in.bounded_i32(0, 15);
        increasing = in.boolean();
        period = in.bounded_i32(0, 7);
        volume = in.bounded_i32(0, 15);
        timer_ = in.bounded_i32(0, 7);
    }
private:
    int timer_{};
};

} // namespace ravenemu::gba
