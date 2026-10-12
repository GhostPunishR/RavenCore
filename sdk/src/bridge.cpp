#include <ravenemu/bridge.h>
#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gbc/core.hpp>

#include <array>
#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct ravenemu_v3_bridge {
    std::unique_ptr<ravenemu::Core> core;
    std::vector<std::int32_t> framebuffer;
    bool loaded = false;
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

void require_loaded(const ravenemu_v3_bridge* handle) {
    if (!handle || !handle->core || !handle->loaded)
        throw std::runtime_error("No ROM loaded");
}

constexpr std::array<std::uint32_t, 4> dmg_palette = {
    0xFFE0F8D0u, 0xFF88C070u, 0xFF346856u, 0xFF081820u
};
} // namespace

extern "C" {

ravenemu_v3_bridge* ravenemu_v3_create(int system) {
    ravenemu_v3_bridge* result = nullptr;
    attempt([&] {
        auto handle = std::make_unique<ravenemu_v3_bridge>();
        switch (system) {
        case RAVENEMU_V3_GB:
            handle->core = ravenemu::gb::make_core(ravenemu::gb::HardwareModel::dmg);
            break;
        case RAVENEMU_V3_GBC:
            handle->core = ravenemu::gbc::make_core();
            break;
        case RAVENEMU_V3_GBA:
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
void ravenemu_v3_destroy(ravenemu_v3_bridge* handle) { delete handle; }
const char* ravenemu_v3_last_error(void) { return error_message.c_str(); }

int ravenemu_v3_load(ravenemu_v3_bridge* handle, const uint8_t* rom, size_t rom_size,
                      const uint8_t* battery, size_t battery_size) {
    return attempt([&] {
        if (!handle || !handle->core) throw std::invalid_argument("Null handle");
        if (!rom || !rom_size || (battery_size && !battery))
            throw std::invalid_argument("Invalid ROM or battery data");
        handle->loaded = false;
        handle->core->load_rom({rom, rom_size}, {battery, battery_size});
        handle->loaded = true;
    });
}
int ravenemu_v3_reset(ravenemu_v3_bridge* handle) {
    return attempt([&] { require_loaded(handle); handle->core->reset(); });
}
int ravenemu_v3_set_button(ravenemu_v3_bridge* handle, int button, int pressed) {
    return attempt([&] {
        require_loaded(handle);
        if (button < RAVENEMU_V3_UP || button > RAVENEMU_V3_R)
            throw std::invalid_argument("Invalid button");
        handle->core->set_button(static_cast<ravenemu::Button>(button), pressed != 0);
    });
}
int ravenemu_v3_video_width(const ravenemu_v3_bridge* handle) {
    return handle && handle->core ? handle->core->video_spec().width : 0;
}
int ravenemu_v3_video_height(const ravenemu_v3_bridge* handle) {
    return handle && handle->core ? handle->core->video_spec().height : 0;
}
double ravenemu_v3_video_fps(const ravenemu_v3_bridge* handle) {
    return handle && handle->core ? handle->core->video_spec().refresh_rate_hz : 0;
}
int ravenemu_v3_audio_rate(const ravenemu_v3_bridge* handle) {
    return handle && handle->core ? handle->core->audio_spec().sample_rate_hz : 0;
}
int ravenemu_v3_run_frame(ravenemu_v3_bridge* handle, uint32_t* argb, size_t pixel_capacity) {
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
size_t ravenemu_v3_read_audio(ravenemu_v3_bridge* handle,
                              int16_t* output, size_t sample_capacity) {
    size_t count = 0;
    attempt([&] {
        require_loaded(handle);
        if (sample_capacity && !output) throw std::invalid_argument("Null audio buffer");
        count = handle->core->read_audio({output, sample_capacity});
    });
    return count;
}
size_t ravenemu_v3_state_size(ravenemu_v3_bridge* handle) {
    size_t count = 0;
    attempt([&] { require_loaded(handle); count = handle->core->save_state().size(); });
    return count;
}
int ravenemu_v3_save_state(ravenemu_v3_bridge* handle, uint8_t* output, size_t capacity) {
    return attempt([&] {
        require_loaded(handle);
        const auto state = handle->core->save_state();
        if (!output || capacity < state.size()) throw std::invalid_argument("Insufficient state buffer");
        std::copy(state.begin(), state.end(), output);
    });
}
int ravenemu_v3_load_state(ravenemu_v3_bridge* handle, const uint8_t* state, size_t size) {
    return attempt([&] {
        require_loaded(handle);
        if (!state || !size) throw std::invalid_argument("Invalid save state");
        handle->core->load_state({state, size});
    });
}
size_t ravenemu_v3_battery_size(ravenemu_v3_bridge* handle) {
    size_t count = 0;
    attempt([&] {
        require_loaded(handle);
        const auto battery = handle->core->snapshot_battery_ram();
        count = battery ? battery->data.size() : 0;
    });
    return count;
}
int ravenemu_v3_copy_battery(ravenemu_v3_bridge* handle, uint8_t* output, size_t capacity) {
    return attempt([&] {
        require_loaded(handle);
        const auto battery = handle->core->snapshot_battery_ram();
        if (!battery) throw std::runtime_error("No battery RAM");
        if (!output || capacity < battery->data.size())
            throw std::invalid_argument("Insufficient battery buffer");
        std::copy(battery->data.begin(), battery->data.end(), output);
    });
}
} // extern "C"
