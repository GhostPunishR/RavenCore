#include <libretro.h>
#include <synthetic_roms.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
unsigned video_frames = 0;
unsigned last_width = 0, last_height = 0;
bool pixel_format_requested = false;
bool achievements_advertised = true;

bool environment(unsigned command, void* value) {
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        pixel_format_requested = true;
        return *static_cast<retro_pixel_format*>(value) == RETRO_PIXEL_FORMAT_XRGB8888;
    }
    if (command == RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS) {
        achievements_advertised = *static_cast<bool*>(value);
        return true;
    }
    return false;
}
void video(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (data && pitch == width * sizeof(std::uint32_t)) {
        ++video_frames;
        last_width = width;
        last_height = height;
    }
}
std::size_t audio(const std::int16_t*, std::size_t count) { return count; }
void input_poll() {}
std::int16_t input_state(unsigned, unsigned, unsigned, unsigned) { return 0; }
}

int main() {
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    retro_system_info system{};
    retro_get_system_info(&system);
    if (retro_api_version() != RETRO_API_VERSION || !system.library_name ||
        !system.valid_extensions || achievements_advertised ||
        !system.library_version || std::string(system.library_version) != RAVENEMU_VERSION) {
        std::cerr << "Invalid Libretro API metadata or unsupported achievements flag\n";
        return 1;
    }

#if defined(RAVENEMU_LIBRETRO_GBA)
    auto rom = ravenemu::testing::minimal_gba_rom();
    const char marker[] = "EEPROM_V";
    std::copy_n(marker, 8, rom.begin() + 0x100);
    constexpr unsigned expected_width = 240, expected_height = 160;
#else
    auto rom = ravenemu::testing::minimal_game_boy_rom();
#if defined(RAVENEMU_LIBRETRO_GBC)
    rom[0x143] = 0x80;
#endif
    constexpr unsigned expected_width = 160, expected_height = 144;
#endif

    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    if (!retro_load_game(&game) || !pixel_format_requested) {
        std::cerr << "Synthetic ROM did not load through Libretro\n";
        return 1;
    }
    retro_system_av_info av{};
    retro_get_system_av_info(&av);
    if (av.geometry.base_width != expected_width ||
        av.geometry.base_height != expected_height || av.timing.sample_rate <= 0) {
        std::cerr << "Invalid Libretro AV metadata\n";
        return 1;
    }
#if defined(RAVENEMU_LIBRETRO_GBA)
    auto* battery = static_cast<std::uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    if (!battery || retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) != 8192) return 1;
    battery[8184] = 0x5a; // Frontend import before the first frame.
#endif
    retro_run();
    retro_run();
#if defined(RAVENEMU_LIBRETRO_GBA)
    if (retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) != battery || battery[8184] != 0x5a) return 1;
#endif
    if (video_frames != 2 || last_width != expected_width || last_height != expected_height) {
        std::cerr << "Libretro video callback missing\n";
        return 1;
    }
    if (retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) ||
        retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) != 0) {
        std::cerr << "Misleading system RAM mapping exposed\n";
        return 1;
    }

    const auto state_size = retro_serialize_size();
    if (state_size < 16 || state_size > 4U * 1024U * 1024U) {
        std::cerr << "Invalid state size\n";
        return 1;
    }
    std::vector<std::uint8_t> state(state_size);
    if (!retro_serialize(state.data(), state.size()) ||
        !retro_unserialize(state.data(), state.size())) {
        std::cerr << "State round-trip failed\n";
        return 1;
    }
    retro_reset();
    retro_unload_game();
    if (retro_serialize_size() != 0) {
        std::cerr << "State still present after unload\n";
        return 1;
    }
    retro_deinit();
    std::cout << system.library_name << " Libretro smoke test passed\n";
}
