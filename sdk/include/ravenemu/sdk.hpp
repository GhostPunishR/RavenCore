#pragma once

#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gbc/core.hpp>
#include <memory>

namespace ravenemu::sdk {

/** Header-only convenience API over RavenCore's native C++20 core factories. */
enum class System { game_boy, game_boy_color, game_boy_advance };

[[nodiscard]] inline std::unique_ptr<Core> make_core(System system) {
    switch (system) {
    case System::game_boy:         return gb::make_core(gb::HardwareModel::dmg);
    case System::game_boy_color:   return gbc::make_core();
    case System::game_boy_advance: return make_gba_core(std::nullopt);
    }
    return {};
}

} // namespace ravenemu::sdk
