#include <ravenemu/nds/core.hpp>
#include <ravenemu/nds/machine.hpp>

#include <algorithm>
#include <bit>

namespace ravenemu::nds {
namespace {
class NdsCore final : public FrontendCore {
public:
    explicit NdsCore(const CoreOptions& options) : backup_type_(options.backup_type), machine_(std::make_unique<Machine>()) {
        if (!options.arm9_bios.empty()) machine_->memory().load_bios(Processor::arm9,options.arm9_bios);
        if (!options.arm7_bios.empty()) machine_->memory().load_bios(Processor::arm7,options.arm7_bios);
        if (!options.firmware.empty()) machine_->load_firmware(options.firmware);
    }
    Console console() const noexcept override { return Console::nintendo_ds; }
    VideoSpec video_spec() const noexcept override { return {256,384,static_cast<double>(timer_clock_hz) / LcdTiming::frame_cycles}; }
    AudioSpec audio_spec() const noexcept override { return {32768,2}; }
    FramebufferFormat framebuffer_format() const noexcept override { return FramebufferFormat::argb_8888; }
    bool supports_video_frame_skipping() const noexcept override { return true; }
    bool supports_save_states() const noexcept override { return false; }

    void load_rom(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> battery) override {
        auto prepared = std::make_unique<Machine>(*machine_);
        prepared->load_cartridge(Cartridge{rom});
        prepared->load_backup(backup_type_,battery);
        machine_ = std::move(prepared); // échec de ROM/sauvegarde : machine précédente intacte
    }
    void reset() override { machine_->reset(); }
    void run_frame(std::span<std::int32_t> output, bool render_video) override {
        if (!machine_->entries()) throw Error("NDS : aucune cartouche chargée");
        if (output.size() < video_spec().pixel_count()) throw std::invalid_argument("Framebuffer NDS trop petit");
        machine_->run_frame();
        if (!render_video) return;
        const auto frame = machine_->render_frame();
        const auto& power = machine_->power_management();
        const auto draw = [&](const auto& screen, std::size_t offset, bool lit) {
            for (std::size_t i = 0; i < screen.size(); ++i) {
                const auto expand = [](unsigned component) { return (component << 3U) | (component >> 2U); };
                const unsigned pixel = screen[i];
                const auto rgb = lit ? (expand(pixel & 31U) << 16U) | (expand((pixel >> 5U) & 31U) << 8U) |
                    expand((pixel >> 10U) & 31U) : 0U;
                output[offset + i] = std::bit_cast<std::int32_t>(0xff000000U | rgb);
            }
        };
        draw(frame.upper,0,power.upper_backlight_enabled());
        draw(frame.lower,frame.upper.size(),power.lower_backlight_enabled());
    }
    void set_button(Button button, bool pressed) override {
        Key key;
        switch (button) {
        case Button::a: key = Key::a; break;
        case Button::b: key = Key::b; break;
        case Button::up: key = Key::up; break;
        case Button::down: key = Key::down; break;
        case Button::left: key = Key::left; break;
        case Button::right: key = Key::right; break;
        case Button::start: key = Key::start; break;
        case Button::select: key = Key::select; break;
        case Button::l: key = Key::l; break;
        case Button::r: key = Key::r; break;
        default: throw std::invalid_argument("NDS : bouton invalide");
        }
        set_key(key,pressed);
    }
    void set_key(Key key, bool pressed) override { machine_->set_key(key,pressed); }
    bool touch_available() const noexcept override {
        const auto& settings = machine_->memory().firmware().settings();
        return settings && settings->touch_calibration.has_value();
    }
    void set_touch_position(std::uint16_t x, std::uint16_t y, bool pressed) override {
        if (pressed && (x >= 256 || y >= 192)) throw std::invalid_argument("NDS : contact hors de l'écran tactile");
        machine_->set_touch_position(x,y,pressed);
    }
    void set_lid_closed(bool closed) noexcept override {
        auto input = machine_->input_state(); input.lid_closed = closed; machine_->set_input(input);
    }
    bool powered_off() const noexcept override { return machine_->powered_off(); }
    std::size_t read_audio(std::span<std::int16_t> output) override { return machine_->memory().read_audio(output); }
    bool has_battery_ram() const noexcept override { return machine_->entries() && backup_type_ != BackupType::none; }
    bool battery_ram_dirty() const noexcept override { return has_battery_ram() && machine_->backup_dirty(); }
    std::optional<BatterySnapshot> snapshot_battery_ram() override {
        // Le frontend conserve la copie précédente pendant une écriture interne.
        if (!has_battery_ram() || machine_->memory().backup().busy()) return std::nullopt;
        auto snapshot = machine_->snapshot_backup();
        return BatterySnapshot{std::move(snapshot.data),snapshot.generation};
    }
    void acknowledge_battery_ram_saved(std::uint64_t generation) noexcept override { machine_->acknowledge_backup_saved(generation); }
    std::vector<std::uint8_t> save_state() const override { throw SaveStateError("NDS : sauvegardes instantanées non implémentées"); }
    void load_state(std::span<const std::uint8_t>) override { throw SaveStateError("NDS : sauvegardes instantanées non implémentées"); }
private:
    BackupType backup_type_;
    std::unique_ptr<Machine> machine_;
};
} // namespace

std::unique_ptr<FrontendCore> make_core(const CoreOptions& options) { return std::make_unique<NdsCore>(options); }

} // namespace ravenemu::nds
