#include <ravenemu/bridge.h>
#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gbc/core.hpp>
#include <ravenemu/remote_link.hpp>

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
    int system = -1;  // valeur de ravenemu_system donnée à ravenemu_create
    std::vector<std::int32_t> framebuffer;
    bool loaded = false;
    // État sérialisé par ravenemu_state_size : un frontend appelle presque
    // toujours save_state juste après, machine inchangée. Toute fonction qui
    // peut modifier la machine doit appeler machine_may_change().
    std::vector<std::uint8_t> state_cache;
    bool state_cache_valid = false;
    ravenemu_link* link = nullptr;
    ravenemu_remote_link* remote = nullptr;
};

struct ravenemu_link {
    std::vector<ravenemu_bridge*> handles;
    std::unique_ptr<ravenemu::LinkCable> cable;
};

struct ravenemu_remote_link {
    ravenemu_bridge* handle = nullptr;
    std::unique_ptr<ravenemu::RemoteLink> link;
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

// Console d'une valeur de ravenemu_system, pour décoder ses codes de triche.
std::optional<ravenemu::Console> console_of(int system) noexcept {
    switch (system) {
    case RAVENEMU_GB: case RAVENEMU_GBC: case RAVENEMU_GBC_ON_GBA: return ravenemu::Console::game_boy;
    case RAVENEMU_GBA: return ravenemu::Console::game_boy_advance;
    default: return std::nullopt;
    }
}

void require_loaded(const ravenemu_bridge* handle) {
    if (!handle || !handle->core || !handle->loaded)
        throw std::runtime_error("No ROM loaded");
}

constexpr std::array<std::uint32_t, 4> dmg_palette = {
    0xFFE0F8D0u, 0xFF88C070u, 0xFF346856u, 0xFF081820u
};

void copy_frame(const ravenemu_bridge& handle, uint32_t* argb) noexcept {
    const bool indexed = handle.core->framebuffer_format() == ravenemu::FramebufferFormat::indexed_4;
    for (size_t i = 0; i < handle.framebuffer.size(); ++i) {
        const auto pixel = static_cast<uint32_t>(handle.framebuffer[i]);
        argb[i] = indexed ? dmg_palette[pixel & 3u] : (0xFF000000u | (pixel & 0x00FFFFFFu));
    }
}

// Débranche le câble de toutes ses consoles ; la structure reste à libérer.
void unplug(ravenemu_link& link) noexcept {
    link.cable.reset();
    for (auto* handle : link.handles)
        if (handle) handle->link = nullptr;
    link.handles.clear();
}
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
        case RAVENEMU_GBC_ON_GBA:
            handle->core = ravenemu::gb::make_core(ravenemu::gb::HardwareModel::agb);
            break;
        default:
            throw std::invalid_argument("Invalid system");
        }
        handle->framebuffer.resize(handle->core->video_spec().pixel_count());
        handle->system = system;
        result = handle.release();
    });
    return result;
}
void ravenemu_destroy(ravenemu_bridge* handle) {
    if (handle && handle->link) unplug(*handle->link);
    if (handle && handle->remote) {
        static_cast<void>(handle->core->connect_remote_link(nullptr, 0));
        handle->remote->handle = nullptr;
    }
    delete handle;
}
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
        if (handle->link) throw std::logic_error("Linked handle: use ravenemu_link_run_frame");
        handle->core->run_frame(handle->framebuffer, true);
        copy_frame(*handle, argb);
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
ravenemu_link* ravenemu_link_create(ravenemu_bridge* const* handles, size_t count) {
    ravenemu_link* result = nullptr;
    attempt([&] {
        if (!handles || count < 2) throw std::invalid_argument("A link cable needs at least two handles");
        std::vector<ravenemu::Core*> cores;
        for (size_t i = 0; i < count; ++i) {
            if (!handles[i] || !handles[i]->core) throw std::invalid_argument("Null handle");
            if (handles[i]->link || handles[i]->remote) throw std::invalid_argument("Handle already linked");
            cores.push_back(handles[i]->core.get());
        }
        auto link = std::make_unique<ravenemu_link>();
        link->cable = ravenemu::connect_link_cable(cores);
        link->handles.assign(handles, handles + count);
        for (auto* handle : link->handles) {
            machine_may_change(handle);
            handle->link = link.get();
        }
        result = link.release();
    });
    return result;
}
void ravenemu_link_destroy(ravenemu_link* link) {
    if (!link) return;
    for (auto* handle : link->handles) machine_may_change(handle);
    unplug(*link);
    delete link;
}
int ravenemu_link_run_frame(ravenemu_link* link, uint32_t* const* argb, size_t pixel_capacity) {
    return attempt([&] {
        if (!link || !link->cable) throw std::logic_error("Link cable unplugged");
        if (!argb) throw std::invalid_argument("Insufficient video buffer");
        std::vector<std::span<std::int32_t>> frames;
        for (size_t i = 0; i < link->handles.size(); ++i) {
            auto* handle = link->handles[i];
            require_loaded(handle);
            if (!argb[i] || pixel_capacity < handle->framebuffer.size())
                throw std::invalid_argument("Insufficient video buffer");
            machine_may_change(handle);
            frames.emplace_back(handle->framebuffer);
        }
        link->cable->run_frame(frames, true);
        for (size_t i = 0; i < link->handles.size(); ++i) copy_frame(*link->handles[i], argb[i]);
    });
}
ravenemu_remote_link* ravenemu_remote_link_create(ravenemu_bridge* handle, int player,
                                                  ravenemu_remote_send send, void* user) {
    ravenemu_remote_link* result = nullptr;
    attempt([&] {
        if (!handle || !handle->core) throw std::invalid_argument("Null handle");
        if (!send) throw std::invalid_argument("Null send callback");
        if (handle->link || handle->remote) throw std::invalid_argument("Handle already linked");
        if (player < 0 || player > 1) throw std::invalid_argument("Invalid player");
        const auto family = handle->core->console() == ravenemu::Console::game_boy_advance
            ? ravenemu::RemoteLink::Family::game_boy_advance : ravenemu::RemoteLink::Family::game_boy;
        auto remote = std::make_unique<ravenemu_remote_link>();
        remote->link = std::make_unique<ravenemu::RemoteLink>(family, [send, user](std::span<const std::uint8_t> bytes) {
            send(user, bytes.data(), bytes.size());
        });
        if (!handle->core->connect_remote_link(remote->link.get(), player))
            throw std::invalid_argument("This console takes no remote link cable");
        machine_may_change(handle);
        remote->handle = handle;
        handle->remote = remote.get();
        result = remote.release();
    });
    return result;
}
void ravenemu_remote_link_destroy(ravenemu_remote_link* link) {
    if (!link) return;
    link->link->set_connected(false);
    if (auto* handle = link->handle) {
        machine_may_change(handle);
        static_cast<void>(handle->core->connect_remote_link(nullptr, 0));
        handle->remote = nullptr;
    }
    delete link;
}
int ravenemu_remote_link_receive(ravenemu_remote_link* link, const uint8_t* message, size_t size) {
    if (!link || !message) return 0;
    try {
        return link->link->receive(std::span<const std::uint8_t>(message, size)) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}
void ravenemu_remote_link_set_connected(ravenemu_remote_link* link, int connected) {
    if (!link) return;
    try {
        link->link->set_connected(connected != 0);
    } catch (...) {
    }
}
int ravenemu_remote_link_state(const ravenemu_remote_link* link) {
    if (!link) return RAVENEMU_REMOTE_UNPLUGGED;
    try {
        switch (link->link->state()) {
        case ravenemu::RemoteLink::State::unplugged: return RAVENEMU_REMOTE_UNPLUGGED;
        case ravenemu::RemoteLink::State::waiting: return RAVENEMU_REMOTE_WAITING;
        case ravenemu::RemoteLink::State::plugged: return RAVENEMU_REMOTE_PLUGGED;
        case ravenemu::RemoteLink::State::incompatible: return RAVENEMU_REMOTE_INCOMPATIBLE;
        }
    } catch (...) {
    }
    return RAVENEMU_REMOTE_UNPLUGGED;
}
int ravenemu_remote_link_service(ravenemu_remote_link* link) {
    return attempt([&] {
        if (!link || !link->handle) throw std::invalid_argument("Remote link cable without console");
        if (!link->handle->loaded) return;
        machine_may_change(link->handle);
        link->handle->core->service_remote_link();
    });
}
int ravenemu_set_gbc_colors(ravenemu_bridge* handle, int colors) {
    if (!handle || !handle->core || (handle->system != RAVENEMU_GBC && handle->system != RAVENEMU_GBC_ON_GBA)) return 0;
    if (colors != RAVENEMU_GBC_COLORS_AUTO && colors != RAVENEMU_GBC_COLORS_GRAY) return 0;
    machine_may_change(handle);
    handle->core->set_gbc_compatibility_colors(colors == RAVENEMU_GBC_COLORS_GRAY
                                                   ? ravenemu::GbcCompatibilityColors::grayscale
                                                   : ravenemu::GbcCompatibilityColors::automatic);
    return 1;
}
int ravenemu_check_cheat(int system, const char* code) {
    return attempt([&] {
        const auto console = console_of(system);
        if (!console) throw std::invalid_argument("Invalid system");
        if (!code) throw std::invalid_argument("Null cheat code");
        static_cast<void>(ravenemu::parse_cheat(*console, code));
    });
}
int ravenemu_set_cheats(ravenemu_bridge* handle, const char* const* codes, size_t count) {
    return attempt([&] {
        if (!handle || !handle->core) throw std::invalid_argument("Null handle");
        if (count > 0 && !codes) throw std::invalid_argument("Null cheat list");
        std::vector<ravenemu::Cheat> cheats;
        cheats.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            if (!codes[index]) throw std::invalid_argument("Null cheat code");
            try {
                cheats.push_back(ravenemu::parse_cheat(handle->core->console(), codes[index]));
            } catch (const ravenemu::CheatError& error) {
                throw std::runtime_error("code de triche " + std::to_string(index + 1) + " : " + error.what());
            }
        }
        handle->core->set_cheats(cheats);
    });
}
int ravenemu_link_wanted(const ravenemu_bridge* handle) {
    return handle && handle->core && handle->loaded && handle->core->link_wanted() ? 1 : 0;
}
} // extern "C"
