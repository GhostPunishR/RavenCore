#include "machine/machine.hpp"

#include <ravenemu/gb/core.hpp>
#include "ravenemu/sha256.hpp"

namespace ravenemu {
namespace cgb {

class GameBoyCore final : public Core {
public:
    GameBoyCore(gb::HardwareModel requested_hardware, std::span<const std::uint8_t> boot_rom)
        : requested_hardware_(requested_hardware), boot_rom_(boot_rom.begin(), boot_rom.end()) {}

    [[nodiscard]] Console console() const noexcept override { return Console::game_boy; }
    [[nodiscard]] VideoSpec video_spec() const noexcept override { return {160, 144, 4'194'304.0 / 70'224.0}; }
    [[nodiscard]] AudioSpec audio_spec() const noexcept override { return {Apu::sample_rate, 2}; }
    [[nodiscard]] FramebufferFormat framebuffer_format() const noexcept override {
        return machine_ != nullptr && gb::cgb_color_output(machine_->hardware_mode)
            ? FramebufferFormat::argb_8888 : FramebufferFormat::indexed_4;
    }

    void load_rom(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> battery) override {
        const auto tilt_sensor = CartridgeHeader::parse(rom).mbc == MbcType::mbc7;
        auto image = std::make_shared<const std::vector<std::uint8_t>>(rom.begin(), rom.end());
        auto replacement = new_machine(image);
        if (!battery.empty()) replacement->cartridge->import_battery(battery);
        const auto new_hash = detail::sha256(rom);
        install_machine(std::move(replacement));
        loaded_rom_ = std::move(image);
        rom_hash_ = new_hash;
        tilt_sensor_ = tilt_sensor;
    }
    void reset() override {
        require_loaded();
        const auto& previous = *machine_->cartridge;
        const auto battery = machine_->cartridge->export_battery();
        auto replacement = new_machine(loaded_rom_);
        if (battery) replacement->cartridge->import_battery(*battery);
        // Une écriture pas encore enregistrée reste à enregistrer après le reset.
        if (previous.dirty()) replacement->cartridge->mark_restored_after(previous.generation());
        install_machine(std::move(replacement));
    }
    void run_frame(std::span<std::int32_t> framebuffer, bool) override {
        require_loaded();
        if (framebuffer.size() < Ppu::frame_pixels) throw std::invalid_argument("Framebuffer trop petit");
        int ppu_cycles{};
        while (ppu_cycles < 70'224) {
            const int advanced = machine_->step();
            if (advanced <= 0) break;
            ppu_cycles += advanced;
        }
        std::copy(machine_->ppu.completed_frame.begin(), machine_->ppu.completed_frame.end(), framebuffer.begin());
    }
    void set_button(Button button, bool pressed) override { if (machine_) machine_->joypad.set_button(button, pressed); }
    std::size_t read_audio(std::span<std::int16_t> destination) override {
        return machine_ ? machine_->apu.read_samples(destination) : 0;
    }
    std::size_t read_achievement_memory(std::uint32_t address, std::span<std::uint8_t> out) const override {
        if (!machine_) return 0;
        const auto& m = *machine_;
        const auto ram = m.cartridge->achievement_ram();
        std::size_t count = 0;
        for (auto& value : out) {
            const auto a = static_cast<std::uint64_t>(address) + count;
            // Chaque branche borne `a` : l'index tient dans size_t, même sur 32 bits.
            const auto at = [a](std::uint64_t base) { return static_cast<std::size_t>(a - base); };
            if (a < 0x8000) value = static_cast<std::uint8_t>(m.cartridge->read_rom(static_cast<int>(a)));
            else if (a < 0xa000) value = m.ppu.vram[at(0x8000)];
            else if (a < 0xc000 && a - 0xa000 < ram.size()) value = ram[at(0xa000)];
            else if (a >= 0xc000 && a < 0xe000) value = m.bus.wram[at(0xc000)];
            else if (a >= 0xe000 && a < 0xfe00) value = m.bus.wram[at(0xe000)];
            else if (a >= 0xfe00 && a < 0xfea0) value = m.ppu.oam[at(0xfe00)];
            else if (a >= 0xff80 && a < 0xffff) value = m.bus.hram[at(0xff80)];
            else if (a == 0xffff) value = static_cast<std::uint8_t>(m.interrupts.enable);
            else if (a >= 0x10000 && a < 0x16000 && gb::is_cgb_hardware(m.hardware_mode)) value = m.bus.wram[at(0x10000) + 0x2000];
            else if (a >= 0x16000 && a < 0x34000 && a - 0x14000 < ram.size()) value = ram[at(0x14000)];
            else break; // Unsupported IO/mapper memory invalidates dependent achievements.
            ++count;
        }
        return count;
    }
    [[nodiscard]] bool connect_link_endpoint(LinkEndpoint* endpoint) noexcept override {
        if (machine_ != nullptr && !machine_->serial.connect(endpoint)) return false;
        link_endpoint_ = endpoint;
        return true;
    }
    [[nodiscard]] bool connect_infrared_endpoint(InfraredEndpoint* endpoint) noexcept override {
        if (machine_ != nullptr && !machine_->infrared_router.connect(endpoint)) return false;
        infrared_endpoint_ = endpoint;
        return true;
    }
    [[nodiscard]] bool rumble_active() const noexcept override {
        return machine_ != nullptr && machine_->cartridge->rumble_active();
    }
    /** L'accéléromètre des cartouches MBC7 (Kirby Tilt 'n' Tumble, Command Master). */
    [[nodiscard]] CartridgeSensors cartridge_sensors() const noexcept override {
        CartridgeSensors sensors;
        sensors.tilt = tilt_sensor_;
        return sensors;
    }
    void set_game_boy_acceleration(int x, int y) noexcept override {
        host_acceleration_x_ = x;
        host_acceleration_y_ = y;
        if (machine_) machine_->cartridge->set_acceleration(x, y);
    }
    [[nodiscard]] bool has_battery_ram() const noexcept override {
        return machine_ != nullptr && machine_->cartridge->has_persistent_data();
    }
    [[nodiscard]] bool battery_ram_dirty() const noexcept override {
        return machine_ != nullptr && machine_->cartridge->dirty();
    }
    std::optional<BatterySnapshot> snapshot_battery_ram() override {
        if (!machine_) return std::nullopt;
        auto data = machine_->cartridge->export_battery();
        if (!data) return std::nullopt;
        return BatterySnapshot{std::move(*data), machine_->cartridge->generation()};
    }
    void acknowledge_battery_ram_saved(std::uint64_t generation) noexcept override {
        if (machine_) machine_->cartridge->acknowledge_saved(generation);
    }

    [[nodiscard]] std::vector<std::uint8_t> save_state() const override {
        // Réserver la taille du dernier état évite plusieurs réallocations par
        // sauvegarde (176 ko au moins, 1,2 Mo avec un MBC6).
        require_loaded(); BinaryWriter out(state_size_hint_);
        out.u32(0x52564e53U); out.u16(state_version); out.u8(static_cast<std::uint8_t>(Console::game_boy)); out.raw(rom_hash_);
        out.u8(static_cast<std::uint8_t>(machine_->hardware_mode));
        machine_->cpu.save(out);
        out.i32(machine_->interrupts.flags); out.i32(machine_->interrupts.enable);
        machine_->timer.save(out); machine_->serial.save(out); machine_->joypad.save(out);
        machine_->ppu.save(out); machine_->speed.save(out); machine_->bus.save(out);
        machine_->apu.save(out); machine_->cartridge->save_state(out);
        auto bytes = std::move(out).take();
        state_size_hint_ = bytes.size();
        return bytes;
    }
    void load_state(std::span<const std::uint8_t> state) override {
        require_loaded();
        // Un MBC6 contient 1 Mio de flash en plus de la machine et de sa SRAM.
        // Deux Mio conservent un garde-fou strict tout en couvrant son état.
        if (state.size() > (2U << 20U)) throw SaveStateError("État instantané trop volumineux");
        BinaryReader in(state);
        if (in.u32() != 0x52564e53U) throw SaveStateError("Ce fichier n'est pas un état RavenEmu");
        // Le format 10 ne diffère que par ses images PPU en i32 : il reste lisible.
        const auto version = in.u16();
        if (version != 10 && version != state_version) throw SaveStateError("Version d'état non prise en charge");
        if (in.u8() != static_cast<std::uint8_t>(Console::game_boy)) throw SaveStateError("État issu d'une autre console");
        std::array<std::uint8_t, 32> hash{}; in.raw(hash);
        if (hash != rom_hash_) throw SaveStateError("État issu d'une autre ROM");
        auto replacement = new_machine(loaded_rom_);
        const auto state_hardware = static_cast<gb::HardwareMode>(in.u8());
        if (state_hardware != replacement->hardware_mode) {
            throw SaveStateError("État issu d'un autre mode matériel GB/GBC");
        }
        replacement->cpu.load(in);
        replacement->interrupts.flags = in.bounded_i32(0, 0xff);
        replacement->interrupts.enable = in.bounded_i32(0, 0xff);
        replacement->timer.load(in); replacement->serial.load(in); replacement->joypad.load(in);
        replacement->ppu.load(in, version >= 11); replacement->speed.load(in); replacement->bus.load(in);
        replacement->apu.load(in); replacement->cartridge->load_state(in);
        if (!in.exhausted()) throw SaveStateError("État instantané corrompu (données excédentaires)");
        replacement->cartridge->mark_restored_after(machine_->cartridge->generation());
        install_machine(std::move(replacement));
    }
    void set_clock_epoch(std::optional<std::int64_t> epoch) noexcept override { clock_override_ = epoch; }

private:
    static constexpr std::uint16_t state_version = 11;

    [[nodiscard]] std::int64_t current_epoch() const noexcept {
        if (clock_override_) return *clock_override_;
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    [[nodiscard]] std::unique_ptr<Machine> new_machine(RomImage rom) const {
        const auto& header = CartridgeHeader::parse(*rom);
        gb::HardwareMode mode{};
        switch (requested_hardware_) {
        case gb::HardwareModel::automatic:
            mode = header.uses_color ? gb::HardwareMode::cgb_native : gb::HardwareMode::dmg;
            break;
        case gb::HardwareModel::dmg:
            if (header.requires_color) throw RomLoadError("Cette cartouche requiert le matériel Game Boy Color");
            mode = gb::HardwareMode::dmg;
            break;
        case gb::HardwareModel::cgb:
            mode = header.uses_color ? gb::HardwareMode::cgb_native : gb::HardwareMode::cgb_compatibility;
            break;
        }
        auto machine = std::make_unique<Machine>(
            rom,
            [this] { return current_epoch(); },
            mode,
            boot_rom_
        );
        machine->cartridge->set_acceleration(host_acceleration_x_, host_acceleration_y_);
        return machine;
    }
    void install_machine(std::unique_ptr<Machine> replacement) {
        if (machine_) {
            machine_->serial.disconnect();
            machine_->infrared_router.disconnect();
        }
        if (!replacement->serial.connect(link_endpoint_)) {
            reconnect_current_endpoints();
            throw std::logic_error("LinkEndpoint refuse une nouvelle extrémité série");
        }
        if (!replacement->infrared_router.connect(infrared_endpoint_)) {
            replacement->serial.disconnect();
            reconnect_current_endpoints();
            throw std::logic_error("InfraredEndpoint refuse une nouvelle extrémité");
        }
        machine_ = std::move(replacement);
    }
    void reconnect_current_endpoints() noexcept {
        if (!machine_) return;
        static_cast<void>(machine_->serial.connect(link_endpoint_));
        static_cast<void>(machine_->infrared_router.connect(infrared_endpoint_));
    }
    void require_loaded() const {
        if (!machine_) throw std::logic_error("Aucune ROM chargée");
    }

    std::unique_ptr<Machine> machine_;
    RomImage loaded_rom_;
    std::array<std::uint8_t, 32> rom_hash_{};
    bool tilt_sensor_{};
    mutable std::size_t state_size_hint_{64 * 1024};
    std::optional<std::int64_t> clock_override_;
    gb::HardwareModel requested_hardware_{gb::HardwareModel::automatic};
    std::vector<std::uint8_t> boot_rom_;
    LinkEndpoint* link_endpoint_{};
    InfraredEndpoint* infrared_endpoint_{};
    int host_acceleration_x_{};
    int host_acceleration_y_{};
};

} // namespace cgb

std::unique_ptr<Core> make_game_boy_core() {
    return gb::make_core(gb::HardwareModel::automatic);
}

namespace gb {
std::unique_ptr<Core> make_core(HardwareModel model, std::span<const std::uint8_t> boot_rom) {
    return std::make_unique<cgb::GameBoyCore>(model, boot_rom);
}
} // namespace gb

} // namespace ravenemu

