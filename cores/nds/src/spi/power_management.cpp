#include <ravenemu/nds/power_management.hpp>

namespace ravenemu::nds {

void PowerManagement::reset() noexcept {
    control_ = 0x0d;
    microphone_control_ = 0;
    microphone_gain_ = 1;
}

std::uint8_t PowerManagement::exchange(Session& session, std::uint8_t output) const noexcept {
    if (!session.selected) {
        session.index = output;
        session.selected = true;
        return 0;
    }
    if (!(session.index & 0x80U)) {
        session.data = output;
        session.write_pending = true;
        return 0;
    }
    // DS originale : les indices 4..7f sont des miroirs de 0..3.
    switch (session.index & 3U) {
    case 0: return control_;
    case 1: return battery_low_ ? 1 : 0;
    case 2: return microphone_control_;
    default: return microphone_gain_;
    }
}

void PowerManagement::complete_byte(Session& session) noexcept {
    if (!session.write_pending || powered_off()) return;
    switch (session.index & 3U) {
    case 0: control_ = session.data & 0x7fU; break;
    case 1: break; // statut batterie en lecture seule
    case 2: microphone_control_ = session.data & 1U; break;
    case 3: microphone_gain_ = session.data & 3U; break;
    }
    session.write_pending = false;
}

} // namespace ravenemu::nds
