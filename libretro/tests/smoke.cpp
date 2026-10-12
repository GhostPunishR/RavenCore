#include <libretro.h>
#include <synthetic_roms.hpp>
#include <algorithm>
#include <iterator>
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
unsigned rumble_calls = 0;
std::uint16_t rumble_strength = 0;
unsigned descriptor_count = 0;
bool descriptors_have_lr = false;
bool descriptors_have_tilt = false;
unsigned analog_queries = 0;

bool rumble(unsigned port, retro_rumble_effect effect, std::uint16_t strength) {
    if (port == 0 && effect == RETRO_RUMBLE_STRONG) {
        ++rumble_calls;
        rumble_strength = strength;
    }
    return true;
}

bool environment(unsigned command, void* value) {
    if (command == RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE) {
        static_cast<retro_rumble_interface*>(value)->set_rumble_state = rumble;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS) {
        descriptor_count = 0;
        descriptors_have_lr = descriptors_have_tilt = false;
        for (auto* d = static_cast<const retro_input_descriptor*>(value); d->description; ++d) {
            ++descriptor_count;
            descriptors_have_lr |= d->device == RETRO_DEVICE_JOYPAD &&
                (d->id == RETRO_DEVICE_ID_JOYPAD_L || d->id == RETRO_DEVICE_ID_JOYPAD_R);
            descriptors_have_tilt |= d->device == RETRO_DEVICE_ANALOG;
        }
        return true;
    }
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
std::int16_t input_state(unsigned, unsigned device, unsigned, unsigned) {
    if (device == RETRO_DEVICE_ANALOG) ++analog_queries;
    return 0;
}
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
#if defined(RAVENEMU_LIBRETRO_GBA)
    constexpr unsigned expected_descriptors = 10;
#else
    constexpr unsigned expected_descriptors = 8;
#endif
    if (descriptor_count != expected_descriptors || descriptors_have_tilt ||
        descriptors_have_lr != (expected_descriptors == 10)) {
        std::cerr << "Unexpected input descriptors\n";
        return 1;
    }
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
    if (analog_queries != 0 || rumble_calls != 0) {
        std::cerr << "Stick polled or motor driven without a tilt or rumble cartridge\n";
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
#if !defined(RAVENEMU_LIBRETRO_GBA)
    // MBC5+RUMBLE program: LD A,$08 ; LD ($4000),A ; JR $ (motor on, then loop).
    auto rumble_rom = ravenemu::testing::minimal_game_boy_rom();
    const std::uint8_t motor_on[] = {0x3e, 0x08, 0xea, 0x00, 0x40, 0x18, 0xfe};
    std::copy(std::begin(motor_on), std::end(motor_on), rumble_rom.begin() + 0x100);
    rumble_rom[0x147] = 0x1c;
    game.data = rumble_rom.data();
    game.size = rumble_rom.size();
    if (!retro_load_game(&game)) return 1;
    retro_run();
    if (rumble_calls != 1 || rumble_strength != 0xffff) {
        std::cerr << "Rumble motor not started\n";
        return 1;
    }
    retro_unload_game();
    if (rumble_calls != 2 || rumble_strength != 0) {
        std::cerr << "Rumble motor left running after unload\n";
        return 1;
    }

    // MBC5+RAM+BATTERY program: enable RAM, store $77 at $A000, loop. The
    // write must reach SAVE_RAM, which the frontend persists.
    auto battery_rom = ravenemu::testing::minimal_game_boy_rom();
    const std::uint8_t store[] = {0x3e, 0x0a, 0xea, 0x00, 0x00, 0x3e, 0x77, 0xea, 0x00, 0xa0, 0x18, 0xfe};
    std::copy(std::begin(store), std::end(store), battery_rom.begin() + 0x100);
    battery_rom[0x147] = 0x1b;
    battery_rom[0x149] = 0x02;
    game.data = battery_rom.data();
    game.size = battery_rom.size();
    if (!retro_load_game(&game)) return 1;
    auto* save_ram = static_cast<std::uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    if (!save_ram || retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) != 8192 || save_ram[0] == 0x77) return 1;
    retro_run();
    if (save_ram[0] != 0x77) {
        std::cerr << "Battery write not mirrored into SAVE_RAM\n";
        return 1;
    }
    retro_unload_game();

    // An MBC7 header announces the accelerometer and polls the left stick.
    auto tilt_rom = ravenemu::testing::minimal_game_boy_rom();
    tilt_rom[0x147] = 0x22;
    game.data = tilt_rom.data();
    game.size = tilt_rom.size();
    if (!retro_load_game(&game) || !descriptors_have_tilt || descriptor_count != 10) {
        std::cerr << "MBC7 tilt not announced\n";
        return 1;
    }
    analog_queries = 0;
    retro_run();
    if (analog_queries != 2) {
        std::cerr << "MBC7 tilt not polled\n";
        return 1;
    }
    retro_unload_game();
#endif
    retro_deinit();
    std::cout << system.library_name << " Libretro smoke test passed\n";
}
