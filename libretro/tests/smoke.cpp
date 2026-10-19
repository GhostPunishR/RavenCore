#include <libretro.h>
#include <synthetic_roms.hpp>
#include <algorithm>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
unsigned video_frames = 0;
unsigned last_width = 0, last_height = 0;
bool pixel_format_requested = false;
bool achievements_advertised = false;
std::vector<retro_memory_descriptor> memory_map;  // dernière carte mémoire déclarée
unsigned rumble_calls = 0;
std::uint16_t rumble_strength = 0;
unsigned descriptor_count = 0;
bool descriptors_have_lr = false;
bool descriptors_have_tilt = false;
unsigned analog_queries = 0;
std::map<std::string, std::string> declared_options; // clé -> « Libellé; a|b|c »
std::map<std::string, std::string> option_values;   // réglages choisis par l'utilisateur

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
    if (command == RETRO_ENVIRONMENT_SET_VARIABLES) {
        for (auto* v = static_cast<const retro_variable*>(value); v->key; ++v) declared_options[v->key] = v->value;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_VARIABLE) {
        auto* variable = static_cast<retro_variable*>(value);
        const auto found = option_values.find(variable->key);
        variable->value = found == option_values.end() ? nullptr : found->second.c_str();
        return found != option_values.end();
    }
    if (command == RETRO_ENVIRONMENT_SET_MEMORY_MAPS) {
        const auto* map = static_cast<const retro_memory_map*>(value);
        memory_map.assign(map->descriptors, map->descriptors + map->num_descriptors);
        return true;
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
// Sans masque select, chaque descripteur a une longueur en puissance de deux
// et un départ aligné sur elle, sinon RetroArch rejette la carte.
bool memory_map_valid() {
    for (const auto& descriptor : memory_map)
        if (descriptor.select != 0 || descriptor.len == 0 || (descriptor.len & (descriptor.len - 1)) != 0 ||
            descriptor.start % descriptor.len != 0)
            return false;
    return true;
}
// Octet `address` (adresse de la console) lu comme le frontend : par la carte mémoire.
const std::uint8_t* mapped(std::size_t address) {
    for (const auto& descriptor : memory_map)
        if (descriptor.ptr && address >= descriptor.start && address - descriptor.start < descriptor.len)
            return static_cast<const std::uint8_t*>(descriptor.ptr) + descriptor.offset + (address - descriptor.start);
    return nullptr;
}
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
        !system.valid_extensions || !achievements_advertised ||
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
    // Carte mémoire de RetroAchievements : régions aux adresses de la console.
    if (!memory_map_valid()) {
        std::cerr << "Memory descriptor RetroArch would reject\n";
        return 1;
    }
#if defined(RAVENEMU_LIBRETRO_GBA)
    const auto region_length = [](std::size_t start) {
        for (const auto& descriptor : memory_map)
            if (descriptor.start == start) return descriptor.len;
        return std::size_t{0};
    };
    if (memory_map.size() != 3 || region_length(0x0300'0000) != 0x8000 || region_length(0x0200'0000) != 0x40000 ||
        region_length(0x0e00'0000) != 8192 || !mapped(0x0e00'0000 + 8184) || *mapped(0x0e00'0000 + 8184) != 0x5a) {
        std::cerr << "GBA achievement memory map missing or not mirrored\n";
        return 1;
    }
#else
    // ROM 0, VRAM, WRAM, OAM (0x80 puis 0x20 octets), HRAM, et sur GBC les
    // banques 2 à 7 de la WRAM (0x4000 puis 0x2000 octets).
#if defined(RAVENEMU_LIBRETRO_GBC)
    constexpr std::size_t expected_regions = 8;
#else
    constexpr std::size_t expected_regions = 6;
#endif
    if (memory_map.size() != expected_regions || !mapped(0x0134) || *mapped(0x0134) != rom[0x134] || mapped(0x4000) ||
        !mapped(0xc000) || mapped(0xa000) || mapped(0xff00) || !mapped(0xfe9f) || mapped(0xfea0)) {
        std::cerr << "Game Boy achievement memory map missing or wrong\n";
        return 1;
    }
#endif

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
#if defined(RAVENEMU_LIBRETRO_GBA)
    // Options déclarées avec « auto » par défaut, appliquées au chargement ;
    // la lumière du capteur solaire part de 50 %.
    if (declared_options.size() != 3 ||
        declared_options["ravenemu_gba_save_type"].find("; auto|") == std::string::npos ||
        declared_options["ravenemu_gba_rtc"].find("; auto|") == std::string::npos ||
        declared_options["ravenemu_gba_solar_light"].find("; 50|") == std::string::npos) {
        std::cerr << "GBA core options not declared\n";
        return 1;
    }
    const auto save_ram_size_with = [&](const char* save_type) {
        option_values["ravenemu_gba_save_type"] = save_type;
        const bool loaded = retro_load_game(&game);
        const auto size = loaded ? retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) : std::size_t{1};
        retro_unload_game();
        return size;
    };
    if (save_ram_size_with("sram") != 32768 || save_ram_size_with("flash128") != 131072 ||
        save_ram_size_with("none") != 0 || save_ram_size_with("auto") != 8192 ||
        save_ram_size_with("bogus") != 8192) {
        std::cerr << "GBA save type option ignored\n";
        return 1;
    }
    // L'horloge ajoute le port GPIO à l'état sauvegardé.
    const auto state_size_with = [&](const char* rtc) {
        option_values["ravenemu_gba_rtc"] = rtc;
        const auto size = retro_load_game(&game) ? retro_serialize_size() : 0;
        retro_unload_game();
        return size;
    };
    if (state_size_with("enabled") <= state_size_with("disabled") ||
        state_size_with("auto") != state_size_with("disabled")) {
        std::cerr << "GBA RTC option ignored\n";
        return 1;
    }
    option_values.clear();

    // Le choix auto doit instancier la RTC avant d'exposer SAVE_RAM au frontend.
    auto rtc_hack = ravenemu::testing::pokemon_rtc_gba_rom();
    retro_game_info rtc_game{};
    rtc_game.data = rtc_hack.data(); rtc_game.size = rtc_hack.size();
    if (!retro_load_game(&rtc_game) || retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) != 131072 + 16) {
        std::cerr << "GBA Pokemon RTC auto-detection failed\n";
        return 1;
    }
    auto* rtc_save = static_cast<std::uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    if (!rtc_save || rtc_save[131072 + 7] != 0x40) return 1;
    rtc_save[0x123] = 0x5a; rtc_save[0x10123] = 0xa5; // ancienne .sav importée par le frontend
    retro_reset(); retro_run();
    rtc_save = static_cast<std::uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    if (!rtc_save || rtc_save[0x123] != 0x5a || rtc_save[0x10123] != 0xa5 ||
        retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) != 131072 + 16) return 1;
    retro_unload_game();
    option_values["ravenemu_gba_rtc"] = "disabled";
    if (!retro_load_game(&rtc_game) || retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) != 131072) return 1;
    retro_unload_game(); option_values.clear();
    auto original_pokemon = ravenemu::testing::pokemon_rtc_gba_rom(false);
    rtc_game.data = original_pokemon.data(); rtc_game.size = original_pokemon.size();
    if (!retro_load_game(&rtc_game) || retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) != 131072) return 1;
    retro_unload_game();

    // Drill Dozer (V49) : LDR r0, =0x080000C4 ; MOV r1, #8 ; STRH r1, [r0, #2] ;
    // STRH r1, [r0] ; B . — la broche 3 du GPIO allume le moteur.
    auto rumble_rom = ravenemu::testing::minimal_gba_rom();
    const std::uint32_t motor_on[]{0xe59f000cU, 0xe3a01008U, 0xe1c010b2U, 0xe1c010b0U, 0xeafffffeU, 0x080000c4U};
    for (std::size_t index = 0; index < std::size(motor_on); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            rumble_rom[index * 4U + byte] = static_cast<std::uint8_t>(motor_on[index] >> (byte * 8U));
    std::copy_n("V49E", 4, rumble_rom.begin() + 0xac);
#else
    if (!declared_options.empty()) {
        std::cerr << "Game Boy core declares GBA options\n";
        return 1;
    }
    // MBC5+RUMBLE program: LD A,$08 ; LD ($4000),A ; JR $ (motor on, then loop).
    auto rumble_rom = ravenemu::testing::minimal_game_boy_rom();
    const std::uint8_t motor_on[] = {0x3e, 0x08, 0xea, 0x00, 0x40, 0x18, 0xfe};
    std::copy(std::begin(motor_on), std::end(motor_on), rumble_rom.begin() + 0x100);
    rumble_rom[0x147] = 0x1c;
#endif
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
#if !defined(RAVENEMU_LIBRETRO_GBA)
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
    if (!mapped(0xa000) || *mapped(0xa000) != 0x77 || !mapped(0xbfff) || mapped(0x16000)) {
        std::cerr << "Cartridge RAM not in the achievement memory map\n";
        return 1;
    }
    retro_unload_game();
    // 32 Kio de RAM : les banques 1 à 3 suivent en 0x16000, en blocs valides.
    battery_rom[0x149] = 0x03;
    if (!retro_load_game(&game)) return 1;
    if (!memory_map_valid() || !mapped(0x16000) || !mapped(0x1bfff) || mapped(0x1c000)) {
        std::cerr << "Cartridge RAM banks not in the achievement memory map\n";
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
    // Un compteur en mémoire de travail : la copie suit chaque image, garde son
    // adresse et repart de l'état rechargé ou du reset.
#if defined(RAVENEMU_LIBRETRO_GBA)
    auto counter_rom = ravenemu::testing::minimal_gba_rom();
    const std::uint32_t count_loop[]{0xe3a00403U, 0xe5d01000U, 0xe2811001U, 0xe5c01000U, 0xeafffffbU};
    for (std::size_t index = 0; index < std::size(count_loop); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            counter_rom[index * 4U + byte] = static_cast<std::uint8_t>(count_loop[index] >> (byte * 8U));
    constexpr std::size_t counter_address = 0x0300'0000;  // IWRAM
#else
    // LD HL,$C000 ; loop: INC (HL) ; JR loop
    auto counter_rom = ravenemu::testing::minimal_game_boy_rom();
    const std::uint8_t count_loop[] = {0x21, 0x00, 0xc0, 0x34, 0x18, 0xfd};
    std::copy(std::begin(count_loop), std::end(count_loop), counter_rom.begin() + 0x100);
    constexpr std::size_t counter_address = 0xc000;  // WRAM
#endif
    game.data = counter_rom.data();
    game.size = counter_rom.size();
    if (!retro_load_game(&game)) return 1;
    const auto* counter = mapped(counter_address);
    retro_run();
    const auto first = counter ? *counter : 0;
    std::vector<std::uint8_t> counter_state(retro_serialize_size());
    if (!counter || !retro_serialize(counter_state.data(), counter_state.size())) return 1;
    retro_run();
    const auto second = *counter;
    if (!retro_unserialize(counter_state.data(), counter_state.size()) || *counter != first ||
        mapped(counter_address) != counter) {
        std::cerr << "Achievement memory not refreshed after a state load\n";
        return 1;
    }
    retro_run();
    if (first == second || *counter != second) {
        std::cerr << "Achievement memory does not follow the frames\n";
        return 1;
    }
    retro_reset();
    if (*counter != 0) {
        std::cerr << "Achievement memory not refreshed after a reset\n";
        return 1;
    }
    retro_unload_game();

    retro_deinit();
    std::cout << system.library_name << " Libretro smoke test passed\n";
}
