#include <libretro.h>
#include "check.hpp"
#include "support/interactive_fixture.hpp"
#include "support/firmware_fixture.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using ravenemu::testing::check;
using namespace ravenemu::nds::test_support;

namespace {
std::vector<std::uint32_t> frame;
std::array<bool,16> keys{};
bool pointer_pressed{}, achievements{true}, touch_descriptor{}, x_descriptor{}, y_descriptor{};
std::int16_t pointer_x{}, pointer_y{};
unsigned messages{}, frames{}, polls{};
std::string system_directory;
std::vector<std::int16_t> audio;
unsigned audio_calls{};
std::size_t audio_batch(const std::int16_t* data, std::size_t count) {
    audio.insert(audio.end(),data,data + count * 2); ++audio_calls; return count;
}
void audio_sample(std::int16_t left, std::int16_t right) { audio.push_back(left); audio.push_back(right); }
const char* save_type = "fram8k";

bool environment(unsigned command, void* data) {
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    if (command == RETRO_ENVIRONMENT_GET_VARIABLE) {
        auto& variable = *static_cast<retro_variable*>(data);
        if (std::strcmp(variable.key,"ravenemu_nds_save_type") != 0) return false;
        variable.value = save_type; return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS) { achievements = *static_cast<bool*>(data); return true; }
    if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        if (system_directory.empty()) return false;
        *static_cast<const char**>(data) = system_directory.c_str(); return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS) {
        touch_descriptor = x_descriptor = y_descriptor = false;
        for (auto* d = static_cast<retro_input_descriptor*>(data); d->description; ++d) {
            touch_descriptor |= d->device == RETRO_DEVICE_POINTER;
            x_descriptor |= d->device == RETRO_DEVICE_JOYPAD && d->id == RETRO_DEVICE_ID_JOYPAD_X;
            y_descriptor |= d->device == RETRO_DEVICE_JOYPAD && d->id == RETRO_DEVICE_ID_JOYPAD_Y;
        }
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_MESSAGE) { ++messages; return true; }
    if (command == RETRO_ENVIRONMENT_SET_MEMORY_MAPS)
        check(static_cast<retro_memory_map*>(data)->num_descriptors == 0, "fausse carte mémoire RetroAchievements NDS");
    return false;
}
void video(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    check(width == 256 && height == 384 && pitch == 1024 && data, "géométrie vidéo NDS");
    const auto* pixels = static_cast<const std::uint32_t*>(data);
    frame.assign(pixels,pixels + width * height); ++frames;
}
void poll() { ++polls; }
std::int16_t input(unsigned port, unsigned device, unsigned index, unsigned id) {
    check(port == 0 && index == 0, "port ou index d'entrée NDS");
    if (device == RETRO_DEVICE_JOYPAD && id < keys.size()) return keys[id] ? 1 : 0;
    if (device == RETRO_DEVICE_POINTER) {
        if (id == RETRO_DEVICE_ID_POINTER_PRESSED) return pointer_pressed ? 1 : 0;
        if (id == RETRO_DEVICE_ID_POINTER_X) return pointer_x;
        if (id == RETRO_DEVICE_ID_POINTER_Y) return pointer_y;
    }
    return 0;
}
void run(unsigned count = 2) { while (count--) retro_run(); }
bool load(const std::vector<std::uint8_t>& rom) {
    retro_game_info game{}; game.data = rom.data(); game.size = rom.size(); return retro_load_game(&game);
}
} // namespace

int main() {
    retro_set_environment(environment); retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(poll); retro_set_input_state(input); retro_init();
    retro_system_info info{}; retro_get_system_info(&info);
    check(info.library_name && std::string(info.valid_extensions) == "nds" && !info.need_fullpath && !achievements,
          "métadonnées Libretro NDS");
    retro_system_av_info av{}; retro_get_system_av_info(&av);
    check(av.geometry.base_width == 256 && av.geometry.base_height == 384 && av.timing.fps > 59 && av.timing.fps < 60,
          "AV info avant chargement");
    const auto rom = interactive_cartridge();
    check(load(rom) && x_descriptor && y_descriptor && !touch_descriptor, "chargement homebrew sans BIOS/firmware");
    check(retro_serialize_size() == 0, "taille de savestate fictive");
    std::array<std::uint8_t,8> state{'R','V','L','1',0,0,0,0};
    check(!retro_serialize(state.data(),state.size()) && !retro_unserialize(state.data(),state.size()), "savestate non supportée acceptée");
    auto* save = static_cast<std::uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    check(save && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 8192, "profil FRAM absent");
    save[123] = 0x5a; run();
    check(frame[88 * 256 + 120] == 0xff0000 && frame[0] == 0x00ff00 && frame[192 * 256] == 0x0000ff && messages == 0,
          "homebrew ne produit pas les deux écrans dans Libretro");
    keys[RETRO_DEVICE_ID_JOYPAD_RIGHT] = true; run(); keys[RETRO_DEVICE_ID_JOYPAD_RIGHT] = false;
    check(frame[88 * 256 + 120] == 0x00ff00 && frame[88 * 256 + 122] == 0xff0000, "croix Libretro ne déplace pas le sprite");
    keys[RETRO_DEVICE_ID_JOYPAD_X] = true; run(); keys[RETRO_DEVICE_ID_JOYPAD_X] = false;
    check(frame[88 * 256 + 122] == 0xffffff, "X Libretro non transmis à ARM7");
    check(audio_calls > 0 && std::ranges::all_of(audio,[](auto value) { return value == 0; }), "silence Libretro absent");
    audio.clear(); keys[RETRO_DEVICE_ID_JOYPAD_B] = true; run(1);
    check(audio.size() >= 1094 && audio.size() <= 1098 && std::ranges::any_of(audio,[](auto value) { return value != 0; }),
          "callback audio batch NDS absent ou muet");
    retro_set_audio_sample_batch(nullptr); retro_set_audio_sample(audio_sample); audio.clear(); run(1);
    check(audio.size() >= 1094 && audio.size() <= 1098 && std::ranges::any_of(audio,[](auto value) { return value != 0; }),
          "callback audio unitaire NDS absent");
    for (std::size_t i = 0; i < audio.size(); i += 2) check(audio[i] == audio[i + 1], "canaux stéréo Libretro inversés");
    keys[RETRO_DEVICE_ID_JOYPAD_B] = false; retro_set_audio_sample_batch(audio_batch);
    retro_reset(); run();
    check(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == save && save[123] == 0x5a && frame[88 * 256 + 120] == 0xff0000,
          "reset perd sauvegarde, pointeur stable ou état initial");
    check(polls == frames && polls > 0, "callbacks poll/vidéo manquants");
    retro_unload_game(); check(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == nullptr, "RAM après déchargement");

    const auto directory = std::filesystem::temp_directory_path() /
        ("ravencore-nds-smoke-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{directory};
    std::filesystem::create_directory(directory); system_directory = directory.string();
    const auto firmware_bytes = firmware();
    { std::ofstream f(directory / "firmware.bin",std::ios::binary);
      f.write(reinterpret_cast<const char*>(firmware_bytes.data()),static_cast<std::streamsize>(firmware_bytes.size())); }
    check(load(rom) && touch_descriptor, "firmware fourni non utilisé pour le tactile");
    pointer_pressed = true; pointer_x = 0; pointer_y = -100; run();
    check(frame[192 * 256] == 0x0000ff, "écran supérieur traité comme tactile");
    pointer_y = 0; run(); const auto touched = frame[192 * 256];
    check(touched != 0x0000ff, "pointeur inférieur ne touche pas le convertisseur");
    pointer_x = 32767; pointer_y = 32767; run();
    check(frame[192 * 256] != touched, "coordonnée du pointeur perdue");
    pointer_x = -32768; run(); check(frame[192 * 256] == 0x0000ff, "pointeur hors écran non relâché");
    pointer_pressed = false; retro_unload_game();
    { std::ofstream f(directory / "bios9.bin",std::ios::binary); f.put('x'); }
    check(!load(rom) && messages == 1, "BIOS invalide ignoré sans diagnostic");
    std::filesystem::remove(directory / "bios9.bin");
    auto broken = rom; put32(broken,0x200,0xe5900000); // LDR depuis zéro, sans BIOS ARM9
    check(load(broken), "ROM de diagnostic refusée avant exécution");
    const auto before = frames; run(1); check(messages == 2 && frames == before + 1, "erreur CPU masquée par Libretro");
    run(1); check(messages == 2, "diagnostic répété à chaque image");
    retro_reset(); run(1); check(messages == 3, "reset ne réarme pas l'exécution après erreur");
    retro_deinit();
}
