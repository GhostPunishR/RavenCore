#pragma once

#include <ravenemu/core.hpp>
#include <ravenemu/gbc/core.hpp>
#include <memory>

namespace ravenemu::sdk {

/** Header-only convenience API over RavenCore's native C++20 core factories. */
enum class System {
    game_boy,
    game_boy_color,
    game_boy_advance,
    game_boy_color_on_gba,  ///< la GBC d'une Game Boy Advance (gbc::HardwareModel::agb)
};

[[nodiscard]] inline std::unique_ptr<Core> make_core(System system) {
    switch (system) {
    case System::game_boy:         return gbc::make_core(gbc::HardwareModel::dmg);
    case System::game_boy_color:   return gbc::make_core(gbc::HardwareModel::cgb);
    case System::game_boy_advance: return make_gba_core(std::nullopt);
    case System::game_boy_color_on_gba: return gbc::make_core(gbc::HardwareModel::agb);
    }
    return {};
}

} // namespace ravenemu::sdk
