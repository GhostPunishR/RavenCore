#include <libretro.h>
#include <synthetic_roms.hpp>
#include <algorithm>
#include <array>
#include <iterator>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if defined(RAVENEMU_LIBRETRO_GB)
#define OPTION(name) "ravenemu_gb_" name
#elif defined(RAVENEMU_LIBRETRO_GBC)
#define OPTION(name) "ravenemu_gbc_" name
#else
#define OPTION(name) "ravenemu_gba_" name
#endif

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
bool options_updated = false;                         // réponse à GET_VARIABLE_UPDATE
unsigned options_version = 0;                         // 0 : options v2 inconnues du frontend
struct DeclaredV2 { std::string desc, category, default_value; std::vector<std::string> values; };
std::map<std::string, DeclaredV2> declared_v2;
int av_flags = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
unsigned duped_frames = 0;
unsigned audio_batches = 0;
std::uint32_t first_pixel = 0;
std::string last_message;
std::string last_log;
unsigned error_logs = 0;
std::array<bool, 16> held_buttons{};  // boutons de la manette tenus, par identifiant Libretro
std::string system_directory;         // vide : le frontend n'en donne pas
// Capteurs de l'appareil simulé : ceux qu'il a, ceux que le cœur a allumés,
// et leurs mesures, par identifiant RETRO_SENSOR_*.
bool sensor_interface_offered = false;
std::array<bool, 3> device_has_sensor{};   // accéléromètre, gyroscope, lumière
std::array<bool, 3> device_sensor_on{};
std::array<float, 7> device_readings{};
unsigned sensor_reads = 0;
// Sous-systèmes déclarés : identifiant, nombre de ROMs, types de mémoire.
struct DeclaredSubsystem { std::string ident; unsigned id; std::vector<unsigned> memory_types; };
std::vector<DeclaredSubsystem> subsystems;
unsigned joypad_ports = 0;  // ports de manette lus, un bit chacun

void log_printf(retro_log_level level, const char* format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    last_log = line;
    if (level == RETRO_LOG_ERROR) ++error_logs;
}

bool rumble(unsigned port, retro_rumble_effect effect, std::uint16_t strength) {
    if (port == 0 && effect == RETRO_RUMBLE_STRONG) {
        ++rumble_calls;
        rumble_strength = strength;
    }
    return true;
}

bool set_sensor_state(unsigned port, retro_sensor_action action, unsigned) {
    const auto sensor = static_cast<std::size_t>(action) / 2;
    if (port != 0 || sensor >= device_has_sensor.size()) return false;
    const bool enable = static_cast<unsigned>(action) % 2 == 0;
    if (enable && !device_has_sensor[sensor]) return false;
    device_sensor_on[sensor] = enable;
    return true;
}
float sensor_input(unsigned port, unsigned id) {
    ++sensor_reads;
    return port == 0 && id < device_readings.size() ? device_readings[id] : 0.0f;
}

bool environment(unsigned command, void* value) {
    if (command == RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO) {
        subsystems.clear();
        for (auto* info = static_cast<const retro_subsystem_info*>(value); info->ident; ++info) {
            DeclaredSubsystem declared{info->ident, info->id, {}};
            for (unsigned rom = 0; rom < info->num_roms; ++rom)
                for (unsigned memory = 0; memory < info->roms[rom].num_memory; ++memory)
                    declared.memory_types.push_back(info->roms[rom].memory[memory].type);
            subsystems.push_back(declared);
        }
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE) {
        if (!sensor_interface_offered) return false;
        *static_cast<retro_sensor_interface*>(value) = {set_sensor_state, sensor_input};
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        if (system_directory.empty()) return false;
        *static_cast<const char**>(value) = system_directory.c_str();
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        static_cast<retro_log_callback*>(value)->log = log_printf;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_MESSAGE) {
        last_message = static_cast<const retro_message*>(value)->msg;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_CAN_DUPE) {
        *static_cast<bool*>(value) = true;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE) {
        *static_cast<int*>(value) = av_flags;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE) {
        *static_cast<bool*>(value) = options_updated;
        options_updated = false;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION) {
        if (options_version == 0) return false;
        *static_cast<unsigned*>(value) = options_version;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2) {
        const auto* options = static_cast<const retro_core_options_v2*>(value);
        for (auto* d = options->definitions; d->key; ++d) {
            DeclaredV2 declared{d->desc ? d->desc : "", d->category_key ? d->category_key : "",
                                d->default_value ? d->default_value : "", {}};
            for (auto* v = d->values; v->value; ++v) declared.values.emplace_back(v->value);
            declared_v2[d->key] = declared;
        }
        return true;
    }
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
    if (!data) {
        ++duped_frames;
        return;
    }
    if (pitch == width * sizeof(std::uint32_t)) {
        first_pixel = *static_cast<const std::uint32_t*>(data) & 0x00ffffffU;
        ++video_frames;
        last_width = width;
        last_height = height;
    }
}
std::size_t audio(const std::int16_t*, std::size_t count) {
    ++audio_batches;
    return count;
}
// Charge `rom`, fait tourner `frames` images et rend le premier pixel affiché.
std::uint32_t pixel_after(std::vector<std::uint8_t>& rom, unsigned frames) {
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    if (!retro_load_game(&game)) return 0xffffffffU;
    for (unsigned frame = 0; frame < frames; ++frame) retro_run();
    return first_pixel;
}
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
std::int16_t input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (device == RETRO_DEVICE_ANALOG) ++analog_queries;
    if (device == RETRO_DEVICE_JOYPAD && port < 8) joypad_ports |= 1U << port;
    if (device == RETRO_DEVICE_JOYPAD && id < held_buttons.size() && held_buttons[id]) return 1;
    return 0;
}
// Mélange d'un canal sur deux images, comme le cœur : (a × wa + b × wb) / 8, arrondi.
std::uint32_t blend(std::uint32_t a, std::uint32_t b, unsigned wa, unsigned wb) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
        result |= (((a >> shift) & 0xffU) * wa + ((b >> shift) & 0xffU) * wb + 4) / 8 << shift;
    return result;
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
    if (video_frames != 2 || duped_frames != 0 || last_width != expected_width || last_height != expected_height) {
        std::cerr << "Libretro video callback missing\n";
        return 1;
    }
    // Image et son dont le frontend ne veut pas (anticipation, avance rapide) :
    // l'image précédente est reprise, le son est jeté.
    const auto batches_before = audio_batches;
    av_flags = 0;
    retro_run();
    av_flags = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    if (duped_frames != 1 || video_frames != 2 || audio_batches != batches_before) {
        std::cerr << "Skipped image or sound still delivered\n";
        return 1;
    }
    retro_run();
    if (video_frames != 3 || audio_batches == batches_before) {
        std::cerr << "Image or sound not delivered again\n";
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
    // Les deux fenêtres de ROM, VRAM, WRAM, OAM (0x80 puis 0x20 octets),
    // registres d'E/S, HRAM, et sur GBC les banques 2 à 7 de la WRAM (0x4000
    // puis 0x2000 octets).
#if defined(RAVENEMU_LIBRETRO_GBC)
    constexpr std::size_t expected_regions = 10;
#else
    constexpr std::size_t expected_regions = 8;
#endif
    if (memory_map.size() != expected_regions || !mapped(0x0134) || *mapped(0x0134) != rom[0x134] ||
        !mapped(0x7fff) || !mapped(0xc000) || mapped(0xa000) || !mapped(0xff00) || !mapped(0xff7f) ||
        !mapped(0xfe9f) || mapped(0xfea0) || mapped(0xfeff)) {
        std::cerr << "Game Boy achievement memory map missing or wrong\n";
        return 1;
    }
#endif

    // Codes de triche du menu : appliqués dès l'image suivante ; un code refusé
    // dit pourquoi, et n'empêche pas les autres.
#if defined(RAVENEMU_LIBRETRO_GBA)
    const char* cheat = "33000100 0042";          // Codebreaker : [03000100] = 42
    const char* refused = "9A123456 7890";        // Codebreaker chiffré
    constexpr std::size_t cheat_address = 0x0300'0100;
    constexpr std::uint8_t cheat_value = 0x42;
#else
    const char* cheat = "3EA-12B-FAE+01AA10C0";   // Game Genie, puis GameShark : [C010] = AA
    const char* refused = "01021234";             // GameShark en ROM
    constexpr std::size_t cheat_address = 0xc010;
    constexpr std::uint8_t cheat_value = 0xaa;
#endif
    last_message.clear();
    retro_cheat_set(0, true, refused);
    retro_cheat_set(1, true, cheat);
    if (last_message.find("code de triche 1 refusé") == std::string::npos) {
        std::cerr << "Refused cheat not reported: " << last_message << "\n";
        return 1;
    }
    retro_run();
    if (!mapped(cheat_address) || *mapped(cheat_address) != cheat_value) {
        std::cerr << "Cheat not applied\n";
        return 1;
    }
    retro_cheat_set(1, false, cheat);
    retro_cheat_reset();
    retro_cheat_set(5000, true, cheat);
    retro_run();

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
    if (declared_options.size() != 8 ||
        declared_options["ravenemu_gba_device_sensors"].find("; enabled|disabled") == std::string::npos ||
        declared_options["ravenemu_gba_solar_light"].find("|sensor") == std::string::npos ||
        declared_options["ravenemu_gba_color_correction"].find("; disabled|enabled") == std::string::npos ||
        declared_options["ravenemu_gba_bios"].find("; disabled|enabled|skip_intro") == std::string::npos ||
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
#if defined(RAVENEMU_LIBRETRO_GB)
    const char* video_option = "ravenemu_gb_palette";
    const char* video_choices = "; dmg|pocket";
#else
    const char* video_option = "ravenemu_gbc_color_correction";
    const char* video_choices = "; disabled|enabled";
#endif
#if defined(RAVENEMU_LIBRETRO_GB)
    constexpr std::size_t option_count = 5;
#else
    constexpr std::size_t option_count = 7;
#endif
    if (declared_options.size() != option_count ||
        declared_options[video_option].find(video_choices) == std::string::npos) {
        std::cerr << "Game Boy core options not declared\n";
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

    // Les fenêtres de ROM suivent la banque choisie par le jeu à chaque image.
    // MBC5, banques remplies de leur numéro : LD A,5 ; LD ($2000),A ; JR $
    auto banked_rom = ravenemu::testing::minimal_game_boy_rom();
    banked_rom.resize(16U * 0x4000U);
    for (std::size_t bank = 1; bank < 16; ++bank)
        std::fill_n(banked_rom.begin() + static_cast<std::ptrdiff_t>(bank * 0x4000U), 0x4000,
                    static_cast<std::uint8_t>(bank));
    banked_rom[0x147] = 0x19;
    banked_rom[0x148] = 0x03;
    const std::uint8_t select_bank[] = {0x3e, 0x05, 0xea, 0x00, 0x20, 0x18, 0xfe};
    std::copy(std::begin(select_bank), std::end(select_bank), banked_rom.begin() + 0x100);
    game.data = banked_rom.data();
    game.size = banked_rom.size();
    if (!retro_load_game(&game) || !mapped(0x4000) || *mapped(0x4000) != 1) return 1;
    retro_run();
    if (*mapped(0x4000) != 5 || *mapped(0x7fff) != 5) {
        std::cerr << "Switchable ROM bank not followed in the achievement memory map\n";
        return 1;
    }
    retro_unload_game();
    // MBC1 de 1 Mio en mode 1 : la première fenêtre passe à la banque $20.
    // LD A,1 ; LD ($6000),A ; LD ($4000),A ; JR $, aussi présent en banque $20.
    auto mbc1_rom = ravenemu::testing::minimal_game_boy_rom();
    mbc1_rom.resize(64U * 0x4000U);
    for (std::size_t bank = 1; bank < 64; ++bank)
        std::fill_n(mbc1_rom.begin() + static_cast<std::ptrdiff_t>(bank * 0x4000U), 0x4000,
                    static_cast<std::uint8_t>(bank));
    mbc1_rom[0x147] = 0x01;
    mbc1_rom[0x148] = 0x05;
    const std::uint8_t mode_1[] = {0x3e, 0x01, 0xea, 0x00, 0x60, 0xea, 0x00, 0x40, 0x18, 0xfe};
    std::copy(std::begin(mode_1), std::end(mode_1), mbc1_rom.begin() + 0x100);
    std::copy(std::begin(mode_1), std::end(mode_1), mbc1_rom.begin() + 0x20 * 0x4000 + 0x100);
    game.data = mbc1_rom.data();
    game.size = mbc1_rom.size();
    if (!retro_load_game(&game) || !mapped(0x3000) || *mapped(0x3000) != 0) return 1;
    retro_run();
    if (*mapped(0x3000) != 0x20) {
        std::cerr << "First ROM window not followed when MBC1 switches it\n";
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

    // Accéléromètre de l'appareil : côté droit 0,5 g plus bas, haut 0,25 g plus
    // haut. Le jeu active les registres, lance une mesure et range X puis Y en
    // C000 : X = 81D0h - 38h, Y = 81D0h + 1Ch.
    const std::uint8_t measure[]{0x3e, 0x0a, 0xea, 0x00, 0x00, 0x3e, 0x40, 0xea, 0x00, 0x40,  // registres
                                 0x3e, 0x55, 0xea, 0x00, 0xa0, 0x3e, 0xaa, 0xea, 0x10, 0xa0,  // mesure
                                 0xfa, 0x20, 0xa0, 0xea, 0x00, 0xc0, 0xfa, 0x30, 0xa0, 0xea, 0x01, 0xc0,
                                 0xfa, 0x40, 0xa0, 0xea, 0x02, 0xc0, 0xfa, 0x50, 0xa0, 0xea, 0x03, 0xc0,
                                 0x18, 0xfe};
    std::copy(std::begin(measure), std::end(measure), tilt_rom.begin() + 0x100);
    sensor_interface_offered = true;
    device_has_sensor = {true, true, true};
    const auto latched = [&](float x, float y, float z) {
        device_readings = {x, y, z, 0.0f, 0.0f, 0.0f, 0.0f};
        static_cast<void>(pixel_after(tilt_rom, 1));
        const auto* bytes = mapped(0xc000);
        return bytes ? std::array<int, 2>{bytes[0] | bytes[1] << 8, bytes[2] | bytes[3] << 8}
                     : std::array<int, 2>{-1, -1};
    };
    const bool tilted = latched(-0.5f, 0.25f, 0.8f) == std::array<int, 2>{0x8198, 0x81ec} &&
                        device_sensor_on == std::array<bool, 3>{true, false, false};
    retro_unload_game();
    // Un frontend plus ancien donne des m/s².
    const bool metric = latched(-0.5f * 9.80665f, 0.25f * 9.80665f, 0.8f * 9.80665f) ==
                        std::array<int, 2>{0x8198, 0x81ec};
    retro_unload_game();
    const bool released = device_sensor_on == std::array<bool, 3>{};
    option_values[OPTION("device_sensors")] = "disabled";
    sensor_reads = 0;
    const bool stick_only = latched(-0.5f, 0.25f, 0.8f) == std::array<int, 2>{0x81d0, 0x81d0} &&
                            sensor_reads == 0 && device_sensor_on == std::array<bool, 3>{};
    option_values.erase(OPTION("device_sensors"));
    options_updated = true;
    retro_run();
    const bool switched_on = device_sensor_on[0];
    retro_unload_game();
    sensor_interface_offered = false;
    if (!tilted || !metric || !released || !stick_only || !switched_on) {
        std::cerr << "MBC7 tilt from the device's accelerometer not handled\n";
        return 1;
    }
#else
    // Capteurs de l'appareil sur GBA. Yoshi Topsy-Turvy (KYG) : le jeu lance
    // une mesure et range les quatre registres du capteur en 03000000. Côté
    // droit 0,5 g plus bas, haut 0,25 g plus haut : X = 392h + 70h, Y = 3A0h - 38h.
    {
        const auto put = [](std::vector<std::uint8_t>& image, std::size_t offset,
                            std::initializer_list<std::uint32_t> words) {
            for (const auto word : words)
                for (unsigned byte = 0; byte < 4; ++byte)
                    image[offset++] = static_cast<std::uint8_t>(word >> (byte * 8U));
        };
        auto yoshi = ravenemu::testing::minimal_gba_rom();
        put(yoshi, 0, {0xe3a0040eU, 0xe2800902U,                   // r0 = 0E008000
                       0xe3a01055U, 0xe5c01000U, 0xe3a010aaU, 0xe5c01100U,  // 55h, AAh : mesure
                       0xe3a02403U,                                // r2 = 03000000
                       0xe5d01200U, 0xe5c21000U, 0xe5d01300U, 0xe5c21001U,
                       0xe5d01400U, 0xe5c21002U, 0xe5d01500U, 0xe5c21003U, 0xeafffffeU});
        std::copy_n("KYGE", 4, yoshi.begin() + 0xac);
        sensor_interface_offered = true;
        device_has_sensor = {true, true, false};
        device_readings = {-0.5f, 0.25f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f};
        static_cast<void>(pixel_after(yoshi, 1));
        const auto* bytes = mapped(0x0300'0000);
        const bool tilted = bytes && (bytes[0] | (bytes[1] & 0x0f) << 8) == 0x402 &&
                            (bytes[2] | bytes[3] << 8) == 0x368 &&
                            device_sensor_on == std::array<bool, 3>{true, false, false};
        retro_unload_game();
        // WarioWare: Twisted! allume le gyroscope. Le jeu lance une conversion
        // et range les 16 bits lus (GBATEK) en 03000000 : un quart de tour par
        // seconde dans le sens horaire donne 6C0h + 191h.
        auto wario = ravenemu::testing::minimal_gba_rom();
        put(wario, 0, {0xe3a00302U, 0xe28000c4U,                   // r0 = 080000C4
                       0xe3a01001U, 0xe1c010b4U, 0xe3a0100bU, 0xe1c010b2U,  // lisible ; broches 0, 1, 3 en sortie
                       0xe3a01003U, 0xe1c010b0U, 0xe3a01002U, 0xe1c010b0U,  // conversion
                       0xe3a03000U, 0xe3a04010U,                   // 16 bits
                       0xe1d010b0U, 0xe1a01121U, 0xe2011001U, 0xe1813083U,  // bit de la broche 2
                       0xe3a01000U, 0xe1c010b0U, 0xe3a01002U, 0xe1c010b0U,  // horloge basse, haute
                       0xe2544001U, 0x1afffff5U,
                       0xe3a02403U, 0xe5823000U, 0xeafffffeU});
        std::copy_n("RZWE", 4, wario.begin() + 0xac);
        device_readings = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -3.14159265f / 2, 0.0f};
        static_cast<void>(pixel_after(wario, 1));
        const auto* sample = mapped(0x0300'0000);
        const bool gyroscope = sample && (sample[0] | sample[1] << 8) == 0x6c0 + 0x191 &&
                               device_sensor_on == std::array<bool, 3>{false, true, false};
        retro_unload_game();
        // Boktai allume la lumière si l'option le demande, et sinon le dit.
        auto boktai = ravenemu::testing::minimal_gba_rom();
        std::copy_n("U3IE", 4, boktai.begin() + 0xac);
        static_cast<void>(pixel_after(boktai, 1));
        const bool light_off = device_sensor_on == std::array<bool, 3>{};
        retro_unload_game();
        option_values["ravenemu_gba_solar_light"] = "sensor";
        last_message.clear();
        static_cast<void>(pixel_after(boktai, 1));
        const bool no_light = device_sensor_on == std::array<bool, 3>{} &&
                              last_message.find("capteur de lumière") != std::string::npos;
        retro_unload_game();
        device_has_sensor = {true, true, true};
        sensor_reads = 0;
        static_cast<void>(pixel_after(boktai, 2));
        const bool light_on = device_sensor_on == std::array<bool, 3>{false, false, true} && sensor_reads == 2;
        retro_unload_game();
        option_values.erase("ravenemu_gba_solar_light");
        sensor_interface_offered = false;
        if (!tilted || !gyroscope || !light_off || !no_light || !light_on ||
            device_sensor_on != std::array<bool, 3>{}) {
            std::cerr << "GBA cartridge sensors from the device not handled\n";
            return 1;
        }
    }
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

    // Un jeu refusé : la raison va au journal et à l'écran.
    std::vector<std::uint8_t> broken(16);
    retro_game_info broken_game{};
    broken_game.data = broken.data();
    broken_game.size = broken.size();
    const auto errors_before = error_logs;
    if (retro_load_game(&broken_game) || error_logs != errors_before + 1 ||
#if defined(RAVENEMU_LIBRETRO_GBA)
        last_message.find("RavenGBA") != 0 || last_message.find("trop courte") == std::string::npos
#else
        last_message.find("RavenGB") != 0 || last_message.find("trop petite") == std::string::npos
#endif
    ) {
        std::cerr << "Refused game not reported: " << last_message << "\n";
        return 1;
    }

    // Options d'image, changées en cours de jeu.
#if defined(RAVENEMU_LIBRETRO_GB)
    auto plain = ravenemu::testing::minimal_game_boy_rom();
    const auto green = pixel_after(plain, 3);
    constexpr std::uint32_t greens[]{0xe0f8d0, 0x88c070, 0x346856, 0x081820};
    constexpr std::uint32_t greys[]{0xececec, 0xa6a6a6, 0x565656, 0x141414};
    const auto shade = std::find(std::begin(greens), std::end(greens), green) - std::begin(greens);
    option_values["ravenemu_gb_palette"] = "pocket";
    options_updated = true;
    retro_run();
    if (shade == 4 || first_pixel != greys[shade]) {
        std::cerr << "Game Boy palette not applied\n";
        return 1;
    }
    retro_unload_game();
#else
    // Fond rouge pur ; corrigé : environ 28 % de gris en plus, puis gamma 1,15.
#if defined(RAVENEMU_LIBRETRO_GBA)
    const char* correction = "ravenemu_gba_color_correction";
    auto red = ravenemu::testing::minimal_gba_rom();
    const std::uint32_t backdrop[]{0xe3a00405U, 0xe3a0101fU, 0xe1c010b0U, 0xe3a00301U,
                                   0xe3a01000U, 0xe1c010b0U, 0xeafffffeU};
    const std::uint32_t entry = 0xea00002eU;  // B 0xC0, après l'en-tête
    std::memcpy(red.data(), &entry, sizeof entry);
    std::memcpy(red.data() + 0xc0, backdrop, sizeof backdrop);
#else
    const char* correction = "ravenemu_gbc_color_correction";
    // LCD éteint, couleur 0 de la palette de fond 0 en rouge pur, LCD rallumé.
    auto red = ravenemu::testing::minimal_game_boy_rom();
    red[0x143] = 0x80;
    const std::uint8_t backdrop[] = {0x3e, 0x00, 0xe0, 0x40, 0x3e, 0x80, 0xe0, 0x68, 0x3e, 0x1f, 0xe0, 0x69,
                                     0xaf, 0xe0, 0x69, 0x3e, 0x91, 0xe0, 0x40, 0x18, 0xfe};
    std::copy(std::begin(backdrop), std::end(backdrop), red.begin() + 0x100);
#endif
    if (pixel_after(red, 3) != 0xff0000) {
        std::cerr << "Raw red changed without correction: " << std::hex << first_pixel << "\n";
        return 1;
    }
    option_values[correction] = "enabled";
    options_updated = true;
    retro_run();
    if (first_pixel != 0xbf0909) {
        std::cerr << "LCD correction not applied: " << std::hex << first_pixel << "\n";
        return 1;
    }
    retro_unload_game();
#endif
    option_values.clear();

    // Un jeu qui change de fond à chaque image (scintillement) et range la
    // croix en mémoire ; sur Game Boy, il range aussi le registre B du démarrage.
#if defined(RAVENEMU_LIBRETRO_GBA)
    auto flicker = ravenemu::testing::minimal_gba_rom();
    // DISPCNT = 0, puis à chaque VBlank : palette[0] ^= 7FFF, KEYINPUT -> [03000000].
    const std::uint32_t flicker_code[]{
        0xe3a00404U, 0xe3a01405U, 0xe3a02000U, 0xe1c020b0U, 0xe3a04000U, 0xe3a05c7fU, 0xe38550ffU,
        0xe2807c01U, 0xe3a08403U, 0xe1d030b6U, 0xe35300a0U, 0x1afffffcU, 0xe0244005U, 0xe1c140b0U,
        0xe1d763b0U, 0xe1c860b0U, 0xe1d030b6U, 0xe35300a0U, 0x0afffffcU, 0xeafffff4U};
    std::memcpy(flicker.data(), flicker_code, sizeof flicker_code);
    constexpr std::size_t directions_address = 0x0300'0000;
    constexpr std::uint8_t left_right = 0x30;  // KEYINPUT : droite, gauche (0 = tenue)
#else
    auto flicker = ravenemu::testing::minimal_game_boy_rom();
    flicker[0x100] = 0xc3;  // JP $0150
    flicker[0x101] = 0x50;
    flicker[0x102] = 0x01;
    // LD A,B ; LD ($C001),A, puis à chaque VBlank : BGP ^= 3, croix -> ($C000).
    const std::uint8_t flicker_code[]{
        0x78, 0xea, 0x01, 0xc0, 0xf0, 0x44, 0xfe, 0x90, 0x20, 0xfa, 0xf0, 0x47, 0xee, 0x03, 0xe0, 0x47,
        0x3e, 0x20, 0xe0, 0x00, 0xf0, 0x00, 0xf0, 0x00, 0xea, 0x00, 0xc0, 0xf0, 0x44, 0xfe, 0x90, 0x28,
        0xfa, 0x18, 0xe1};
    std::copy(std::begin(flicker_code), std::end(flicker_code), flicker.begin() + 0x150);
    constexpr std::size_t directions_address = 0xc000;
    constexpr std::uint8_t left_right = 0x03;  // P1 : droite, gauche (0 = tenue)
#endif
    // Gauche et droite ensemble : bloquées par défaut, transmises si autorisées.
    held_buttons[RETRO_DEVICE_ID_JOYPAD_LEFT] = held_buttons[RETRO_DEVICE_ID_JOYPAD_RIGHT] = true;
    static_cast<void>(pixel_after(flicker, 4));
    if (!mapped(directions_address) || (*mapped(directions_address) & left_right) != left_right) {
        std::cerr << "Opposing directions reached the game\n";
        return 1;
    }
    option_values[OPTION("opposing_directions")] = "allowed";
    options_updated = true;
    retro_run();
    retro_run();
    if ((*mapped(directions_address) & left_right) != 0) {
        std::cerr << "Allowed opposing directions not delivered\n";
        return 1;
    }
    held_buttons = {};
    // Mélange : la moyenne des deux dernières images ; rémanence : 5/8 de la
    // nouvelle, 3/8 de l'image affichée. Chaque réglage repart de l'image en cours.
    retro_run();
    const auto image_a = first_pixel;
    retro_run();
    const auto image_b = first_pixel;
    option_values[OPTION("frame_blending")] = "mix";
    options_updated = true;
    retro_run();
    const auto mix_start = first_pixel;
    retro_run();
    const auto mixed = first_pixel;
    option_values[OPTION("frame_blending")] = "ghosting";
    options_updated = true;
    retro_run();
    const auto ghost_start = first_pixel;
    retro_run();
    if (image_a == image_b || mix_start != image_a || mixed != blend(image_b, image_a, 4, 4) || ghost_start != image_a ||
        first_pixel != blend(image_b, image_a, 5, 3)) {
        std::cerr << "Frame blending wrong: " << std::hex << image_a << ' ' << image_b << ' ' << mix_start << ' '
                  << mixed << ' ' << ghost_start << ' ' << first_pixel << "\n";
        return 1;
    }
#if !defined(RAVENEMU_LIBRETRO_GBA)
    // B au démarrage : 0 sur Game Boy et Game Boy Color, 1 sur Game Boy Advance.
    if (!mapped(0xc001) || *mapped(0xc001) != 0) {
        std::cerr << "Unexpected B register at boot\n";
        return 1;
    }
#endif
#if defined(RAVENEMU_LIBRETRO_GBC)
    retro_unload_game();
    option_values["ravenemu_gbc_hardware"] = "gba";
    static_cast<void>(pixel_after(flicker, 2));
    if (!mapped(0xc001) || *mapped(0xc001) != 1) {
        std::cerr << "Game Boy Advance hardware not selected\n";
        return 1;
    }
    option_values.erase("ravenemu_gbc_hardware");
#endif
#if !defined(RAVENEMU_LIBRETRO_GBA)
    // Boot ROM du dossier système : la nôtre range 42 en C010, puis rend la
    // main au jeu en 0100. Désactivée par l'option, elle n'est plus lue.
    {
#if defined(RAVENEMU_LIBRETRO_GB)
        std::vector<std::uint8_t> boot(0x100);
        const char* boot_file = "dmg_boot.bin";
#else
        std::vector<std::uint8_t> boot(0x900);
        const char* boot_file = "cgb_boot.bin";
#endif
        const std::uint8_t start[]{0x3e, 0x42, 0xea, 0x10, 0xc0};    // LD A,$42 ; LD ($C010),A
        const std::uint8_t leave[]{0x3e, 0x01, 0xe0, 0x50};          // LD A,1 ; LDH ($50),A
        std::copy(std::begin(start), std::end(start), boot.begin());
        std::copy(std::begin(leave), std::end(leave), boot.begin() + 0xfc);
        const auto directory = std::filesystem::temp_directory_path() /
                               ("ravenemu_smoke_" + std::string(system.library_name));
        std::filesystem::create_directories(directory);
        std::ofstream(directory / boot_file, std::ios::binary)
            .write(reinterpret_cast<const char*>(boot.data()), static_cast<std::streamsize>(boot.size()));
        system_directory = directory.string();
        retro_unload_game();
        static_cast<void>(pixel_after(flicker, 2));
        const bool used = mapped(0xc010) && *mapped(0xc010) == 0x42;
        retro_unload_game();
        option_values[OPTION("boot_rom")] = "disabled";
        static_cast<void>(pixel_after(flicker, 2));
        const bool skipped = mapped(0xc010) && *mapped(0xc010) == 0;
        system_directory.clear();
        std::filesystem::remove_all(directory);
        if (!used || !skipped) {
            std::cerr << "Boot ROM from the system directory not handled\n";
            return 1;
        }
    }
#else
    // Vrai BIOS du dossier système : le nôtre range 5C en 03000010 à la mise
    // sous tension, et 5B en 03000011 à chaque SWI. Le jeu appelle un SWI,
    // puis boucle.
    {
        const auto put = [](std::vector<std::uint8_t>& image, std::size_t offset,
                            std::initializer_list<std::uint32_t> words) {
            for (const auto word : words)
                for (unsigned byte = 0; byte < 4; ++byte)
                    image[offset++] = static_cast<std::uint8_t>(word >> (byte * 8U));
        };
        std::vector<std::uint8_t> bios(0x4000);
        put(bios, 0x00, {0xea00003eU});                                          // B 0x100
        put(bios, 0x08, {0xea00007cU});                                          // B 0x200
        put(bios, 0x100, {0xe3a00403U, 0xe3a0105cU, 0xe5c01010U, 0xe3a0f302U});  // ... ; MOV PC, #0x08000000
        put(bios, 0x200, {0xe3a00403U, 0xe3a0105bU, 0xe5c01011U, 0xe1b0f00eU});  // ... ; MOVS PC, LR
        auto swi_rom = ravenemu::testing::minimal_gba_rom();
        put(swi_rom, 0, {0xef0d0000U, 0xeafffffeU});                              // SWI 0x0D ; B .
        const auto directory = std::filesystem::temp_directory_path() /
                               ("ravenemu_smoke_" + std::string(system.library_name));
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "gba_bios.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(bios.data()), static_cast<std::streamsize>(bios.size()));
        system_directory = directory.string();
        const auto markers = [&](const char* choice) {
            retro_unload_game();
            option_values["ravenemu_gba_bios"] = choice;
            last_message.clear();
            static_cast<void>(pixel_after(swi_rom, 2));
            const auto* power_on = mapped(0x0300'0010);
            const auto* swi = mapped(0x0300'0011);
            return power_on && swi ? *power_on << 8 | *swi : -1;
        };
        const bool intro = markers("enabled") == 0x5c5b;
        const bool skipped = markers("skip_intro") == 0x005b;
        const bool emulated = markers("disabled") == 0 && last_message.empty();
        std::filesystem::remove(directory / "gba_bios.bin");
        const bool missing = markers("enabled") == 0 && last_message.find("gba_bios.bin") != std::string::npos;
        system_directory.clear();
        std::filesystem::remove_all(directory);
        if (!intro || !skipped || !emulated || !missing) {
            std::cerr << "GBA BIOS from the system directory not handled\n";
            return 1;
        }
    }
#endif
    retro_unload_game();
    option_values.clear();

    // Deux joueurs reliés par câble (sous-système « link ») : images côte à
    // côte, une manette et une sauvegarde chacun. Le joueur 1 envoie un mot
    // par le port série et reçoit celui du joueur 2, qui a une sauvegarde.
    {
#if defined(RAVENEMU_LIBRETRO_GBA)
        // Mode multijoueur : RCNT = 0, SIOCNT = 2003h, SIOMLT_SEND, puis le
        // parent lance le transfert, l'attend et range SIOMULTI1 en 03000000.
        const auto program = [](std::initializer_list<std::uint32_t> words) {
            auto image = ravenemu::testing::minimal_gba_rom();
            std::size_t offset = 0;
            for (const auto word : words)
                for (unsigned byte = 0; byte < 4; ++byte)
                    image[offset++] = static_cast<std::uint8_t>(word >> (byte * 8U));
            return image;
        };
        auto first_rom = program({0xe3a00301U, 0xe2800c01U, 0xe3a01000U, 0xe1c013b4U, 0xe3a01a02U, 0xe3811003U,
                                  0xe1c012b8U, 0xe3a02c12U, 0xe3822034U, 0xe1c022baU,   // envoie 1234h
                                  0xe3811080U, 0xe1c012b8U, 0xe1d012b8U, 0xe3110080U, 0x1afffffcU,
                                  0xe1d012b2U, 0xe3a02403U, 0xe1c210b0U, 0xeafffffeU});
        auto second_rom = program({0xe3a00301U, 0xe2800c01U, 0xe3a01000U, 0xe1c013b4U, 0xe3a01a02U, 0xe3811003U,
                                   0xe1c012b8U, 0xe3a02c56U, 0xe3822078U, 0xe1c022baU,  // envoie 5678h
                                   0xeafffffeU});
        std::copy_n("SRAM_V", 6, second_rom.begin() + 0x100);
        constexpr std::size_t received_address = 0x0300'0000;
        constexpr int expected_word = 0x5678;
        constexpr std::size_t second_save = 32768;
#else
        // Le joueur 2 prépare 99h, horloge externe ; le joueur 1 lui en laisse
        // le temps, puis envoie 42h, horloge interne. SB du joueur 1 vaut
        // ensuite 99h.
        auto first_rom = ravenemu::testing::minimal_game_boy_rom();
        auto second_rom = ravenemu::testing::minimal_game_boy_rom();
        const std::uint8_t send[]{0x06, 0x00, 0x05, 0x20, 0xfd,  // LD B,0 ; DEC B ; JR NZ
                                  0x3e, 0x42, 0xe0, 0x01, 0x3e, 0x81, 0xe0, 0x02, 0x18, 0xfe};
        const std::uint8_t answer[]{0x3e, 0x99, 0xe0, 0x01, 0x3e, 0x80, 0xe0, 0x02, 0x18, 0xfe};
        std::copy(std::begin(send), std::end(send), first_rom.begin() + 0x100);
        std::copy(std::begin(answer), std::end(answer), second_rom.begin() + 0x100);
#if defined(RAVENEMU_LIBRETRO_GBC)
        first_rom[0x143] = second_rom[0x143] = 0x80;
#endif
        second_rom[0x147] = 0x1b;  // MBC5+RAM+BATTERY, 8 Kio
        second_rom[0x149] = 0x02;
        constexpr std::size_t received_address = 0xff01;
        constexpr int expected_word = 0x99;
        constexpr std::size_t second_save = 8192;
#endif
        const bool declared = subsystems.size() == 1 && subsystems[0].ident == "link" &&
                              subsystems[0].memory_types == std::vector<unsigned>{0x101, 0x102};
        const unsigned link_id = declared ? subsystems[0].id : 0;
        retro_game_info pair[2]{};
        pair[0].data = first_rom.data(); pair[0].size = first_rom.size();
        pair[1].data = second_rom.data(); pair[1].size = second_rom.size();
        if (!declared || !retro_load_game_special(link_id, pair, 2)) {
            std::cerr << "Link subsystem not declared or not loaded\n";
            return 1;
        }
        retro_system_av_info pair_av{};
        retro_get_system_av_info(&pair_av);
        // Sauvegarde du joueur 2 écrite par le frontend : sa console repart
        // avec, sans quitter le câble.
        if (auto* second_save_ram = static_cast<std::uint8_t*>(retro_get_memory_data(0x102)))
            second_save_ram[0] ^= 0xff;
        joypad_ports = 0;
        retro_run();
        retro_run();
        const auto* received = mapped(received_address);
        const int word = received ? (received[0] | (expected_word > 0xff ? received[1] << 8 : 0)) : -1;
        const bool side_by_side = pair_av.geometry.base_width == 2 * expected_width &&
                                  last_width == 2 * expected_width && last_height == expected_height;
        const bool saves = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0 &&
                           retro_get_memory_size(0x102) == second_save && retro_get_memory_data(0x102) &&
                           retro_get_memory_size(0x103) == 0;
        // État des deux consoles : repris tel quel, refusé s'il vient d'un jeu seul.
        std::vector<std::uint8_t> pair_state(retro_serialize_size()), again(pair_state.size());
        const bool saved = retro_serialize(pair_state.data(), pair_state.size()) && pair_state[3] == '2';
        retro_run();
        const bool restored = retro_unserialize(pair_state.data(), pair_state.size()) &&
                              retro_serialize(again.data(), again.size()) && again == pair_state;
        auto single_state = pair_state;
        single_state[3] = '1';
        const bool single_refused = !retro_unserialize(single_state.data(), single_state.size());
        retro_reset();
        retro_run();
        retro_unload_game();
        const bool refused =
            !retro_load_game_special(link_id + 1, pair, 2) && !retro_load_game_special(link_id, pair, 1);
        if (word != expected_word || !side_by_side || joypad_ports != 3 || !saves || !saved || !restored ||
            !single_refused || !refused) {
            std::cerr << "Linked players not handled: word " << word << ", width " << last_width << "\n";
            return 1;
        }
    }
    retro_unload_game();

    // Options v2 : libellés français, catégories et valeur par défaut.
    options_version = 2;
    declared_options.clear();
    retro_set_environment(environment);
#if defined(RAVENEMU_LIBRETRO_GBA)
    const auto& light = declared_v2["ravenemu_gba_solar_light"];
    const auto& bios_option = declared_v2["ravenemu_gba_bios"];
    if (declared_v2.size() != 8 || !declared_options.empty() || light.default_value != "50" ||
        light.category != "cartridge" || light.values.size() != 12 || bios_option.category != "system" ||
        bios_option.default_value != "disabled" || bios_option.values.size() != 3 ||
        declared_v2["ravenemu_gba_color_correction"].desc != "Correction des couleurs LCD") {
        std::cerr << "Options v2 not declared\n";
        return 1;
    }
#elif defined(RAVENEMU_LIBRETRO_GB)
    const auto& palette = declared_v2["ravenemu_gb_palette"];
    if (declared_v2.size() != 5 || !declared_options.empty() || palette.default_value != "dmg" ||
        palette.category != "video" || palette.values.size() != 3 || palette.desc != "Palette") {
        std::cerr << "Options v2 not declared\n";
        return 1;
    }
#else
    const auto& lcd = declared_v2["ravenemu_gbc_color_correction"];
    const auto& dmg_colors = declared_v2["ravenemu_gbc_dmg_colors"];
    if (declared_v2.size() != 7 || !declared_options.empty() || lcd.default_value != "disabled" ||
        lcd.category != "video" || lcd.values.size() != 2 || dmg_colors.default_value != "auto" ||
        dmg_colors.category != "video" || dmg_colors.values.size() != 14) {
        std::cerr << "Options v2 not declared\n";
        return 1;
    }
#endif
    const auto& blending = declared_v2[OPTION("frame_blending")];
    const auto& directions = declared_v2[OPTION("opposing_directions")];
    const auto& device = declared_v2[OPTION("device_sensors")];
    if (blending.category != "video" || blending.default_value != "disabled" || blending.values.size() != 3 ||
        directions.category != "input" || directions.default_value != "blocked" || device.category != "input" ||
        device.default_value != "enabled" || device.values.size() != 2) {
        std::cerr << "Blending or input options v2 not declared\n";
        return 1;
    }
#if !defined(RAVENEMU_LIBRETRO_GBA)
    if (declared_v2[OPTION("boot_rom")].category != "system" || declared_v2[OPTION("boot_rom")].default_value != "enabled") {
        std::cerr << "Boot ROM option v2 not declared\n";
        return 1;
    }
#endif
    retro_deinit();
    std::cout << system.library_name << " Libretro smoke test passed\n";
}
