#include <ravenemu/bridge.h>
#include <synthetic_roms.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

using Handle = std::unique_ptr<ravenemu_bridge, decltype(&ravenemu_destroy)>;

/* The minimal GBA program stores 0x5A into SRAM once, then loops. */
int battery_tracking_test() {
    auto rom = ravenemu::testing::minimal_gba_rom();
    constexpr char marker[] = "SRAM_V";
    std::memcpy(rom.data() + 0x100, marker, sizeof marker - 1);
    Handle handle(ravenemu_create(RAVENEMU_GBA), ravenemu_destroy);
    if (!handle || !ravenemu_load(handle.get(), rom.data(), rom.size(), nullptr, 0)) return 20;
    if (ravenemu_battery_dirty(handle.get())) return 21;

    std::vector<std::uint32_t> frame(240 * 160);
    if (!ravenemu_run_frame(handle.get(), frame.data(), frame.size())) return 22;
    if (!ravenemu_battery_dirty(handle.get())) return 23;

    std::vector<std::uint8_t> battery(ravenemu_battery_size(handle.get()));
    std::uint64_t generation = 0;
    if (battery.empty() ||
        !ravenemu_copy_battery_generation(handle.get(), battery.data(), battery.size(), &generation) ||
        battery[0] != 0x5a) return 24;
    // Acknowledging another generation must not hide the pending write.
    if (!ravenemu_acknowledge_battery(handle.get(), generation + 1) ||
        !ravenemu_battery_dirty(handle.get())) return 25;
    if (!ravenemu_acknowledge_battery(handle.get(), generation) ||
        ravenemu_battery_dirty(handle.get())) return 26;
    if (!ravenemu_run_frame(handle.get(), frame.data(), frame.size()) ||
        ravenemu_battery_dirty(handle.get())) return 27;

    if (ravenemu_battery_dirty(nullptr) || ravenemu_acknowledge_battery(nullptr, 0)) return 28;
    return 0;
}

/* state_size met l'état en cache pour save_state : toute modification de la
 * machine entre les deux doit le rendre caduc, sinon l'état serait périmé. */
int state_cache_test() {
    auto rom = ravenemu::testing::minimal_game_boy_rom();
    Handle handle(ravenemu_create(RAVENEMU_GB), ravenemu_destroy);
    if (!handle || !ravenemu_load(handle.get(), rom.data(), rom.size(), nullptr, 0)) return 30;
    std::vector<std::uint32_t> frame(160 * 144);
    const auto snapshot = [&] {
        std::vector<std::uint8_t> state(ravenemu_state_size(handle.get()));
        if (!ravenemu_save_state(handle.get(), state.data(), state.size())) state.clear();
        return state;
    };
    const auto save_only = [&](std::size_t size) {
        std::vector<std::uint8_t> state(size);
        if (!ravenemu_save_state(handle.get(), state.data(), state.size())) state.clear();
        return state;
    };
    const auto first = snapshot();
    if (first.empty() || save_only(first.size()) != first) return 31;
    if (!ravenemu_run_frame(handle.get(), frame.data(), frame.size())) return 32;
    const auto after_frame = save_only(first.size());
    if (after_frame.empty() || after_frame == first) return 33;
    if (!ravenemu_set_button(handle.get(), RAVENEMU_START, 1)) return 34;
    const auto after_button = save_only(first.size());
    if (after_button.empty() || after_button == after_frame) return 35;
    if (!ravenemu_load_state(handle.get(), first.data(), first.size()) ||
        save_only(first.size()) != first) return 36;
    return 0;
}

/* Réglages de cartouche GBA : refusés sur un autre système ou hors bornes,
 * appliqués au prochain chargement ou reset. */
int gba_cartridge_settings_test() {
    auto rom = ravenemu::testing::minimal_gba_rom();
    Handle game_boy(ravenemu_create(RAVENEMU_GB), ravenemu_destroy);
    if (!game_boy || ravenemu_set_gba_save_type(game_boy.get(), RAVENEMU_GBA_SAVE_SRAM) ||
        ravenemu_set_gba_rtc(game_boy.get(), RAVENEMU_GBA_RTC_ON) ||
        std::string(ravenemu_last_error()).empty()) return 40;
    if (ravenemu_set_gba_save_type(nullptr, RAVENEMU_GBA_SAVE_AUTO)) return 41;

    Handle handle(ravenemu_create(RAVENEMU_GBA), ravenemu_destroy);
    if (!handle || ravenemu_set_gba_save_type(handle.get(), -1) ||
        ravenemu_set_gba_save_type(handle.get(), RAVENEMU_GBA_SAVE_EEPROM_8K + 1) ||
        ravenemu_set_gba_rtc(handle.get(), RAVENEMU_GBA_RTC_ON + 1)) return 42;
    if (!ravenemu_set_gba_save_type(handle.get(), RAVENEMU_GBA_SAVE_FLASH_128K) ||
        !ravenemu_load(handle.get(), rom.data(), rom.size(), nullptr, 0) ||
        ravenemu_battery_size(handle.get()) != 128 * 1024) return 43;
    // Sans signature, la détection ne trouve aucune sauvegarde.
    if (!ravenemu_set_gba_save_type(handle.get(), RAVENEMU_GBA_SAVE_AUTO) ||
        ravenemu_battery_size(handle.get()) != 128 * 1024 ||
        !ravenemu_reset(handle.get()) || ravenemu_battery_size(handle.get()) != 0) return 44;
    // L'horloge ajoute le port GPIO, donc son état, à la sauvegarde d'état.
    const auto without_rtc = ravenemu_state_size(handle.get());
    if (!ravenemu_set_gba_rtc(handle.get(), RAVENEMU_GBA_RTC_ON) || !ravenemu_reset(handle.get()) ||
        ravenemu_state_size(handle.get()) <= without_rtc) return 45;
    if (!ravenemu_set_gba_rtc(handle.get(), RAVENEMU_GBA_RTC_AUTO) || !ravenemu_reset(handle.get()) ||
        ravenemu_state_size(handle.get()) != without_rtc) return 46;
    return 0;
}

/* Drill Dozer (code V49) allume son moteur par la broche 3 du GPIO. */
int gba_rumble_test() {
    std::vector<std::uint8_t> rom(0x200);
    // LDR r0, =0x080000C4 ; MOV r1, #8 ; STRH r1, [r0, #2] ; STRH r1, [r0] ; B .
    const std::uint32_t program[]{0xe59f000cU, 0xe3a01008U, 0xe1c010b2U, 0xe1c010b0U, 0xeafffffeU, 0x080000c4U};
    for (std::size_t index = 0; index < std::size(program); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[index * 4U + byte] = static_cast<std::uint8_t>(program[index] >> (byte * 8U));
    rom[0xb2] = 0x96;
    std::vector<std::uint32_t> frame(240 * 160);
    for (const bool motor : {false, true}) {
        std::memcpy(rom.data() + 0xac, motor ? "V49E" : "AAAE", 4);
        Handle handle(ravenemu_create(RAVENEMU_GBA), ravenemu_destroy);
        if (!handle || !ravenemu_load(handle.get(), rom.data(), rom.size(), nullptr, 0) ||
            ravenemu_rumble_active(handle.get())) return 50;
        if (!ravenemu_run_frame(handle.get(), frame.data(), frame.size()) ||
            ravenemu_rumble_active(handle.get()) != (motor ? 1 : 0)) return 51;
    }
    return ravenemu_rumble_active(nullptr) ? 52 : 0;
}

/* Capteurs : reconnus par le jeu, et fournis avant même le chargement. Le
 * programme lance une mesure d'inclinaison (Yoshi, KYG) puis recopie l'octet
 * bas de X en IWRAM. */
int sensors_test() {
    auto rom = ravenemu::testing::minimal_gba_rom();
    const std::uint32_t program[]{0xe59f001cU, 0xe3a01055U, 0xe5c01000U, 0xe3a010aaU, 0xe5c01100U, 0xe59f200cU,
                                  0xe5d01200U, 0xe5c21000U, 0xeafffffcU, 0x0e008000U, 0x03000000U};
    for (std::size_t index = 0; index < std::size(program); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[index * 4U + byte] = static_cast<std::uint8_t>(program[index] >> (byte * 8U));
    struct Case { const char* code; int sensors; };
    for (const auto& item : {Case{"U3IE", RAVENEMU_SENSOR_SOLAR}, Case{"RZWE", RAVENEMU_SENSOR_ROTATION},
                             Case{"AAAE", 0}, Case{"KYGE", RAVENEMU_SENSOR_TILT}}) {
        std::memcpy(rom.data() + 0xac, item.code, 4);
        Handle handle(ravenemu_create(RAVENEMU_GBA), ravenemu_destroy);
        if (!handle || ravenemu_sensors(handle.get()) != 0) return 60;
        // Hors limites : ramené à la plage, jamais refusé.
        if (!ravenemu_set_solar_light(handle.get(), 999) || !ravenemu_set_rotation(handle.get(), -99999) ||
            !ravenemu_set_tilt(handle.get(), 40000, 0)) return 61;
        if (!ravenemu_load(handle.get(), rom.data(), rom.size(), nullptr, 0) ||
            ravenemu_sensors(handle.get()) != item.sensors) return 62;
    }
    // Dernier cas, Yoshi : inclinaison maximale à droite, 392h + DFh = 471h.
    Handle yoshi(ravenemu_create(RAVENEMU_GBA), ravenemu_destroy);
    std::vector<std::uint32_t> frame(240 * 160);
    std::uint8_t copied = 0;
    if (!yoshi || !ravenemu_set_tilt(yoshi.get(), 32767, 0) ||
        !ravenemu_load(yoshi.get(), rom.data(), rom.size(), nullptr, 0) ||
        !ravenemu_run_frame(yoshi.get(), frame.data(), frame.size()) ||
        ravenemu_read_achievement_memory(yoshi.get(), 0, &copied, 1) != 1 || copied != 0x71) return 63;

    // Accéléromètre MBC7 des cartouches Game Boy.
    auto game_boy_rom = ravenemu::testing::minimal_game_boy_rom();
    game_boy_rom[0x147] = 0x22;
    Handle kirby(ravenemu_create(RAVENEMU_GBC), ravenemu_destroy);
    if (!kirby || !ravenemu_load(kirby.get(), game_boy_rom.data(), game_boy_rom.size(), nullptr, 0) ||
        ravenemu_sensors(kirby.get()) != RAVENEMU_SENSOR_TILT || !ravenemu_set_tilt(kirby.get(), -32768, 32767))
        return 64;
    return ravenemu_sensors(nullptr) || ravenemu_set_solar_light(nullptr, 0) ||
        ravenemu_set_rotation(nullptr, 0) || ravenemu_set_tilt(nullptr, 0, 0) ? 65 : 0;
}

} // namespace

int main() {
    if (ravenemu_api_version() != RAVENEMU_API_VERSION ||
        std::string(ravenemu_version()) != RAVENEMU_VERSION) {
        std::cerr << "C API or library version mismatch\n";
        return 1;
    }
    if (const int failure = state_cache_test()) {
        std::cerr << "State cache failed (" << failure << "): " << ravenemu_last_error() << "\n";
        return failure;
    }
    if (const int failure = battery_tracking_test()) {
        std::cerr << "Battery tracking failed (" << failure << "): " << ravenemu_last_error() << "\n";
        return failure;
    }
    if (const int failure = gba_cartridge_settings_test()) {
        std::cerr << "GBA cartridge settings failed (" << failure << "): " << ravenemu_last_error() << "\n";
        return failure;
    }
    if (const int failure = gba_rumble_test()) {
        std::cerr << "GBA rumble failed (" << failure << "): " << ravenemu_last_error() << "\n";
        return failure;
    }
    if (const int failure = sensors_test()) {
        std::cerr << "Cartridge sensors failed (" << failure << "): " << ravenemu_last_error() << "\n";
        return failure;
    }
    if (ravenemu_create(99)) {
        std::cerr << "Invalid platform was accepted\n";
        return 1;
    }
    for (int system = RAVENEMU_GB; system <= RAVENEMU_GBA; ++system) {
        Handle handle(ravenemu_create(system), ravenemu_destroy);
        if (!handle) return 2;
        auto rom = system == RAVENEMU_GBA
            ? ravenemu::testing::minimal_gba_rom()
            : ravenemu::testing::minimal_game_boy_rom();
        if (system == RAVENEMU_GBC) rom[0x143] = 0x80;
        if (!ravenemu_load(handle.get(), rom.data(), rom.size(), nullptr, 0)) {
            std::cerr << ravenemu_last_error() << "\n";
            return 3;
        }
        const auto width = ravenemu_video_width(handle.get());
        const auto height = ravenemu_video_height(handle.get());
        if (width != (system == RAVENEMU_GBA ? 240 : 160) ||
            height != (system == RAVENEMU_GBA ? 160 : 144)) return 4;
        // Unmapped/OOB reads must report a short length. Each read is
        // observational: it cannot advance a frame or clock a mapper.
        std::uint8_t achievement[16]{};
        if (ravenemu_read_achievement_memory(
                handle.get(), 0xFFFFFFFFu, achievement, sizeof achievement) != 0)
            return 41;
        if (ravenemu_read_achievement_memory(
                handle.get(), 0x0000u, achievement, sizeof achievement) != sizeof achievement)
            return 42;
        if (ravenemu_read_achievement_memory(
                handle.get(), 0x0000u, nullptr, sizeof achievement) != 0)
            return 43;
        std::vector<std::uint32_t> frame(static_cast<size_t>(width * height));
        if (ravenemu_run_frame(handle.get(), frame.data(), frame.size() - 1)) return 5;
        if (!ravenemu_run_frame(handle.get(), frame.data(), frame.size())) {
            std::cerr << ravenemu_last_error() << "\n"; return 6;
        }
        if ((frame[0] & 0xFF000000u) != 0xFF000000u) return 7;
        if (!ravenemu_set_button(handle.get(), RAVENEMU_A, 1)) return 8;
        if (!ravenemu_set_button(handle.get(), RAVENEMU_A, 0)) return 9;
        std::vector<std::int16_t> audio(4096);
        ravenemu_read_audio(handle.get(), audio.data(), audio.size());
        if (ravenemu_audio_rate(handle.get()) <= 0) return 10;
        const auto size = ravenemu_state_size(handle.get());
        if (size == 0) return 11;
        std::vector<std::uint8_t> state(size);
        if (!ravenemu_save_state(handle.get(), state.data(), state.size())) return 12;
        if (!ravenemu_load_state(handle.get(), state.data(), state.size())) {
            std::cerr << ravenemu_last_error() << "\n"; return 13;
        }
        if (!ravenemu_reset(handle.get())) return 14;
    }
    std::cout << "Portable bridge GB/GBC/GBA smoke tests OK\n";
}
