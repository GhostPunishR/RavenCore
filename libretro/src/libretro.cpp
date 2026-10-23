#include <libretro.h>
#include <ravenemu/core.hpp>
#if defined(RAVENEMU_LIBRETRO_GB)
#include <ravenemu/gb/core.hpp>
#elif defined(RAVENEMU_LIBRETRO_GBC)
#include <ravenemu/gbc/core.hpp>
#elif defined(RAVENEMU_LIBRETRO_GBA)
#else
#error "Choose exactly one Libretro core"
#endif
#ifndef RAVENEMU_VERSION
#error "RAVENEMU_VERSION comes from the CMake project version"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
retro_environment_t env_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_t sample_cb = nullptr;
retro_audio_sample_batch_t batch_cb = nullptr;
retro_input_poll_t poll_cb = nullptr;
retro_input_state_t input_cb = nullptr;
retro_log_printf_t log_cb = nullptr;
// The frontend accepts a null frame: the previous image is shown again.
bool can_dupe = false;
bool run_error_logged = false;

std::unique_ptr<ravenemu::Core> core;
std::vector<std::uint8_t> rom;
std::vector<std::uint8_t> save_ram, initial_save_ram;
std::vector<std::int32_t> native_frame;
std::vector<std::uint32_t> output_frame;
std::size_t state_capacity = 0;
bool pending_save_import = false;
retro_set_rumble_state_t rumble_cb = nullptr;
bool rumble_on = false;
// Sensors of the loaded cartridge, driven by the left analog stick.
ravenemu::CartridgeSensors sensors{};

// RetroAchievements reads emulated memory through pointers that must stay valid
// for the whole session, but a reset or a state load replaces RavenCore's
// machine. The frontend therefore reads a copy of the RetroAchievements address
// space, laid out at the same offsets and refreshed after every frame, when
// achievements are evaluated. Writes to the copy reach nothing.
struct AchievementRegion {
    std::uint32_t address;  // offset in the RetroAchievements address space
    std::uint32_t size;
    std::size_t start;      // console address, as the frontend maps it
    std::uint64_t flags;
    bool every_frame;
};
#if defined(RAVENEMU_LIBRETRO_GBA)
constexpr std::size_t achievement_space = 0x58000;
constexpr std::array<AchievementRegion, 3> achievement_regions{{
    {0x00000, 0x08000, 0x0300'0000, RETRO_MEMDESC_SYSTEM_RAM, true},  // IWRAM
    {0x08000, 0x40000, 0x0200'0000, RETRO_MEMDESC_SYSTEM_RAM, true},  // EWRAM
    {0x48000, 0x10000, 0x0e00'0000, RETRO_MEMDESC_SAVE_RAM, true},    // save chip, 64 KiB at most
}};
#else
// Console addresses, plus the extra banks RetroAchievements places above
// 0xFFFF. Echo RAM maps onto WRAM. Both ROM windows are copied as the
// cartridge maps them, every frame: MBC1 in its second mode and MMM01 also
// switch the first one. A plain bank is one copy of 16 KiB.
constexpr std::size_t achievement_space = 0x34000;
constexpr std::array<AchievementRegion, 10> achievement_regions{{
    {0x00000, 0x04000, 0x00000, 0, true},                          // ROM, first window
    {0x04000, 0x04000, 0x04000, 0, true},                          // ROM, switchable bank
    {0x08000, 0x02000, 0x08000, RETRO_MEMDESC_VIDEO_RAM, true},    // VRAM
    {0x0a000, 0x02000, 0x0a000, RETRO_MEMDESC_SAVE_RAM, true},     // cartridge RAM, bank 0
    {0x0c000, 0x02000, 0x0c000, RETRO_MEMDESC_SYSTEM_RAM, true},   // WRAM, banks 0 and 1
    {0x0fe00, 0x000a0, 0x0fe00, RETRO_MEMDESC_VIDEO_RAM, true},    // OAM
    {0x0ff00, 0x00080, 0x0ff00, 0, true},                          // I/O registers, as the CPU reads them
    {0x0ff80, 0x00080, 0x0ff80, RETRO_MEMDESC_SYSTEM_RAM, true},   // HRAM, then IE
    {0x10000, 0x06000, 0x10000, RETRO_MEMDESC_SYSTEM_RAM, true},   // GBC WRAM, banks 2 to 7
    {0x16000, 0x1e000, 0x16000, RETRO_MEMDESC_SAVE_RAM, true},     // cartridge RAM, banks 1 to 15
}};
#endif
std::vector<std::uint8_t> achievement_ram;
// Bytes the loaded game exposes in each region: no cartridge RAM, a DMG
// without the GBC banks, a smaller save chip...
std::array<std::size_t, achievement_regions.size()> achievement_sizes{};

#if defined(RAVENEMU_LIBRETRO_GB)
constexpr const char* label = "RavenGB";
constexpr const char* name = "RavenGB";
constexpr const char* extensions = "gb|sgb";
constexpr unsigned width = 160, height = 144;
constexpr double fps = 4'194'304.0 / 70'224.0;
std::unique_ptr<ravenemu::Core> new_core() {
    return ravenemu::gb::make_core(ravenemu::gb::HardwareModel::dmg);
}
#elif defined(RAVENEMU_LIBRETRO_GBC)
constexpr const char* label = "RavenGBC";
constexpr const char* name = "RavenGBC";
constexpr const char* extensions = "gbc|gb";
constexpr unsigned width = 160, height = 144;
constexpr double fps = 4'194'304.0 / 70'224.0;
std::unique_ptr<ravenemu::Core> new_core() { return ravenemu::gbc::make_core(); }
#else
constexpr const char* label = "RavenGBA (experimental)";
constexpr const char* name = "RavenGBA";
constexpr const char* extensions = "gba";
constexpr unsigned width = 240, height = 160;
constexpr double fps = 16'777'216.0 / 280'896.0;
std::unique_ptr<ravenemu::Core> new_core() { return ravenemu::make_gba_core(std::nullopt); }
#endif

// Cheats from the frontend's menu, by index. A disabled or refused cheat
// keeps its slot, empty, so that later indexes stay where the frontend put them.
std::vector<std::optional<ravenemu::Cheat>> cheats;
constexpr unsigned max_cheats = 1024;

// Teintes d'un jeu Game Boy, de la plus claire à la plus sombre : le vert de
// l'écran d'origine, ou les gris du Game Boy Pocket (luminance des mêmes
// teintes, comme la palette « Pocket » de RavenEmu Android).
using Shades = std::array<std::uint32_t, 4>;
constexpr Shades dmg_shades{0x00e0f8d0, 0x0088c070, 0x00346856, 0x00081820};
#if defined(RAVENEMU_LIBRETRO_GB)
constexpr Shades pocket_shades{0x00ececec, 0x00a6a6a6, 0x00565656, 0x00141414};
#endif
Shades shades = dmg_shades;

// Correction LCD des consoles couleur, la même que RavenEmu Android (réglée à
// l'œil, pas d'après les mesures d'un écran) : environ 28 % de désaturation
// vers la luminance Rec. 709, puis le gamma 1,15 d'un écran réfléchissant.
bool lcd_correction = false;
const std::array<std::uint8_t, 256> lcd_gamma = [] {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t value = 0; value < table.size(); ++value)
        table[value] = static_cast<std::uint8_t>(std::lround(255.0 * std::pow(static_cast<double>(value) / 255.0, 1.15)));
    return table;
}();
std::uint32_t lcd_corrected(std::uint32_t pixel) {
    const int r = static_cast<int>((pixel >> 16U) & 255U);
    const int g = static_cast<int>((pixel >> 8U) & 255U);
    const int b = static_cast<int>(pixel & 255U);
    const int luma = (54 * r + 183 * g + 19 * b) >> 8;
    // 72/256 du chemin vers la luminance : un mélange, toujours entre 0 et 255.
    const auto channel = [luma](int value) -> std::uint32_t {
        return lcd_gamma[static_cast<std::size_t>(value + (((luma - value) * 72) >> 8))];
    };
    return channel(r) << 16U | channel(g) << 8U | channel(b);
}
// MBC7 accelerometer (Kirby Tilt 'n' Tumble, Command Master) driven by the
// left analog stick. The cartridge reads 0x81D0 at rest and moves by about
// 0x70 per g; a full deflection is a 1 g tilt. Signs follow accelerometer
// physics with the sensor's X axis to the cartridge's right and Y to its top:
// tilting right (stick right) lowers X, tilting the top away (stick up)
// lowers Y. Not yet checked in game: flip the signs here if it plays mirrored.
struct Tilt { int x; int y; };
constexpr int tilt_units_per_g = 0x70;
constexpr int scale_tilt(int stick) { return stick * tilt_units_per_g / 0x7fff; }
constexpr Tilt tilt_from_stick(int stick_x, int stick_y) {
    return {-scale_tilt(stick_x), scale_tilt(stick_y)};
}
static_assert(tilt_from_stick(0, 0).x == 0 && tilt_from_stick(0, 0).y == 0);
static_assert(tilt_from_stick(0x7fff, 0).x == -tilt_units_per_g);
static_assert(tilt_from_stick(-0x8000, 0).x == tilt_units_per_g);
static_assert(tilt_from_stick(0, -0x8000).y == -tilt_units_per_g);
static_assert(tilt_from_stick(0, 0x7fff).y == tilt_units_per_g);

void log_line(retro_log_level level, const std::string& text) {
    if (log_cb) log_cb(level, "[%s] %s\n", name, text.c_str());
}
// A message on screen, for a few seconds.
void show_message(const std::string& text) {
    if (!env_cb) return;
    retro_message message{text.c_str(), 300};
    env_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}
// The frontend only says that the game did not load: the reason goes in the
// log and on screen.
void report_load_failure(const std::string& reason) {
    log_line(RETRO_LOG_ERROR, "jeu refusé : " + reason);
    show_message(std::string(name) + " : jeu refusé (" + reason + ")");
}
// The engine takes the whole list at once; it runs from the next frame.
void apply_cheats() {
    if (!core) return;
    std::vector<ravenemu::Cheat> active;
    for (const auto& cheat : cheats)
        if (cheat) active.push_back(*cheat);
    core->set_cheats(active);
}

// Core options, labelled in French and sorted in categories by a frontend
// that knows options v2; an older one gets the plain « Label; a|b|c » list.
// The first choice is the default.
struct OptionChoice { const char* value; const char* label; };
struct OptionSpec {
    const char* key;
    const char* label;
    const char* info;
    const char* category;
    std::vector<OptionChoice> choices;
};
#if !defined(RAVENEMU_LIBRETRO_GB)
OptionSpec lcd_option(const char* key) {
    return {key, "Correction des couleurs LCD",
            "Adoucit les couleurs vives pour imiter l'écran réfléchissant de la console.", "video",
            {{"disabled", "Désactivée"}, {"enabled", "Activée"}}};
}
#endif
const std::vector<OptionSpec> option_specs{
#if defined(RAVENEMU_LIBRETRO_GB)
    {"ravenemu_gb_palette", "Palette", "Teintes des jeux Game Boy.", "video",
     {{"dmg", "Vert (Game Boy)"}, {"pocket", "Gris (Game Boy Pocket)"}}},
#elif defined(RAVENEMU_LIBRETRO_GBC)
    lcd_option("ravenemu_gbc_color_correction"),
#else
    lcd_option("ravenemu_gba_color_correction"),
    // For games whose cartridge detection fails: the SAVE_RAM size must not
    // change while the frontend holds it, so both apply when a game loads.
    {"ravenemu_gba_save_type", "Type de sauvegarde",
     "Appliqué au chargement du jeu. Auto reconnaît la puce de la cartouche.", "cartridge",
     {{"auto", "Auto"}, {"sram", "SRAM 32 Kio"}, {"flash64", "Flash 64 Kio"}, {"flash128", "Flash 128 Kio"},
      {"eeprom512", "EEPROM 512 octets"}, {"eeprom8k", "EEPROM 8 Kio"}, {"none", "Aucune"}}},
    {"ravenemu_gba_rtc", "Horloge (RTC)", "Appliquée au chargement du jeu.", "cartridge",
     {{"auto", "Auto"}, {"enabled", "Activée"}, {"disabled", "Désactivée"}}},
    {"ravenemu_gba_solar_light", "Lumière du capteur solaire",
     "Boktai : part du plein soleil que reçoit le capteur de la cartouche.", "cartridge",
     {{"50", "50 %"}, {"60", "60 %"}, {"70", "70 %"}, {"80", "80 %"}, {"90", "90 %"}, {"100", "100 %"},
      {"0", "0 %"}, {"10", "10 %"}, {"20", "20 %"}, {"30", "30 %"}, {"40", "40 %"}}},
#endif
};

void declare_options() {
    unsigned version = 0;
    if (env_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        static std::array<retro_core_option_v2_category, 3> categories{{
            {"video", "Image", "Couleurs de l'image."},
            {"cartridge", "Cartouche", "Matériel de la cartouche, quand sa détection se trompe."},
            {nullptr, nullptr, nullptr},
        }};
        static std::vector<retro_core_option_v2_definition> definitions;
        definitions.assign(option_specs.size() + 1, retro_core_option_v2_definition{});
        for (std::size_t index = 0; index < option_specs.size(); ++index) {
            const auto& spec = option_specs[index];
            auto& definition = definitions[index];
            definition.key = spec.key;
            definition.desc = spec.label;
            definition.info = spec.info;
            definition.category_key = spec.category;
            for (std::size_t choice = 0; choice < spec.choices.size(); ++choice)
                definition.values[choice] = {spec.choices[choice].value, spec.choices[choice].label};
            definition.default_value = spec.choices.front().value;
        }
        retro_core_options_v2 options{categories.data(), definitions.data()};
        if (env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options)) return;
    }
    static std::vector<std::string> texts;
    static std::vector<retro_variable> variables;
    texts.clear();
    variables.clear();
    for (const auto& spec : option_specs) {
        std::string text = std::string(spec.label) + "; ";
        for (std::size_t choice = 0; choice < spec.choices.size(); ++choice)
            text += (choice ? "|" : "") + std::string(spec.choices[choice].value);
        texts.push_back(std::move(text));
    }
    for (std::size_t index = 0; index < option_specs.size(); ++index)
        variables.push_back({option_specs[index].key, texts[index].c_str()});
    variables.push_back({nullptr, nullptr});
    env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables.data());
}

const char* option(const char* key) {
    retro_variable variable{key, nullptr};
    return env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ? variable.value : nullptr;
}
bool option_is(const char* key, std::string_view value) {
    const char* current = option(key);
    return current && value == current;
}
// Image options: read when a game loads, and again whenever they change.
void read_video_options() {
#if defined(RAVENEMU_LIBRETRO_GB)
    shades = option_is("ravenemu_gb_palette", "pocket") ? pocket_shades : dmg_shades;
#elif defined(RAVENEMU_LIBRETRO_GBC)
    lcd_correction = option_is("ravenemu_gbc_color_correction", "enabled");
#else
    lcd_correction = option_is("ravenemu_gba_color_correction", "enabled");
#endif
}

#if defined(RAVENEMU_LIBRETRO_GBA)
// Boktai's solar sensor: a percentage of full sunlight, 0 to 100.
void apply_solar_option(ravenemu::Core& machine) {
    int percent = 50;
    if (const char* value = option("ravenemu_gba_solar_light")) {
        for (int choice = 0; choice <= 100; choice += 10)
            if (std::string_view(value) == std::to_string(choice)) percent = choice;
    }
    machine.set_solar_light(percent * 255 / 100);
}
#endif
void apply_options(ravenemu::Core& machine) {
    read_video_options();
#if defined(RAVENEMU_LIBRETRO_GBA)
    using ravenemu::GbaSaveType;
    struct SaveChoice { std::string_view value; GbaSaveType type; };
    constexpr std::array<SaveChoice, 6> save_choices{{
        {"sram", GbaSaveType::sram}, {"flash64", GbaSaveType::flash_64k},
        {"flash128", GbaSaveType::flash_128k}, {"eeprom512", GbaSaveType::eeprom_512},
        {"eeprom8k", GbaSaveType::eeprom_8k}, {"none", GbaSaveType::none},
    }};
    std::optional<GbaSaveType> save_type;
    if (const char* value = option("ravenemu_gba_save_type"))
        for (const auto& choice : save_choices) if (choice.value == value) save_type = choice.type;
    machine.set_gba_forced_save_type(save_type);
    std::optional<bool> rtc;
    if (option_is("ravenemu_gba_rtc", "enabled")) rtc = true;
    else if (option_is("ravenemu_gba_rtc", "disabled")) rtc = false;
    machine.set_gba_forced_rtc(rtc);
    apply_solar_option(machine);
#else
    static_cast<void>(machine);
#endif
}
// The image options and the solar light change at any time during the game.
void update_options() {
    bool updated = false;
    if (!core || !env_cb || !env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) || !updated) return;
    read_video_options();
#if defined(RAVENEMU_LIBRETRO_GBA)
    apply_solar_option(*core);
#endif
}

struct Key { unsigned id; ravenemu::Button button; };
constexpr std::array<Key, 10> keys{{
    {RETRO_DEVICE_ID_JOYPAD_UP, ravenemu::Button::up},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, ravenemu::Button::down},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, ravenemu::Button::left},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, ravenemu::Button::right},
    {RETRO_DEVICE_ID_JOYPAD_A, ravenemu::Button::a},
    {RETRO_DEVICE_ID_JOYPAD_B, ravenemu::Button::b},
    {RETRO_DEVICE_ID_JOYPAD_START, ravenemu::Button::start},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, ravenemu::Button::select},
    {RETRO_DEVICE_ID_JOYPAD_L, ravenemu::Button::l},
    {RETRO_DEVICE_ID_JOYPAD_R, ravenemu::Button::r}
}};

// SAVE_RAM is the copy the frontend persists, so mirroring the battery there
// counts as saving it: copy only when the game changed it (a full copy cost up
// to 75 us per frame with an MBC6's 1 MiB flash), then acknowledge that
// generation. The MBC3 clock footer is refreshed with each copy; between two
// copies its registers and timestamp stay consistent, which is all a reload needs.
void update_save_ram() {
    if (!core || save_ram.empty() || !core->battery_ram_dirty()) return;
    auto state = core->snapshot_battery_ram();
    if (!state || state->data.size() != save_ram.size()) return;
    std::copy(state->data.begin(), state->data.end(), save_ram.begin());
    core->acknowledge_battery_ram_saved(state->generation);
}
/* Libretro frontends restore battery files into SAVE_RAM after load_game.
 * Apply this initial write before the first emulated frame or serialization. */
void import_save_ram_once() {
    if (!core || !pending_save_import) return;
    pending_save_import = false;
    if (!save_ram.empty() && save_ram != initial_save_ram)
        core->load_rom(rom, save_ram);
    initial_save_ram.clear();
}
void update_rumble() {
    if (!core || !rumble_cb) return;
    const bool active = core->rumble_active();
    if (active == rumble_on) return;
    rumble_on = active;
    rumble_cb(0, RETRO_RUMBLE_STRONG, active ? 0xffff : 0);
}
void set_input_descriptors() {
    std::vector<retro_input_descriptor> descriptors{
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
    };
#if defined(RAVENEMU_LIBRETRO_GBA)
    descriptors.push_back({0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"});
    descriptors.push_back({0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"});
#endif
    if (sensors.tilt) {
        descriptors.push_back({0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                               RETRO_DEVICE_ID_ANALOG_X, "Tilt Left/Right"});
        descriptors.push_back({0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                               RETRO_DEVICE_ID_ANALOG_Y, "Tilt Forward/Back"});
    }
    if (sensors.rotation) {
        descriptors.push_back({0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                               RETRO_DEVICE_ID_ANALOG_X, "Rotate (gyro sensor)"});
    }
    descriptors.push_back({0, 0, 0, 0, nullptr});
    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors.data());
}
void clear_game() {
    // Never leave a gamepad motor running after the game that started it.
    if (rumble_cb && rumble_on) rumble_cb(0, RETRO_RUMBLE_STRONG, 0);
    rumble_cb = nullptr;
    rumble_on = false;
    sensors = {};
    core.reset();
    rom.clear();
    save_ram.clear();
    initial_save_ram.clear();
    native_frame.clear();
    output_frame.clear();
    state_capacity = 0;
    pending_save_import = false;
    achievement_ram.clear();
    achievement_sizes = {};
    run_error_logged = false;
}
void refresh_video() {
    if (!core || !video_cb) return;
    const auto spec = core->video_spec();
    const bool indexed = core->framebuffer_format() == ravenemu::FramebufferFormat::indexed_4;
    for (std::size_t i = 0; i < native_frame.size(); ++i) {
        const auto pixel = static_cast<std::uint32_t>(native_frame[i]);
        output_frame[i] = indexed ? shades[pixel & 3U]
                        : lcd_correction ? lcd_corrected(pixel) : (pixel & 0x00ffffffU);
    }
    video_cb(output_frame.data(), static_cast<unsigned>(spec.width),
             static_cast<unsigned>(spec.height),
             static_cast<std::size_t>(spec.width) * sizeof(std::uint32_t));
}
void refresh_achievement_memory(bool everything) {
    if (!core || achievement_ram.empty()) return;
    for (std::size_t index = 0; index < achievement_regions.size(); ++index) {
        const auto& region = achievement_regions[index];
        if ((everything || region.every_frame) && achievement_sizes[index] > 0)
            core->read_achievement_memory(region.address,
                                          std::span(achievement_ram).subspan(region.address, achievement_sizes[index]));
    }
}
// Once the game is loaded: the copy keeps its address until the game unloads.
void declare_achievement_memory() {
    achievement_ram.assign(achievement_space, 0);
    std::vector<retro_memory_descriptor> descriptors;
    for (std::size_t index = 0; index < achievement_regions.size(); ++index) {
        const auto& region = achievement_regions[index];
        achievement_sizes[index] = core->read_achievement_memory(
            region.address, std::span(achievement_ram).subspan(region.address, region.size));
        // Without a select mask, Libretro wants each descriptor's length to be
        // a power of two, and RetroArch its start aligned on that length: the
        // region is cut into such blocks (OAM, GBC banks, cartridge RAM).
        auto start = region.start;
        auto offset = static_cast<std::size_t>(region.address);
        for (auto left = achievement_sizes[index]; left > 0;) {
            auto length = std::bit_floor(left);
            if (start != 0) length = std::min(length, start & (~start + 1));
            retro_memory_descriptor descriptor{};
            descriptor.flags = region.flags;
            descriptor.ptr = achievement_ram.data();
            descriptor.offset = offset;
            descriptor.start = start;
            descriptor.len = length;
            descriptors.push_back(descriptor);
            start += length;
            offset += length;
            left -= length;
        }
    }
    retro_memory_map map{descriptors.data(), static_cast<unsigned>(descriptors.size())};
    env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
}
// A frontend that does not want the sound of this frame still gets the
// samples drained, or they would pile up for the next one.
void refresh_audio(bool deliver) {
    if (!core) return;
    std::array<std::int16_t, 4096> pcm{};
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto count = core->read_audio(pcm);
        if (count < 2) break;
        const auto frames = count / 2;
        if (deliver && batch_cb) batch_cb(pcm.data(), frames);
        else if (deliver && sample_cb) for (std::size_t i = 0; i < frames; ++i)
            sample_cb(pcm[2 * i], pcm[2 * i + 1]);
        if (count < pcm.size()) break;
    }
}
// What the frontend will use of this frame: it skips the image or the sound
// of the frames it runs ahead or fast-forwards. Without an answer, both.
int wanted_av() {
    int flags = 0;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &flags)) return flags;
    return RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
}
} // namespace

extern "C" {

void retro_set_environment(retro_environment_t callback) {
    env_cb = callback;
    // The memory map declared with each game exposes the RetroAchievements
    // address space (see declare_achievement_memory).
    bool achievements = true;
    if (!env_cb) return;
    retro_log_callback logging{};
    log_cb = env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) ? logging.log : nullptr;
    env_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
    declare_options();
}
void retro_set_video_refresh(retro_video_refresh_t callback) { video_cb = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { sample_cb = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { batch_cb = callback; }
void retro_set_input_poll(retro_input_poll_t callback) { poll_cb = callback; }
void retro_set_input_state(retro_input_state_t callback) { input_cb = callback; }
void retro_set_controller_port_device(unsigned, unsigned) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_init(void) {}
void retro_deinit(void) { clear_game(); }

void retro_get_system_info(struct retro_system_info* info) {
    if (!info) return;
    info->library_name = label;
    info->library_version = RAVENEMU_VERSION;
    info->valid_extensions = extensions;
    info->need_fullpath = false;
    info->block_extract = false;
}
void retro_get_system_av_info(struct retro_system_av_info* info) {
    if (!info) return;
    const auto video = core ? core->video_spec()
                            : ravenemu::VideoSpec{static_cast<int>(width), static_cast<int>(height), fps};
    info->geometry.base_width = static_cast<unsigned>(video.width);
    info->geometry.base_height = static_cast<unsigned>(video.height);
    info->geometry.max_width = static_cast<unsigned>(video.width);
    info->geometry.max_height = static_cast<unsigned>(video.height);
    info->geometry.aspect_ratio = static_cast<float>(video.width) / static_cast<float>(video.height);
    info->timing.fps = video.refresh_rate_hz;
    info->timing.sample_rate = core ? core->audio_spec().sample_rate_hz : 32768.0;
}
bool retro_load_game(const struct retro_game_info* game) {
    clear_game();
    if (!env_cb) return false;
    if (!game || !game->data || !game->size) {
        report_load_failure("aucune donnée de jeu reçue");
        return false;
    }
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        report_load_failure("le frontend refuse les pixels XRGB8888");
        return false;
    }
    try {
        const auto* bytes = static_cast<const std::uint8_t*>(game->data);
        rom.assign(bytes, bytes + game->size);
        auto machine = new_core();
        apply_options(*machine);
        machine->load_rom(rom, {});
        const auto size = machine->video_spec().pixel_count();
        native_frame.resize(size);
        output_frame.resize(size);
        if (machine->has_battery_ram()) {
            const auto battery = machine->snapshot_battery_ram();
            if (battery) {
                save_ram = battery->data;
                initial_save_ram = save_ram;
                pending_save_import = true;
            }
        }
        const auto state_bytes = machine->save_state().size();
        if (state_bytes > std::numeric_limits<std::uint32_t>::max() - 65544ULL)
            throw std::runtime_error("Save state exceeds limits");
        state_capacity = state_bytes + 65544U;
        core = std::move(machine);
        apply_cheats();
        sensors = core->cartridge_sensors();
        set_input_descriptors();
        declare_achievement_memory();
        retro_rumble_interface rumble{};
        if (env_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble)) rumble_cb = rumble.set_rumble_state;
        can_dupe = false;
        env_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);
        return true;
    } catch (const std::exception& error) {
        clear_game();
        report_load_failure(error.what());
        return false;
    }
}
bool retro_load_game_special(unsigned, const struct retro_game_info*, std::size_t) { return false; }
void retro_unload_game(void) {
    clear_game();
    cheats.clear();
}
void retro_reset(void) {
    if (!core) return;
    try { import_save_ram_once(); core->reset(); update_save_ram(); update_rumble(); refresh_achievement_memory(true); }
    catch (const std::exception&) {}
}
void retro_run(void) {
    if (!core) return;
    try {
        import_save_ram_once();
        if (poll_cb) poll_cb();
        if (input_cb) for (const auto& key : keys)
            core->set_button(key.button,
                input_cb(0, RETRO_DEVICE_JOYPAD, 0, key.id) != 0);
        if ((sensors.tilt || sensors.rotation) && input_cb) {
            const int stick_x = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                         RETRO_DEVICE_ID_ANALOG_X);
            const int stick_y = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                         RETRO_DEVICE_ID_ANALOG_Y);
#if defined(RAVENEMU_LIBRETRO_GBA)
            // Stick right: clockwise rotation, or the right side down; stick
            // up (negative Y): the top edge away from the player.
            if (sensors.rotation) core->set_rotation_speed(stick_x);
            if (sensors.tilt) core->set_gba_tilt(stick_x, -stick_y);
#else
            const auto tilt = tilt_from_stick(stick_x, stick_y);
            core->set_game_boy_acceleration(tilt.x, tilt.y);
#endif
        }
        update_options();
        const int av = wanted_av();
        // An image is skipped only when the frontend can show the previous one.
        const bool video = (av & RETRO_AV_ENABLE_VIDEO) != 0 || !can_dupe;
        core->run_frame(native_frame, video);
        update_save_ram();
        update_rumble();
        refresh_achievement_memory(false);
        if (video) {
            refresh_video();
        } else if (video_cb) {
            const auto spec = core->video_spec();
            video_cb(nullptr, static_cast<unsigned>(spec.width), static_cast<unsigned>(spec.height), 0);
        }
        refresh_audio((av & RETRO_AV_ENABLE_AUDIO) != 0);
    } catch (const std::exception& error) {
        // Never propagate an exception through Libretro's C ABI; say it once per game.
        if (!run_error_logged) log_line(RETRO_LOG_ERROR, std::string("image interrompue : ") + error.what());
        run_error_logged = true;
    }
}
std::size_t retro_serialize_size(void) { return core ? state_capacity : 0; }
bool retro_serialize(void* data, std::size_t size) {
    if (!core || !data || size < state_capacity || state_capacity < 8) return false;
    try {
        import_save_ram_once();
        const auto state = core->save_state();
        if (state.size() > state_capacity - 8 ||
            state.size() > std::numeric_limits<std::uint32_t>::max()) return false;
        auto* bytes = static_cast<std::uint8_t*>(data);
        std::memset(bytes, 0, state_capacity);
        bytes[0] = 'R'; bytes[1] = 'V'; bytes[2] = 'L'; bytes[3] = '1';
        const auto length = static_cast<std::uint32_t>(state.size());
        for (unsigned i = 0; i < 4; ++i)
            bytes[4 + i] = static_cast<std::uint8_t>(length >> (8U * i));
        std::memcpy(bytes + 8, state.data(), state.size());
        return true;
    } catch (const std::exception&) { return false; }
}
bool retro_unserialize(const void* data, std::size_t size) {
    if (!core || !data || size < 8) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (bytes[0] != 'R' || bytes[1] != 'V' || bytes[2] != 'L' || bytes[3] != '1')
        return false;
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i)
        length |= static_cast<std::uint32_t>(bytes[i + 4]) << (8U * i);
    if (length > size - 8) return false;
    try {
        import_save_ram_once();
        core->load_state({bytes + 8, static_cast<std::size_t>(length)});
        update_save_ram();
        update_rumble();
        refresh_achievement_memory(true);
        return true;
    } catch (const std::exception& error) {
        log_line(RETRO_LOG_WARN, std::string("état refusé : ") + error.what());
        return false;
    }
}
void retro_cheat_reset(void) {
    cheats.clear();
    try { apply_cheats(); } catch (const std::exception&) {}
}
// One menu entry: Game Genie or GameShark codes on Game Boy, Codebreaker,
// GameShark or Action Replay codes on GBA, lines joined by '+' or spaces.
// A refused code says why, on screen and in the log.
void retro_cheat_set(unsigned index, bool enabled, const char* code) {
    if (index >= max_cheats) {
        log_line(RETRO_LOG_WARN, "code de triche " + std::to_string(index + 1) + " ignoré : trop de codes");
        return;
    }
    try {
        if (cheats.size() <= index) cheats.resize(index + 1);
        cheats[index].reset();
        if (enabled && code && *code) {
#if defined(RAVENEMU_LIBRETRO_GBA)
            constexpr auto console = ravenemu::Console::game_boy_advance;
#else
            constexpr auto console = ravenemu::Console::game_boy;
#endif
            try {
                cheats[index] = ravenemu::parse_cheat(console, code);
            } catch (const ravenemu::CheatError& error) {
                const auto number = std::to_string(index + 1);
                log_line(RETRO_LOG_WARN, "code de triche " + number + " refusé : " + error.what());
                show_message(std::string(name) + " : code de triche " + number + " refusé (" + error.what() + ")");
            }
        }
        apply_cheats();
    } catch (const std::exception& error) {
        log_line(RETRO_LOG_ERROR, std::string("codes de triche non appliqués : ") + error.what());
    }
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
// Battery SRAM is the only exposed mutable memory. System memory reaches the
// frontend only through the memory map, a copy that writes cannot change: no
// misleading writable system RAM view.
void* retro_get_memory_data(unsigned id) {
    return id == RETRO_MEMORY_SAVE_RAM && !save_ram.empty() ? save_ram.data() : nullptr;
}
std::size_t retro_get_memory_size(unsigned id) {
    return id == RETRO_MEMORY_SAVE_RAM ? save_ram.size() : 0;
}
} // extern "C"
