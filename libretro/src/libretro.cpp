#include <libretro.h>
#include <ravenemu/core.hpp>
#if defined(RAVENEMU_LIBRETRO_GB)
#include <ravenemu/gb/core.hpp>
#elif defined(RAVENEMU_LIBRETRO_GBC)
#include <ravenemu/gbc/core.hpp>
#elif defined(RAVENEMU_LIBRETRO_GBA)
#include <ravenemu/gba/core.hpp>
#else
#error "Choose exactly one Libretro core"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
retro_environment_t env_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_t sample_cb = nullptr;
retro_audio_sample_batch_t batch_cb = nullptr;
retro_input_poll_t poll_cb = nullptr;
retro_input_state_t input_cb = nullptr;

std::unique_ptr<ravenemu::Core> core;
std::vector<std::uint8_t> rom;
std::vector<std::uint8_t> save_ram, initial_save_ram;
std::vector<std::int32_t> native_frame;
std::vector<std::uint32_t> output_frame;
std::size_t state_capacity = 0;
bool pending_save_import = false;

#if defined(RAVENEMU_LIBRETRO_GB)
constexpr const char* label = "RavenGB";
constexpr const char* extensions = "gb|sgb";
constexpr unsigned width = 160, height = 144;
constexpr double fps = 4'194'304.0 / 70'224.0;
std::unique_ptr<ravenemu::Core> new_core() {
    return ravenemu::gb::make_core(ravenemu::gb::HardwareModel::dmg);
}
#elif defined(RAVENEMU_LIBRETRO_GBC)
constexpr const char* label = "RavenGBC";
constexpr const char* extensions = "gbc|gb";
constexpr unsigned width = 160, height = 144;
constexpr double fps = 4'194'304.0 / 70'224.0;
std::unique_ptr<ravenemu::Core> new_core() { return ravenemu::gbc::make_core(); }
#else
constexpr const char* label = "RavenGBA (experimental)";
constexpr const char* extensions = "gba";
constexpr unsigned width = 240, height = 160;
constexpr double fps = 16'777'216.0 / 280'896.0;
std::unique_ptr<ravenemu::Core> new_core() { return ravenemu::make_gba_core(std::nullopt); }
#endif

constexpr std::array<std::uint32_t, 4> dmg_shades{
    0x00e0f8d0, 0x0088c070, 0x00346856, 0x00081820
};
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

void update_save_ram() {
    if (!core || save_ram.empty()) return;
    auto state = core->snapshot_battery_ram();
    if (state && state->data.size() == save_ram.size())
        std::copy(state->data.begin(), state->data.end(), save_ram.begin());
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
void clear_game() {
    core.reset();
    rom.clear();
    save_ram.clear();
    initial_save_ram.clear();
    native_frame.clear();
    output_frame.clear();
    state_capacity = 0;
    pending_save_import = false;
}
void refresh_video() {
    if (!core || !video_cb) return;
    const auto spec = core->video_spec();
    const bool indexed = core->framebuffer_format() == ravenemu::FramebufferFormat::indexed_4;
    for (std::size_t i = 0; i < native_frame.size(); ++i) {
        const auto pixel = static_cast<std::uint32_t>(native_frame[i]);
        output_frame[i] = indexed ? dmg_shades[pixel & 3U] : (pixel & 0x00ffffffU);
    }
    video_cb(output_frame.data(), static_cast<unsigned>(spec.width),
             static_cast<unsigned>(spec.height),
             static_cast<std::size_t>(spec.width) * sizeof(std::uint32_t));
}
void refresh_audio() {
    if (!core) return;
    std::array<std::int16_t, 4096> pcm{};
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto count = core->read_audio(pcm);
        if (count < 2) break;
        const auto frames = count / 2;
        if (batch_cb) batch_cb(pcm.data(), frames);
        else if (sample_cb) for (std::size_t i = 0; i < frames; ++i)
            sample_cb(pcm[2 * i], pcm[2 * i + 1]);
        if (count < pcm.size()) break;
    }
}
} // namespace

extern "C" {

void retro_set_environment(retro_environment_t callback) {
    env_cb = callback;
    // No direct writable system memory mappings yet: never falsely claim
    // RetroAchievements (including Hardcore) support.
    bool achievements = false;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
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
    info->library_version = "0.1.0";
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
    if (!game || !game->data || !game->size || !env_cb) return false;
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) return false;
    try {
        const auto* bytes = static_cast<const std::uint8_t*>(game->data);
        rom.assign(bytes, bytes + game->size);
        auto machine = new_core();
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
        return true;
    } catch (const std::exception&) { clear_game(); return false; }
}
bool retro_load_game_special(unsigned, const struct retro_game_info*, std::size_t) { return false; }
void retro_unload_game(void) { clear_game(); }
void retro_reset(void) {
    if (!core) return;
    try { import_save_ram_once(); core->reset(); update_save_ram(); }
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
        core->run_frame(native_frame, true);
        update_save_ram();
        refresh_video();
        refresh_audio();
    } catch (const std::exception&) {
        // Never propagate an exception through Libretro's C ABI.
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
        return true;
    } catch (const std::exception&) { return false; }
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char*) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
// Battery SRAM is the only exposed mutable memory. The internal core has no
// writable system RAM ABI yet, so do NOT provide a misleading system RAM view.
void* retro_get_memory_data(unsigned id) {
    return id == RETRO_MEMORY_SAVE_RAM && !save_ram.empty() ? save_ram.data() : nullptr;
}
std::size_t retro_get_memory_size(unsigned id) {
    return id == RETRO_MEMORY_SAVE_RAM ? save_ram.size() : 0;
}
} // extern "C"
