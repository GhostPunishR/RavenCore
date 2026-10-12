#include <ravenemu/bridge.h>
#include <synthetic_roms.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Handle = std::unique_ptr<ravenemu_v3_bridge, decltype(&ravenemu_v3_destroy)>;

/* The minimal GBA program stores 0x5A into SRAM once, then loops. */
int battery_tracking_test() {
    auto rom = ravenemu::testing::minimal_gba_rom();
    constexpr char marker[] = "SRAM_V";
    std::memcpy(rom.data() + 0x100, marker, sizeof marker - 1);
    Handle handle(ravenemu_v3_create(RAVENEMU_V3_GBA), ravenemu_v3_destroy);
    if (!handle || !ravenemu_v3_load(handle.get(), rom.data(), rom.size(), nullptr, 0)) return 20;
    if (ravenemu_v3_battery_dirty(handle.get())) return 21;

    std::vector<std::uint32_t> frame(240 * 160);
    if (!ravenemu_v3_run_frame(handle.get(), frame.data(), frame.size())) return 22;
    if (!ravenemu_v3_battery_dirty(handle.get())) return 23;

    std::vector<std::uint8_t> battery(ravenemu_v3_battery_size(handle.get()));
    std::uint64_t generation = 0;
    if (battery.empty() ||
        !ravenemu_v3_copy_battery_generation(handle.get(), battery.data(), battery.size(), &generation) ||
        battery[0] != 0x5a) return 24;
    // Acknowledging another generation must not hide the pending write.
    if (!ravenemu_v3_acknowledge_battery(handle.get(), generation + 1) ||
        !ravenemu_v3_battery_dirty(handle.get())) return 25;
    if (!ravenemu_v3_acknowledge_battery(handle.get(), generation) ||
        ravenemu_v3_battery_dirty(handle.get())) return 26;
    if (!ravenemu_v3_run_frame(handle.get(), frame.data(), frame.size()) ||
        ravenemu_v3_battery_dirty(handle.get())) return 27;

    if (ravenemu_v3_battery_dirty(nullptr) || ravenemu_v3_acknowledge_battery(nullptr, 0)) return 28;
    return 0;
}

/* state_size met l'état en cache pour save_state : toute modification de la
 * machine entre les deux doit le rendre caduc, sinon l'état serait périmé. */
int state_cache_test() {
    auto rom = ravenemu::testing::minimal_game_boy_rom();
    Handle handle(ravenemu_v3_create(RAVENEMU_V3_GB), ravenemu_v3_destroy);
    if (!handle || !ravenemu_v3_load(handle.get(), rom.data(), rom.size(), nullptr, 0)) return 30;
    std::vector<std::uint32_t> frame(160 * 144);
    const auto snapshot = [&] {
        std::vector<std::uint8_t> state(ravenemu_v3_state_size(handle.get()));
        if (!ravenemu_v3_save_state(handle.get(), state.data(), state.size())) state.clear();
        return state;
    };
    const auto save_only = [&](std::size_t size) {
        std::vector<std::uint8_t> state(size);
        if (!ravenemu_v3_save_state(handle.get(), state.data(), state.size())) state.clear();
        return state;
    };
    const auto first = snapshot();
    if (first.empty() || save_only(first.size()) != first) return 31;
    if (!ravenemu_v3_run_frame(handle.get(), frame.data(), frame.size())) return 32;
    const auto after_frame = save_only(first.size());
    if (after_frame.empty() || after_frame == first) return 33;
    if (!ravenemu_v3_set_button(handle.get(), RAVENEMU_V3_START, 1)) return 34;
    const auto after_button = save_only(first.size());
    if (after_button.empty() || after_button == after_frame) return 35;
    if (!ravenemu_v3_load_state(handle.get(), first.data(), first.size()) ||
        save_only(first.size()) != first) return 36;
    return 0;
}

} // namespace

int main() {
    if (ravenemu_v3_api_version() != RAVENEMU_V3_API_VERSION ||
        std::string(ravenemu_v3_version()) != RAVENEMU_VERSION) {
        std::cerr << "C API or library version mismatch\n";
        return 1;
    }
    if (const int failure = state_cache_test()) {
        std::cerr << "State cache failed (" << failure << "): " << ravenemu_v3_last_error() << "\n";
        return failure;
    }
    if (const int failure = battery_tracking_test()) {
        std::cerr << "Battery tracking failed (" << failure << "): " << ravenemu_v3_last_error() << "\n";
        return failure;
    }
    if (ravenemu_v3_create(99)) {
        std::cerr << "Invalid platform was accepted\n";
        return 1;
    }
    for (int system = RAVENEMU_V3_GB; system <= RAVENEMU_V3_GBA; ++system) {
        Handle handle(ravenemu_v3_create(system), ravenemu_v3_destroy);
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
        // Unmapped/OOB reads must report a short length. Each read is
        // observational: it cannot advance a frame or clock a mapper.
        std::uint8_t achievement[16]{};
        if (ravenemu_v3_read_achievement_memory(
                handle.get(), 0xFFFFFFFFu, achievement, sizeof achievement) != 0)
            return 41;
        if (ravenemu_v3_read_achievement_memory(
                handle.get(), 0x0000u, achievement, sizeof achievement) != sizeof achievement)
            return 42;
        if (ravenemu_v3_read_achievement_memory(
                handle.get(), 0x0000u, nullptr, sizeof achievement) != 0)
            return 43;
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
