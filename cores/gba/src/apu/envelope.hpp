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
private:
    int timer_{};
};

} // namespace ravenemu::gba
