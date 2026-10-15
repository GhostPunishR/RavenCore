#include <ravenemu/bridge.h>
#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gbc/core.hpp>

#include <array>
#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef RAVENEMU_VERSION
#error "RAVENEMU_VERSION comes from the CMake project version"
#endif

struct ravenemu_bridge {
    std::unique_ptr<ravenemu::Core> core;
    std::vector<std::int32_t> framebuffer;
    bool loaded = false;
    // État sérialisé par ravenemu_state_size : un frontend appelle presque
    // toujours save_state juste après, machine inchangée. Toute fonction qui
    // peut modifier la machine doit appeler machine_may_change().
    std::vector<std::uint8_t> state_cache;
    bool state_cache_valid = false;
};

namespace {
thread_local std::string error_message;

void fail(const char* message) noexcept {
    try { error_message = message; }
    catch (...) {}
}

template<typename F>
int attempt(F&& fn) noexcept {
    try {
        error_message.clear();
        fn();
        return 1;
    } catch (const std::exception& ex) {
        fail(ex.what());
    } catch (...) {
        fail("Unhandled native core error");
    }
    return 0;
}

void machine_may_change(ravenemu_bridge* handle) noexcept {
    if (handle) handle->state_cache_valid = false;
}

void require_gba(const ravenemu_bridge* handle) {
    if (!handle || !handle->core) throw std::invalid_argument("Null handle");
    if (handle->core->console() != ravenemu::Console::game_boy_advance) throw std::invalid_argument("Not a GBA handle");
}

void require_loaded(const ravenemu_bridge* handle) {
    if (!handle || !handle->core || !handle->loaded)
        throw std::runtime_error("No ROM loaded");
}

constexpr std::array<std::uint32_t, 4> dmg_palette = {
    0xFFE0F8D0u, 0xFF88C070u, 0xFF346856u, 0xFF081820u
};
} // namespace

extern "C" {

int ravenemu_api_version(void) { return RAVENEMU_API_VERSION; }
const char* ravenemu_version(void) { return RAVENEMU_VERSION; }

ravenemu_bridge* ravenemu_create(int system) {
    ravenemu_bridge* result = nullptr;
    attempt([&] {
        auto handle = std::make_unique<ravenemu_bridge>();
        switch (system) {
        case RAVENEMU_GB:
            handle->core = ravenemu::gb::make_core(ravenemu::gb::HardwareModel::dmg);
            break;
        case RAVENEMU_GBC:
            handle->core = ravenemu::gbc::make_core();
            break;
        case RAVENEMU_GBA:
            handle->core = ravenemu::make_gba_core(std::nullopt);
            break;
        default:
            throw std::invalid_argument("Invalid system");
        }
        handle->framebuffer.resize(handle->core->video_spec().pixel_count());
        result = handle.release();
    });
    return result;
}
void ravenemu_destroy(ravenemu_bridge* handle) { delete handle; }
const char* ravenemu_last_error(void) { return error_message.c_str(); }

int ravenemu_load(ravenemu_bridge* handle, const uint8_t* rom, size_t rom_size,
                      const uint8_t* battery, size_t battery_size) {
    machine_may_change(handle);
    return attempt([&] {
        if (!handle || !handle->core) throw std::invalid_argument("Null handle");
        if (!rom || !rom_size || (battery_size && !battery))
            throw std::invalid_argument("Invalid ROM or battery data");
        handle->loaded = false;
        handle->core->load_rom({rom, rom_size}, {battery, battery_size});
        handle->loaded = true;
    });
}
int ravenemu_reset(ravenemu_bridge* handle) {
    machine_may_change(handle);
    return attempt([&] { require_loaded(handle); handle->core->reset(); });
}
int ravenemu_set_button(ravenemu_bridge* handle, int button, int pressed) {
    machine_may_change(handle);
    return attempt([&] {
        require_loaded(handle);
        if (button < RAVENEMU_UP || button > RAVENEMU_R)
            throw std::invalid_argument("Invalid button");
        handle->core->set_button(static_cast<ravenemu::Button>(button), pressed != 0);
    });
}
int ravenemu_video_width(const ravenemu_bridge* handle) {
    return handle && handle->core ? handle->core->video_spec().width : 0;
}
int ravenemu_video_height(const ravenemu_bridge* handle) {
    return handle && handle->core ? handle->core->video_spec().height : 0;
}
double ravenemu_video_fps(const ravenemu_bridge* handle) {
    return handle && handle->core ? handle->core->video_spec().refresh_rate_hz : 0;
}
int ravenemu_audio_rate(const ravenemu_bridge* handle) {
    return handle && handle->core ? handle->core->audio_spec().sample_rate_hz : 0;
}
int ravenemu_run_frame(ravenemu_bridge* handle, uint32_t* argb, size_t pixel_capacity) {
    machine_may_change(handle);
    return attempt([&] {
        require_loaded(handle);
        if (!argb || pixel_capacity < handle->framebuffer.size())
            throw std::invalid_argument("Insufficient video buffer");
        handle->core->run_frame(handle->framebuffer, true);
        const bool indexed = handle->core->framebuffer_format() == ravenemu::FramebufferFormat::indexed_4;
        for (size_t i = 0; i < handle->framebuffer.size(); ++i) {
            const auto pixel = static_cast<uint32_t>(handle->framebuffer[i]);
            argb[i] = indexed ? dmg_palette[pixel & 3u] : (0xFF000000u | (pixel & 0x00FFFFFFu));
        }
    });
}
size_t ravenemu_read_audio(ravenemu_bridge* handle,
                              int16_t* output, size_t sample_capacity) {
    machine_may_change(handle);
    size_t count = 0;
    attempt([&] {
        require_loaded(handle);
        if (sample_capacity && !output) throw std::invalid_argument("Null audio buffer");
        count = handle->core->read_audio({output, sample_capacity});
    });
    return count;
}
size_t ravenemu_read_achievement_memory(const ravenemu_bridge* handle,
                                           uint32_t address, uint8_t* output,
                                           size_t capacity) {
    size_t read = 0;
    attempt([&] {
        require_loaded(handle);
        if (capacity > 0x58000 || (capacity && !output))
            throw std::invalid_argument("Invalid achievement memory buffer");
        if (!capacity) return;
        read = handle->core->read_achievement_memory(address, {output, capacity});
        if (read > capacity) throw std::runtime_error("Invalid achievement read count");
    });
    return read;
}

size_t ravenemu_state_size(ravenemu_bridge* handle) {
    size_t count = 0;
    attempt([&] {
        require_loaded(handle);
        handle->state_cache = handle->core->save_state();
        handle->state_cache_valid = true;
        count = handle->state_cache.size();
    });
    return count;
}
int ravenemu_save_state(ravenemu_bridge* handle, uint8_t* output, size_t capacity) {
    return attempt([&] {
        require_loaded(handle);
        if (!handle->state_cache_valid) {
            handle->state_cache = handle->core->save_state();
            handle->state_cache_valid = true;
        }
        const auto& state = handle->state_cache;
        if (!output || capacity < state.size()) throw std::invalid_argument("Insufficient state buffer");
        std::copy(state.begin(), state.end(), output);
    });
}
int ravenemu_load_state(ravenemu_bridge* handle, const uint8_t* state, size_t size) {
    machine_may_change(handle);
    return attempt([&] {
        require_loaded(handle);
        if (!state || !size) throw std::invalid_argument("Invalid save state");
        handle->core->load_state({state, size});
    });
}
size_t ravenemu_battery_size(ravenemu_bridge* handle) {
    machine_may_change(handle);
    size_t count = 0;
    attempt([&] {
        require_loaded(handle);
        const auto battery = handle->core->snapshot_battery_ram();
        count = battery ? battery->data.size() : 0;
    });
    return count;
}
int ravenemu_copy_battery(ravenemu_bridge* handle, uint8_t* output, size_t capacity) {
    return ravenemu_copy_battery_generation(handle, output, capacity, nullptr);
}
int ravenemu_battery_dirty(const ravenemu_bridge* handle) {
    return handle && handle->core && handle->loaded && handle->core->battery_ram_dirty() ? 1 : 0;
}
int ravenemu_copy_battery_generation(ravenemu_bridge* handle, uint8_t* output,
                                        size_t capacity, uint64_t* generation) {
    machine_may_change(handle);
    return attempt([&] {
        require_loaded(handle);
        const auto battery = handle->core->snapshot_battery_ram();
        if (!battery) throw std::runtime_error("No battery RAM");
        if (!output || capacity < battery->data.size())
            throw std::invalid_argument("Insufficient battery buffer");
        std::copy(battery->data.begin(), battery->data.end(), output);
        if (generation) *generation = battery->generation;
    });
}
int ravenemu_acknowledge_battery(ravenemu_bridge* handle, uint64_t generation) {
    machine_may_change(handle);
    return attempt([&] {
        require_loaded(handle);
        handle->core->acknowledge_battery_ram_saved(generation);
    });
}
int ravenemu_set_gba_save_type(ravenemu_bridge* handle, int save_type) {
    return attempt([&] {
        require_gba(handle);
        static constexpr std::array<std::optional<ravenemu::GbaSaveType>, 7> types{
            std::nullopt, ravenemu::GbaSaveType::none, ravenemu::GbaSaveType::sram,
            ravenemu::GbaSaveType::flash_64k, ravenemu::GbaSaveType::flash_128k,
            ravenemu::GbaSaveType::eeprom_512, ravenemu::GbaSaveType::eeprom_8k};
        if (save_type < 0 || save_type >= static_cast<int>(types.size())) throw std::invalid_argument("Invalid GBA save type");
        handle->core->set_gba_forced_save_type(types[static_cast<std::size_t>(save_type)]);
    });
}
int ravenemu_set_gba_rtc(ravenemu_bridge* handle, int rtc) {
    return attempt([&] {
        require_gba(handle);
        if (rtc < RAVENEMU_GBA_RTC_AUTO || rtc > RAVENEMU_GBA_RTC_ON) throw std::invalid_argument("Invalid GBA RTC mode");
        handle->core->set_gba_forced_rtc(rtc == RAVENEMU_GBA_RTC_AUTO ? std::nullopt
                                                                         : std::optional<bool>{rtc == RAVENEMU_GBA_RTC_ON});
    });
}
int ravenemu_rumble_active(const ravenemu_bridge* handle) {
    return handle && handle->core && handle->loaded && handle->core->rumble_active() ? 1 : 0;
}
int ravenemu_sensors(const ravenemu_bridge* handle) {
    if (!handle || !handle->core || !handle->loaded) return 0;
    const auto sensors = handle->core->cartridge_sensors();
    return (sensors.solar ? RAVENEMU_SENSOR_SOLAR : 0) | (sensors.rotation ? RAVENEMU_SENSOR_ROTATION : 0) |
           (sensors.tilt ? RAVENEMU_SENSOR_TILT : 0);
}
int ravenemu_set_solar_light(ravenemu_bridge* handle, int level) {
    if (!handle || !handle->core) return 0;
    machine_may_change(handle);
    handle->core->set_solar_light(std::clamp(level, 0, 255));
    return 1;
}
int ravenemu_set_rotation(ravenemu_bridge* handle, int speed) {
    if (!handle || !handle->core) return 0;
    machine_may_change(handle);
    handle->core->set_rotation_speed(std::clamp(speed, -32768, 32767));
    return 1;
}
int ravenemu_set_tilt(ravenemu_bridge* handle, int x, int y) {
    if (!handle || !handle->core) return 0;
    machine_may_change(handle);
    x = std::clamp(x, -32768, 32767);
    y = std::clamp(y, -32768, 32767);
    if (handle->core->console() == ravenemu::Console::game_boy_advance) {
        handle->core->set_gba_tilt(x, y);
    } else {
        // MBC7 : 81D0h au repos, environ 70h par g ; X baisse quand le côté
        // droit descend, Y quand le haut s'éloigne (comme l'adaptateur Libretro).
        constexpr int units_per_g = 0x70;
        handle->core->set_game_boy_acceleration(-x * units_per_g / 0x7fff, -y * units_per_g / 0x7fff);
    }
    return 1;
}
} // extern "C"
