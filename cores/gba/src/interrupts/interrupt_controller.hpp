#pragma once

#include "support/bits.hpp"

namespace ravenemu::gba {

class InterruptController {
public:
    enum Bit { vblank = 0, hblank = 1, vcount = 2, timer0 = 3, dma0 = 8 };
    void request(int bit) {
        const auto mask = 1 << bit;
        flags |= mask;
        if (on_request) on_request(mask);
        changed();
    }
    void acknowledge(int value) {
        flags &= ~value;
        changed();
    }
    void set_enable(int value) {
        enable = value;
        changed();
    }
    void set_master_enable(bool value) {
        master_enable = value;
        changed();
    }
    // Ligne d'IRQ vers le CPU : IME et au moins une source à la fois activée
    // dans IE et signalée dans IF.
    [[nodiscard]] bool pending() const noexcept {
        return master_enable && (enable & flags & 0x3fff) != 0;
    }
    void reset() {
        enable = 0;
        flags = 0;
        master_enable = false;
        changed();
    }
    int enable{};
    int flags{};
    bool master_enable{};
    std::function<void(int)> on_request;
    // Appelé après tout changement qui peut lever ou baisser la ligne.
    std::function<void()> on_change;

private:
    void changed() {
        if (on_change) on_change();
    }
};

} // namespace ravenemu::gba
