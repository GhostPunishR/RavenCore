// Inclut chaque en-tête public installé et appelle les deux interfaces.
#include <ravenemu/bridge.h>
#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gba/core.hpp>
#include <ravenemu/gbc/core.hpp>
#include <ravenemu/sdk.hpp>

#include <cstdio>

int main() {
    using ravenemu::sdk::System;
    for (const auto system : {System::game_boy, System::game_boy_color, System::game_boy_advance}) {
        if (!ravenemu::sdk::make_core(system)) return 1;
    }
    if (ravenemu::gba::make_core()->console() != ravenemu::Console::game_boy_advance) return 1;

    for (const int system : {RAVENEMU_V3_GB, RAVENEMU_V3_GBC, RAVENEMU_V3_GBA}) {
        auto* handle = ravenemu_v3_create(system);
        if (handle == nullptr) {
            std::fprintf(stderr, "ravenemu_v3_create : %s\n", ravenemu_v3_last_error());
            return 1;
        }
        ravenemu_v3_destroy(handle);
    }
    std::puts("paquet RavenEmuCores consommé avec succès");
    return 0;
}
