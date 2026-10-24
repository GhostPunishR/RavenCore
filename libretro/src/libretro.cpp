#include <libretro.h>
#include <ravenemu/core.hpp>
#if defined(RAVENEMU_LIBRETRO_GB)
#include <ravenemu/gb/core.hpp>
#elif defined(RAVENEMU_LIBRETRO_GBC)
#include <ravenemu/gb/core.hpp>
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
#include <fstream>
#include <initializer_list>
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

// One console: its ROM, the battery copy the frontend persists, its image.
// A game loaded alone has one; the "link" subsystem two, joined by a link
// cable. Player 1 comes first: on the left, heard, and the one RetroAchievements
// and the cheats see. Player i reads the controller on port i.
struct Player {
    std::unique_ptr<ravenemu::Core> core;
    std::vector<std::uint8_t> rom;
    std::vector<std::uint8_t> save_ram, initial_save_ram;
    std::vector<std::int32_t> native_frame;
    bool pending_save_import = false;
    bool rumble_on = false;
    // Sensors of the cartridge, driven by the player's left analog stick, and
    // for player 1 by the device's own sensors.
    ravenemu::CartridgeSensors sensors{};
};
std::vector<Player> players;
// Built after the consoles and destroyed before them.
std::unique_ptr<ravenemu::LinkCable> cable;
ravenemu::Core* core = nullptr;  // player 1's, while a game is loaded
std::vector<std::uint32_t> output_frame;  // the players side by side
std::size_t state_capacity = 0;
retro_set_rumble_state_t rumble_cb = nullptr;

// The "link" subsystem: two games joined by a link cable, each with its own
// save, which RetroArch names after that game's ROM.
constexpr unsigned link_subsystem = 1;
constexpr unsigned link_save_ram = 0x101;  // memory type of player 1's save, then player 2's

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
#define RAVENEMU_OPTION(name) "ravenemu_gb_" name
#elif defined(RAVENEMU_LIBRETRO_GBC)
constexpr const char* label = "RavenGBC";
constexpr const char* name = "RavenGBC";
constexpr const char* extensions = "gbc|gb";
constexpr unsigned width = 160, height = 144;
constexpr double fps = 4'194'304.0 / 70'224.0;
#define RAVENEMU_OPTION(name) "ravenemu_gbc_" name
#else
constexpr const char* label = "RavenGBA (experimental)";
constexpr const char* name = "RavenGBA";
constexpr const char* extensions = "gba";
constexpr unsigned width = 240, height = 160;
constexpr double fps = 16'777'216.0 / 280'896.0;
#define RAVENEMU_OPTION(name) "ravenemu_gba_" name
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
constexpr Shades gray_shades{0x00ffffff, 0x00aaaaaa, 0x00555555, 0x00000000};
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

// Mélange d'images, après la conversion des couleurs. « Mélange » montre la
// moyenne des deux dernières images : un objet affiché une image sur deux
// reste visible, à moitié. « Rémanence » garde 3/8 de l'image affichée, comme
// un écran LCD lent. L'historique repart à chaque jeu et à chaque changement.
enum class Blending { off, mix, ghosting };
Blending blending = Blending::off;
std::vector<std::uint32_t> blend_history;
// Haut et bas, ou gauche et droite, ensemble : impossible sur la croix.
bool opposing_allowed = false;
// Cartridge tilt in the C API's units: -32768..32767 per axis for -1 g..1 g,
// x positive when the console's right side goes down, y when its top edge
// moves away from the player. A full stick deflection is a 1 g tilt: stick
// right lowers the right side, stick up (negative Y) moves the top away.
struct Tilt { int x; int y; };
constexpr int clamp_axis(int value) { return std::clamp(value, -32768, 32767); }
constexpr Tilt tilt_from_stick(int stick_x, int stick_y) { return {stick_x, clamp_axis(-stick_y)}; }
// MBC7 accelerometer (Kirby Tilt 'n' Tumble, Command Master): 0x81D0 at rest,
// about 0x70 per g. Signs follow accelerometer physics with the sensor's X
// axis to the cartridge's right and Y to its top: tilting right lowers X,
// tilting the top away lowers Y. Not yet checked in game: flip the signs here
// if it plays mirrored.
constexpr int mbc7_units_per_g = 0x70;
constexpr int mbc7_axis(int tilt) { return -tilt * mbc7_units_per_g / 0x7fff; }
static_assert(mbc7_axis(tilt_from_stick(0, 0).x) == 0 && mbc7_axis(tilt_from_stick(0, 0).y) == 0);
static_assert(mbc7_axis(tilt_from_stick(0x7fff, 0).x) == -mbc7_units_per_g);
static_assert(mbc7_axis(tilt_from_stick(-0x8000, 0).x) == mbc7_units_per_g);
static_assert(mbc7_axis(tilt_from_stick(0, -0x8000).y) == -mbc7_units_per_g);
static_assert(mbc7_axis(tilt_from_stick(0, 0x7fff).y) == mbc7_units_per_g);

// The device's own sensors (a phone, a gamepad that has them), added to the
// stick. Libretro gives the acceleration in g along the displayed screen's
// axes (x to the right, y up), gravity included; the gyroscope in rad/s,
// positive counterclockwise; the light in lux. Same conversions as RavenEmu
// Android: a device lying flat is not tilted.
retro_sensor_interface sensor_interface{};
struct DeviceSensors { bool accelerometer, gyroscope, illuminance; };
DeviceSensors device_sensors{};
// Running norm of the acceleration: some older frontends give it in m/s².
float acceleration_norm = 0.0f;

// -1..1 to -32768..32767, rounded; NaN gives 0.
constexpr int to_axis(float unit) {
    if (!(unit == unit)) return 0;
    const float scaled = unit * 32768.0f;
    if (scaled >= 32767.0f) return 32767;
    if (scaled <= -32768.0f) return -32768;
    return static_cast<int>(scaled + (scaled < 0.0f ? -0.5f : 0.5f));
}
// A side going down reads negative, the cartridge counts it positive.
constexpr Tilt tilt_from_acceleration(float x_in_g, float y_in_g) { return {to_axis(-x_in_g), to_axis(-y_in_g)}; }
// Half a turn per second is the gyroscope's full scale; the cartridge counts
// clockwise.
constexpr float full_rotation = 3.14159265f;
constexpr int rotation_from_gyroscope(float z) { return to_axis(-z / full_rotation); }
static_assert(tilt_from_acceleration(-0.5f, 0.25f).x == 16384 && tilt_from_acceleration(-0.5f, 0.25f).y == -8192);
static_assert(tilt_from_acceleration(0.0f, -2.0f).y == 32767 && tilt_from_acceleration(0.0f, 2.0f).y == -32768);
static_assert(rotation_from_gyroscope(-full_rotation) == 32767 && rotation_from_gyroscope(full_rotation / 2) == -16384);
// Lux to 0..255 on a log scale, from a bright room (1000 lx: 0) to direct sun
// (100 000 lx: 255); an overcast sky, around 10 000 lx, gives half. Like the
// real cartridge, Boktai wants daylight.
[[maybe_unused]] int light_from_lux(float lux) {
    constexpr double room = 1'000.0, sun = 100'000.0;
    if (!(lux > room)) return 0;
    const double share =
        (std::log10(static_cast<double>(lux)) - std::log10(room)) / (std::log10(sun) - std::log10(room));
    return std::clamp(static_cast<int>(std::lround(share * 255.0)), 0, 255);
}

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
    {RAVENEMU_OPTION("frame_blending"), "Mélange d'images",
     "Atténue le scintillement des jeux qui alternent deux images, comme le faisait l'écran de la console.", "video",
     {{"disabled", "Désactivé"}, {"mix", "Mélange des deux dernières images"}, {"ghosting", "Rémanence de l'écran LCD"}}},
    {RAVENEMU_OPTION("opposing_directions"), "Directions opposées",
     "Haut et bas, ou gauche et droite, ensemble : impossible sur la croix de la console, et certains jeux "
     "plantent. Bloquées, aucune des deux n'est transmise.", "input",
     {{"blocked", "Bloquées"}, {"allowed", "Autorisées"}}},
#if defined(RAVENEMU_LIBRETRO_GBA)
    {RAVENEMU_OPTION("device_sensors"), "Capteurs de l'appareil",
     "Yoshi Topsy-Turvy et Koro Koro Puzzle (inclinaison), WarioWare: Twisted! (rotation) : l'accéléromètre et le "
     "gyroscope du téléphone ou de la manette, s'il en a, s'ajoutent au stick gauche. À plat, l'appareil n'est "
     "pas incliné.", "input",
     {{"enabled", "Utilisés s'il y en a"}, {"disabled", "Stick gauche seul"}}},
#else
    {RAVENEMU_OPTION("device_sensors"), "Capteurs de l'appareil",
     "Cartouches à accéléromètre (Kirby Tilt 'n' Tumble, Command Master) : celui du téléphone ou de la manette, "
     "s'il en a, s'ajoute au stick gauche. À plat, l'appareil n'est pas incliné.", "input",
     {{"enabled", "Utilisés s'il y en a"}, {"disabled", "Stick gauche seul"}}},
#endif
#if !defined(RAVENEMU_LIBRETRO_GBA)
    {RAVENEMU_OPTION("boot_rom"), "Boot ROM",
     "Appliquée au chargement du jeu, si son fichier est dans le dossier système : dmg_boot.bin ou gb_bios.bin "
     "(Game Boy), cgb_boot.bin ou gbc_bios.bin (Game Boy Color), agb_boot.bin (Game Boy Advance).", "system",
     {{"enabled", "Utilisée si présente"}, {"disabled", "Non utilisée"}}},
#endif
#if defined(RAVENEMU_LIBRETRO_GBC)
    {"ravenemu_gbc_hardware", "Console émulée",
     "Appliquée au chargement du jeu. Game Boy Advance : les jeux qui la reconnaissent débloquent leurs bonus "
     "(Zelda Oracle, Shantae…).", "system",
     {{"gbc", "Game Boy Color"}, {"gba", "Game Boy Advance"}}},
#endif
#if defined(RAVENEMU_LIBRETRO_GB)
    {"ravenemu_gb_palette", "Palette", "Teintes des jeux Game Boy.", "video",
     {{"dmg", "Vert (Game Boy)"}, {"pocket", "Gris (Game Boy Pocket)"}, {"gray", "Gris neutre"}}},
#elif defined(RAVENEMU_LIBRETRO_GBC)
    lcd_option("ravenemu_gbc_color_correction"),
    {"ravenemu_gbc_dmg_colors", "Couleurs des jeux Game Boy",
     "Sans boot ROM. Comme sur la GBC : les couleurs que sa boot ROM donne à un jeu qu'elle ne reconnaît "
     "pas. Gris : quatre gris.", "video",
     {{"auto", "Comme sur la GBC"}, {"grayscale", "Gris"}}},
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
    {"ravenemu_gba_bios", "BIOS",
     "Appliqué au chargement du jeu. Vrai BIOS : gba_bios.bin (16 Kio) du dossier système, celui d'une console ; "
     "sinon, ou s'il manque, RavenCore répond lui-même aux appels du BIOS. Un état ne se recharge qu'avec le "
     "BIOS qui l'a créé.", "system",
     {{"disabled", "BIOS de RavenCore"}, {"enabled", "Vrai BIOS, avec son animation"},
      {"skip_intro", "Vrai BIOS, sans animation"}}},
    {"ravenemu_gba_solar_light", "Lumière du capteur solaire",
     "Boktai : part du plein soleil que reçoit le capteur de la cartouche, ou la lumière que mesure "
     "l'appareil : rien sous 1000 lux (une pièce éclairée), le plein soleil vers 100 000 lux.", "cartridge",
     {{"50", "50 %"}, {"60", "60 %"}, {"70", "70 %"}, {"80", "80 %"}, {"90", "90 %"}, {"100", "100 %"},
      {"0", "0 %"}, {"10", "10 %"}, {"20", "20 %"}, {"30", "30 %"}, {"40", "40 %"},
      {"sensor", "Capteur de lumière de l'appareil"}}},
#endif
};

void declare_options() {
    unsigned version = 0;
    if (env_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        static std::array<retro_core_option_v2_category, 5> categories{{
            {"video", "Image", "Couleurs et rendu de l'image."},
            {"input", "Commandes", "Croix, boutons et capteurs."},
            {"system", "Système", "Console émulée, boot ROM et BIOS, appliqués au chargement du jeu."},
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
// Image and input options: read when a game loads, and again whenever they change.
void read_live_options() {
#if defined(RAVENEMU_LIBRETRO_GB)
    shades = option_is("ravenemu_gb_palette", "pocket") ? pocket_shades
           : option_is("ravenemu_gb_palette", "gray")   ? gray_shades
                                                        : dmg_shades;
#elif defined(RAVENEMU_LIBRETRO_GBC)
    lcd_correction = option_is("ravenemu_gbc_color_correction", "enabled");
#else
    lcd_correction = option_is("ravenemu_gba_color_correction", "enabled");
#endif
    const auto previous = blending;
    blending = option_is(RAVENEMU_OPTION("frame_blending"), "mix")        ? Blending::mix
             : option_is(RAVENEMU_OPTION("frame_blending"), "ghosting") ? Blending::ghosting
                                                                         : Blending::off;
    if (blending != previous) blend_history.clear();
    opposing_allowed = option_is(RAVENEMU_OPTION("opposing_directions"), "allowed");
}

#if defined(RAVENEMU_LIBRETRO_GBA)
// Boktai's solar sensor: a percentage of full sunlight, 0 to 100. With the
// device's light sensor, 50 % until it answers, or without one.
void apply_solar_option(ravenemu::Core& machine) {
    int percent = 50;
    if (const char* value = option("ravenemu_gba_solar_light")) {
        for (int choice = 0; choice <= 100; choice += 10)
            if (std::string_view(value) == std::to_string(choice)) percent = choice;
    }
    machine.set_solar_light(percent * 255 / 100);
}
#endif
#if defined(RAVENEMU_LIBRETRO_GBC)
// Couleurs des jeux Game Boy : sans effet sur un jeu GBC, appliquées tout de suite.
void apply_dmg_colors(ravenemu::Core& machine) {
    machine.set_gbc_compatibility_colors(option_is("ravenemu_gbc_dmg_colors", "grayscale")
                                             ? ravenemu::GbcCompatibilityColors::grayscale
                                             : ravenemu::GbcCompatibilityColors::automatic);
}
#endif
// Turns on the device sensors the cartridge needs, and off the others: a
// sensor left on drains the battery.
void switch_device_sensors(DeviceSensors wanted) {
    const auto set = [](bool& on, bool want, retro_sensor_action enable, retro_sensor_action disable) {
        if (on == want) return;
        auto* call = sensor_interface.set_sensor_state;
        if (want) on = call && sensor_interface.get_sensor_input && call(0, enable, 60);
        else {
            if (call) call(0, disable, 0);
            on = false;
        }
    };
    set(device_sensors.accelerometer, wanted.accelerometer, RETRO_SENSOR_ACCELEROMETER_ENABLE,
        RETRO_SENSOR_ACCELEROMETER_DISABLE);
    set(device_sensors.gyroscope, wanted.gyroscope, RETRO_SENSOR_GYROSCOPE_ENABLE, RETRO_SENSOR_GYROSCOPE_DISABLE);
    set(device_sensors.illuminance, wanted.illuminance, RETRO_SENSOR_ILLUMINANCE_ENABLE,
        RETRO_SENSOR_ILLUMINANCE_DISABLE);
}
void update_device_sensors() {
    const auto sensors = core ? players.front().sensors : ravenemu::CartridgeSensors{};
    const bool motion = core && !option_is(RAVENEMU_OPTION("device_sensors"), "disabled");
    DeviceSensors wanted{motion && sensors.tilt, motion && sensors.rotation, false};
#if defined(RAVENEMU_LIBRETRO_GBA)
    wanted.illuminance = core && sensors.solar && option_is("ravenemu_gba_solar_light", "sensor");
    const bool light_was_on = device_sensors.illuminance;
#endif
    switch_device_sensors(wanted);
#if defined(RAVENEMU_LIBRETRO_GBA)
    if (wanted.illuminance && !device_sensors.illuminance && !light_was_on)
        show_message(std::string(name) + " : pas de capteur de lumière, soleil à 50 %");
#endif
}
// The device's tilt, from its accelerometer.
Tilt device_tilt() {
    const auto read = sensor_interface.get_sensor_input;
    const float x = read(0, RETRO_SENSOR_ACCELEROMETER_X);
    const float y = read(0, RETRO_SENSOR_ACCELEROMETER_Y);
    const float z = read(0, RETRO_SENSOR_ACCELEROMETER_Z);
    // At rest, gravity alone measures 1 g, or 9.8 in m/s²: a running norm
    // above 3 gives those units away. A zero reading (no event yet) is skipped.
    const float norm = std::sqrt(x * x + y * y + z * z);
    if (norm > 0.0f)
        acceleration_norm = acceleration_norm > 0.0f ? acceleration_norm + (norm - acceleration_norm) / 32.0f : norm;
    const float to_g = acceleration_norm > 3.0f ? 1.0f / 9.80665f : 1.0f;
    return tilt_from_acceleration(x * to_g, y * to_g);
}
void apply_options(ravenemu::Core& machine) {
    read_live_options();
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
#elif defined(RAVENEMU_LIBRETRO_GBC)
    apply_dmg_colors(machine);
#else
    static_cast<void>(machine);
#endif
}
// The image options, the sensors and the solar light change at any time during the game.
void update_options() {
    bool updated = false;
    if (!core || !env_cb || !env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) || !updated) return;
    read_live_options();
    update_device_sensors();
    for (auto& player : players) {
#if defined(RAVENEMU_LIBRETRO_GBA)
        apply_solar_option(*player.core);
#elif defined(RAVENEMU_LIBRETRO_GBC)
        apply_dmg_colors(*player.core);
#else
        static_cast<void>(player);
#endif
    }
}

// Premier des fichiers du dossier système qui existe et a l'une des tailles
// attendues ; vide sinon. Un fichier d'une autre taille est ignoré, et le
// journal dit pourquoi.
std::vector<std::uint8_t> read_system_file(std::initializer_list<const char*> files,
                                           std::initializer_list<std::size_t> sizes, const std::string& what) {
    const char* directory = nullptr;
    if (!env_cb || !env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory) || !directory || !*directory)
        return {};
    const auto largest = std::max(sizes);
    for (const char* file : files) {
        const auto path = std::string(directory) + "/" + file;
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        std::vector<std::uint8_t> image(largest + 1);
        input.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
        image.resize(static_cast<std::size_t>(input.gcount()));
        if (std::find(sizes.begin(), sizes.end(), image.size()) != sizes.end()) {
            log_line(RETRO_LOG_INFO, what + " : " + path);
            return image;
        }
        log_line(RETRO_LOG_WARN, what + " : fichier ignoré, taille inattendue : " + path);
    }
    return {};
}
#if !defined(RAVENEMU_LIBRETRO_GBA)
// Boot ROM du dossier système, si l'option le permet ; sinon RavenCore démarre
// le jeu dans l'état où la boot ROM l'aurait laissé.
std::vector<std::uint8_t> read_boot_rom(std::initializer_list<const char*> files,
                                        std::initializer_list<std::size_t> sizes) {
    if (option_is(RAVENEMU_OPTION("boot_rom"), "disabled")) return {};
    return read_system_file(files, sizes, "boot ROM");
}
#endif
#if defined(RAVENEMU_LIBRETRO_GB)
std::unique_ptr<ravenemu::Core> new_core() {
    return ravenemu::gb::make_core(ravenemu::gb::HardwareModel::dmg,
                                   read_boot_rom({"dmg_boot.bin", "gb_bios.bin"}, {0x100}));
}
#elif defined(RAVENEMU_LIBRETRO_GBC)
// La boot ROM de la GBA ne diffère de celle de la GBC que par ce qu'elle
// laisse dans B : chacune a son fichier.
std::unique_ptr<ravenemu::Core> new_core() {
    const bool agb = option_is("ravenemu_gbc_hardware", "gba");
    auto boot = agb ? read_boot_rom({"agb_boot.bin"}, {0x800, 0x900})
                    : read_boot_rom({"cgb_boot.bin", "gbc_bios.bin"}, {0x800, 0x900});
    return ravenemu::gb::make_core(agb ? ravenemu::gb::HardwareModel::agb : ravenemu::gb::HardwareModel::cgb, boot);
}
#else
// Le vrai BIOS, si l'option le demande et que gba_bios.bin est là ; sinon le
// BIOS réécrit de RavenCore, et un message dit pourquoi.
std::unique_ptr<ravenemu::Core> new_core() {
    auto machine = ravenemu::make_gba_core(std::nullopt);
    const bool skip_intro = option_is("ravenemu_gba_bios", "skip_intro");
    if (!skip_intro && !option_is("ravenemu_gba_bios", "enabled")) return machine;
    const auto bios = read_system_file({"gba_bios.bin"}, {0x4000}, "BIOS GBA");
    if (bios.empty()) show_message("gba_bios.bin (16 Kio) absent du dossier système : BIOS de RavenCore");
    else machine->set_gba_bios(bios, skip_intro);
    return machine;
}
#endif

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
    for (auto& player : players) {
        if (player.save_ram.empty() || !player.core->battery_ram_dirty()) continue;
        auto state = player.core->snapshot_battery_ram();
        if (!state || state->data.size() != player.save_ram.size()) continue;
        std::copy(state->data.begin(), state->data.end(), player.save_ram.begin());
        player.core->acknowledge_battery_ram_saved(state->generation);
    }
}
/* Libretro frontends restore battery files into SAVE_RAM after load_game.
 * Apply this initial write before the first emulated frame or serialization. */
void import_save_ram_once() {
    for (auto& player : players) {
        if (!player.pending_save_import) continue;
        player.pending_save_import = false;
        if (!player.save_ram.empty() && player.save_ram != player.initial_save_ram)
            player.core->load_rom(player.rom, player.save_ram);
        player.initial_save_ram.clear();
    }
}
void update_rumble() {
    if (!rumble_cb) return;
    for (std::size_t port = 0; port < players.size(); ++port) {
        auto& player = players[port];
        const bool active = player.core->rumble_active();
        if (active == player.rumble_on) continue;
        player.rumble_on = active;
        rumble_cb(static_cast<unsigned>(port), RETRO_RUMBLE_STRONG, active ? 0xffff : 0);
    }
}
void set_input_descriptors() {
    std::vector<retro_input_descriptor> descriptors;
    for (unsigned port = 0; port < players.size(); ++port) {
        const auto& sensors = players[port].sensors;
        const std::initializer_list<retro_input_descriptor> pad{
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B"},
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A"},
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
            {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
        };
        descriptors.insert(descriptors.end(), pad);
#if defined(RAVENEMU_LIBRETRO_GBA)
        descriptors.push_back({port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"});
        descriptors.push_back({port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"});
#endif
        if (sensors.tilt) {
            descriptors.push_back({port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_X, "Tilt Left/Right"});
            descriptors.push_back({port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_Y, "Tilt Forward/Back"});
        }
        if (sensors.rotation) {
            descriptors.push_back({port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_X, "Rotate (gyro sensor)"});
        }
    }
    descriptors.push_back({0, 0, 0, 0, nullptr});
    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors.data());
}
void clear_game() {
    // Never leave a gamepad motor running after the game that started it.
    for (std::size_t port = 0; port < players.size(); ++port)
        if (rumble_cb && players[port].rumble_on) rumble_cb(static_cast<unsigned>(port), RETRO_RUMBLE_STRONG, 0);
    rumble_cb = nullptr;
    switch_device_sensors({});
    acceleration_norm = 0.0f;
    cable.reset();
    core = nullptr;
    players.clear();
    output_frame.clear();
    state_capacity = 0;
    achievement_ram.clear();
    achievement_sizes = {};
    blend_history.clear();
    run_error_logged = false;
}
void blend_output() {
    if (blending == Blending::off) return;
    if (blend_history.size() != output_frame.size()) blend_history = output_frame;
    for (std::size_t i = 0; i < output_frame.size(); ++i) {
        const auto current = output_frame[i];
        const auto previous = blend_history[i];
        std::uint32_t blended = 0;
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const auto now = (current >> shift) & 0xffU;
            const auto before = (previous >> shift) & 0xffU;
            blended |= (blending == Blending::mix ? (now + before + 1) / 2 : (now * 5 + before * 3 + 4) / 8) << shift;
        }
        output_frame[i] = blended;
        blend_history[i] = blending == Blending::mix ? current : blended;
    }
}
// Width of the image sent to the frontend: the players side by side.
unsigned output_width() {
    return core ? static_cast<unsigned>(core->video_spec().width) * static_cast<unsigned>(players.size()) : width;
}
void refresh_video() {
    if (!core || !video_cb) return;
    const auto spec = core->video_spec();
    const auto columns = static_cast<std::size_t>(spec.width);
    const auto stride = static_cast<std::size_t>(output_width());
    for (std::size_t index = 0; index < players.size(); ++index) {
        const auto& player = players[index];
        const bool indexed = player.core->framebuffer_format() == ravenemu::FramebufferFormat::indexed_4;
        const auto* source = player.native_frame.data();
        for (std::size_t row = 0; row < player.native_frame.size() / columns; ++row) {
            auto* line = output_frame.data() + row * stride + index * columns;
            for (std::size_t column = 0; column < columns; ++column) {
                const auto pixel = static_cast<std::uint32_t>(*source++);
                line[column] =
                    indexed ? shades[pixel & 3U] : lcd_correction ? lcd_corrected(pixel) : (pixel & 0x00ffffffU);
            }
        }
    }
    blend_output();
    video_cb(output_frame.data(), static_cast<unsigned>(stride), static_cast<unsigned>(spec.height),
             stride * sizeof(std::uint32_t));
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
// Only player 1 is heard. The other consoles' samples, and those of a frame
// whose sound the frontend does not want, are drained all the same, or they
// would pile up for the next one.
void refresh_audio(bool deliver) {
    std::array<std::int16_t, 4096> pcm{};
    for (std::size_t index = 0; index < players.size(); ++index) {
        const bool heard = deliver && index == 0;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto count = players[index].core->read_audio(pcm);
            if (count < 2) break;
            const auto frames = count / 2;
            if (heard && batch_cb) batch_cb(pcm.data(), frames);
            else if (heard && sample_cb) for (std::size_t i = 0; i < frames; ++i)
                sample_cb(pcm[2 * i], pcm[2 * i + 1]);
            if (count < pcm.size()) break;
        }
    }
}
// What the frontend will use of this frame: it skips the image or the sound
// of the frames it runs ahead or fast-forwards. Without an answer, both.
int wanted_av() {
    int flags = 0;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &flags)) return flags;
    return RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
}
// The buttons and left stick of the controller on `port`, and for player 1
// the device's sensors, handed to the player's console.
void read_controller(Player& player, unsigned port) {
    auto& console = *player.core;
    if (input_cb) {
        std::array<bool, keys.size()> held{};
        for (std::size_t index = 0; index < keys.size(); ++index)
            held[index] = input_cb(port, RETRO_DEVICE_JOYPAD, 0, keys[index].id) != 0;
        // keys commence par haut, bas, gauche, droite.
        if (!opposing_allowed) {
            if (held[0] && held[1]) held[0] = held[1] = false;
            if (held[2] && held[3]) held[2] = held[3] = false;
        }
        for (std::size_t index = 0; index < keys.size(); ++index) console.set_button(keys[index].button, held[index]);
    }
    const auto& sensors = player.sensors;
    const bool device = port == 0;
    if (sensors.tilt || sensors.rotation) {
        int stick_x = 0, stick_y = 0;
        if (input_cb) {
            stick_x = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
            stick_y = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
        }
        // Stick right: clockwise rotation, or the right side down.
        auto tilt = tilt_from_stick(stick_x, stick_y);
        int rotation = stick_x;
        if (device && device_sensors.accelerometer) {
            const auto measured = device_tilt();
            tilt = {clamp_axis(tilt.x + measured.x), clamp_axis(tilt.y + measured.y)};
        }
        if (device && device_sensors.gyroscope)
            rotation = clamp_axis(rotation + rotation_from_gyroscope(
                                                 sensor_interface.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_Z)));
#if defined(RAVENEMU_LIBRETRO_GBA)
        if (sensors.rotation) console.set_rotation_speed(rotation);
        if (sensors.tilt) console.set_gba_tilt(tilt.x, tilt.y);
#else
        console.set_game_boy_acceleration(mbc7_axis(tilt.x), mbc7_axis(tilt.y));
#endif
    }
#if defined(RAVENEMU_LIBRETRO_GBA)
    if (device && device_sensors.illuminance)
        console.set_solar_light(light_from_lux(sensor_interface.get_sensor_input(0, RETRO_SENSOR_ILLUMINANCE)));
#endif
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
    // Two games joined by a link cable, offered by RetroArch's Load Content
    // menu once the core is loaded.
    static const std::array<retro_subsystem_memory_info, 2> saves{{{"srm", link_save_ram}, {"srm", link_save_ram + 1}}};
    static const std::array<retro_subsystem_rom_info, 2> roms{{
        {"Jeu du joueur 1", extensions, false, false, true, &saves[0], 1},
        {"Jeu du joueur 2", extensions, false, false, true, &saves[1], 1},
    }};
    static const std::array<retro_subsystem_info, 2> subsystems{{
        {"Deux joueurs (câble link)", "link", roms.data(), static_cast<unsigned>(roms.size()), link_subsystem},
        {},
    }};
    env_cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, const_cast<retro_subsystem_info*>(subsystems.data()));
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
    const auto shown = output_width();
    info->geometry.base_width = shown;
    info->geometry.base_height = static_cast<unsigned>(video.height);
    info->geometry.max_width = shown;
    info->geometry.max_height = static_cast<unsigned>(video.height);
    info->geometry.aspect_ratio = static_cast<float>(shown) / static_cast<float>(video.height);
    info->timing.fps = video.refresh_rate_hz;
    info->timing.sample_rate = core ? core->audio_spec().sample_rate_hz : 32768.0;
}
} // extern "C"

namespace {
// One game per player; two are joined by a link cable.
bool load_players(std::span<const retro_game_info> games) {
    clear_game();
    if (!env_cb) return false;
    for (const auto& game : games) {
        if (!game.data || !game.size) {
            report_load_failure("aucune donnée de jeu reçue");
            return false;
        }
    }
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        report_load_failure("le frontend refuse les pixels XRGB8888");
        return false;
    }
    std::size_t index = 0;
    try {
        // A state: "RVL", the number of players, then each one's length and state.
        state_capacity = 4;
        players.resize(games.size());
        for (index = 0; index < games.size(); ++index) {
            auto& player = players[index];
            const auto* bytes = static_cast<const std::uint8_t*>(games[index].data);
            player.rom.assign(bytes, bytes + games[index].size);
            player.core = new_core();
            apply_options(*player.core);
            player.core->load_rom(player.rom, {});
            player.native_frame.resize(player.core->video_spec().pixel_count());
            if (player.core->has_battery_ram()) {
                const auto battery = player.core->snapshot_battery_ram();
                if (battery) {
                    player.save_ram = battery->data;
                    player.initial_save_ram = player.save_ram;
                    player.pending_save_import = true;
                }
            }
            const auto state_bytes = player.core->save_state().size();
            if (state_bytes > std::numeric_limits<std::uint32_t>::max() / 4 - 65540ULL)
                throw std::runtime_error("Save state exceeds limits");
            state_capacity += 4 + state_bytes + 65536U;
            player.sensors = player.core->cartridge_sensors();
            output_frame.resize(output_frame.size() + player.native_frame.size());
        }
        if (players.size() > 1) {
            std::vector<ravenemu::Core*> consoles;
            for (auto& player : players) consoles.push_back(player.core.get());
            cable = ravenemu::connect_link_cable(consoles);
        }
        core = players.front().core.get();
        apply_cheats();
        switch_device_sensors({});
        sensor_interface = {};
        env_cb(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensor_interface);
        update_device_sensors();
        set_input_descriptors();
        declare_achievement_memory();
        retro_rumble_interface rumble{};
        if (env_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble)) rumble_cb = rumble.set_rumble_state;
        can_dupe = false;
        env_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);
        return true;
    } catch (const std::exception& error) {
        clear_game();
        report_load_failure(games.size() > 1 && index < games.size()
                                ? "joueur " + std::to_string(index + 1) + " : " + error.what()
                                : std::string(error.what()));
        return false;
    }
}
} // namespace

extern "C" {
bool retro_load_game(const struct retro_game_info* game) {
    if (!game) {
        clear_game();
        report_load_failure("aucune donnée de jeu reçue");
        return false;
    }
    return load_players({game, 1});
}
bool retro_load_game_special(unsigned type, const struct retro_game_info* games, std::size_t count) {
    if (type != link_subsystem || count != 2 || !games) {
        clear_game();
        report_load_failure("sous-système inconnu");
        return false;
    }
    return load_players({games, count});
}
void retro_unload_game(void) {
    clear_game();
    cheats.clear();
}
void retro_reset(void) {
    if (!core) return;
    try {
        import_save_ram_once();
        for (auto& player : players) player.core->reset();
        update_save_ram();
        update_rumble();
        refresh_achievement_memory(true);
    } catch (const std::exception&) {}
}
void retro_run(void) {
    if (!core) return;
    try {
        import_save_ram_once();
        if (poll_cb) poll_cb();
        for (unsigned port = 0; port < players.size(); ++port) read_controller(players[port], port);
        update_options();
        const int av = wanted_av();
        // An image is skipped only when the frontend can show the previous one.
        const bool video = (av & RETRO_AV_ENABLE_VIDEO) != 0 || !can_dupe;
        if (cable) {
            std::array<std::span<std::int32_t>, 2> frames{};
            for (std::size_t index = 0; index < players.size(); ++index) frames.at(index) = players[index].native_frame;
            cable->run_frame(std::span(frames).first(players.size()), video);
        } else {
            core->run_frame(players.front().native_frame, video);
        }
        update_save_ram();
        update_rumble();
        refresh_achievement_memory(false);
        if (video) {
            refresh_video();
        } else if (video_cb) {
            video_cb(nullptr, output_width(), static_cast<unsigned>(core->video_spec().height), 0);
        }
        refresh_audio((av & RETRO_AV_ENABLE_AUDIO) != 0);
    } catch (const std::exception& error) {
        // Never propagate an exception through Libretro's C ABI; say it once per game.
        if (!run_error_logged) log_line(RETRO_LOG_ERROR, std::string("image interrompue : ") + error.what());
        run_error_logged = true;
    }
}
std::size_t retro_serialize_size(void) { return core ? state_capacity : 0; }
// "RVL", then '1' for a game alone or '2' for two linked, then each console's
// state behind its length (32 bits, little-endian).
bool retro_serialize(void* data, std::size_t size) {
    if (!core || !data || size < state_capacity) return false;
    try {
        import_save_ram_once();
        auto* bytes = static_cast<std::uint8_t*>(data);
        std::memset(bytes, 0, state_capacity);
        bytes[0] = 'R'; bytes[1] = 'V'; bytes[2] = 'L'; bytes[3] = static_cast<std::uint8_t>('0' + players.size());
        std::size_t offset = 4;
        for (auto& player : players) {
            const auto state = player.core->save_state();
            if (state.size() > state_capacity - offset - 4 ||
                state.size() > std::numeric_limits<std::uint32_t>::max()) return false;
            const auto length = static_cast<std::uint32_t>(state.size());
            for (unsigned i = 0; i < 4; ++i)
                bytes[offset + i] = static_cast<std::uint8_t>(length >> (8U * i));
            std::memcpy(bytes + offset + 4, state.data(), state.size());
            offset += 4 + state.size();
        }
        return true;
    } catch (const std::exception&) { return false; }
}
bool retro_unserialize(const void* data, std::size_t size) {
    if (!core || !data || size < 4) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto count = static_cast<std::uint8_t>('0' + players.size());
    if (bytes[0] != 'R' || bytes[1] != 'V' || bytes[2] != 'L' || bytes[3] != count) return false;
    std::vector<std::span<const std::uint8_t>> states;
    for (std::size_t offset = 4; states.size() < players.size();) {
        if (size - offset < 4) return false;
        std::uint32_t length = 0;
        for (unsigned i = 0; i < 4; ++i)
            length |= static_cast<std::uint32_t>(bytes[offset + i]) << (8U * i);
        if (length > size - offset - 4) return false;
        states.emplace_back(bytes + offset + 4, length);
        offset += 4 + length;
    }
    try {
        import_save_ram_once();
        // Each console replaces its machine only once its state is read
        // whole; if player 2 refuses its state, player 1 goes back to its own.
        std::vector<std::uint8_t> before;
        if (players.size() > 1) before = core->save_state();
        for (std::size_t index = 0; index < players.size(); ++index) {
            try {
                players[index].core->load_state(states[index]);
            } catch (const std::exception&) {
                if (index > 0) core->load_state(before);
                throw;
            }
        }
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
} // extern "C"

namespace {
std::vector<std::uint8_t>* save_ram_of(unsigned id) {
    if (players.size() == 1 && id == RETRO_MEMORY_SAVE_RAM) return &players.front().save_ram;
    if (players.size() > 1 && id >= link_save_ram && id - link_save_ram < players.size())
        return &players[id - link_save_ram].save_ram;
    return nullptr;
}
} // namespace

extern "C" {
// Battery SRAM is the only exposed mutable memory: SAVE_RAM for a game alone,
// one memory type per player when linked (see the subsystem). System memory
// reaches the frontend only through the memory map, a copy that writes cannot
// change: no misleading writable system RAM view.
void* retro_get_memory_data(unsigned id) {
    auto* save = save_ram_of(id);
    return save && !save->empty() ? save->data() : nullptr;
}
std::size_t retro_get_memory_size(unsigned id) {
    const auto* save = save_ram_of(id);
    return save ? save->size() : 0;
}
} // extern "C"
