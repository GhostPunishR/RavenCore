#include <ravenemu/bridge.h>
#include <synthetic_roms.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    if (ravenemu_v3_create(99)) {
        std::cerr << "Invalid platform was accepted\n";
        return 1;
    }
    for (int system = RAVENEMU_V3_GB; system <= RAVENEMU_V3_GBA; ++system) {
        std::unique_ptr<ravenemu_v3_bridge, decltype(&ravenemu_v3_destroy)> handle(
            ravenemu_v3_create(system), ravenemu_v3_destroy);
        if (!handle) return 2;
        auto rom = system == RAVENEMU_V3_GBA
            ? ravenemu::testing::minimal_gba_rom()
            : ravenemu::testing::minimal_game_boy_rom();
        if (system == RAVENEMU_V3_GBC) rom[0x143] = 0x80;
        if (!ravenemu_v3_load(handle.get(), rom.data(), rom.size(), nullptr, 0)) {
            std::cerr << ravenemu_v3_last_error() << "\n";
            return 3;
        }
        const auto width = ravenemu_v3_video_width(handle.get());
        const auto height = ravenemu_v3_video_height(handle.get());
        if (width != (system == RAVENEMU_V3_GBA ? 240 : 160) ||
            height != (system == RAVENEMU_V3_GBA ? 160 : 144)) return 4;
        std::vector<std::uint32_t> frame(static_cast<size_t>(width * height));
        if (ravenemu_v3_run_frame(handle.get(), frame.data(), frame.size() - 1)) return 5;
        if (!ravenemu_v3_run_frame(handle.get(), frame.data(), frame.size())) {
            std::cerr << ravenemu_v3_last_error() << "\n"; return 6;
        }
        if ((frame[0] & 0xFF000000u) != 0xFF000000u) return 7;
        if (!ravenemu_v3_set_button(handle.get(), RAVENEMU_V3_A, 1)) return 8;
        if (!ravenemu_v3_set_button(handle.get(), RAVENEMU_V3_A, 0)) return 9;
        std::vector<std::int16_t> audio(4096);
        ravenemu_v3_read_audio(handle.get(), audio.data(), audio.size());
        if (ravenemu_v3_audio_rate(handle.get()) <= 0) return 10;
        const auto size = ravenemu_v3_state_size(handle.get());
        if (size == 0) return 11;
        std::vector<std::uint8_t> state(size);
        if (!ravenemu_v3_save_state(handle.get(), state.data(), state.size())) return 12;
        if (!ravenemu_v3_load_state(handle.get(), state.data(), state.size())) {
            std::cerr << ravenemu_v3_last_error() << "\n"; return 13;
        }
        if (!ravenemu_v3_reset(handle.get())) return 14;
    }
    std::cout << "Portable bridge GB/GBC/GBA smoke tests OK\n";
}
