#include "machine/machine.hpp"

#include <ravenemu/gb/core.hpp>
#include "ravenemu/remote_link.hpp"
#include "ravenemu/sha256.hpp"

#include <algorithm>

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
        // Ni écriture en attente oubliée, ni génération déjà rendue réutilisée.
        replacement->cartridge->continue_after(previous.generation(), previous.dirty());
        install_machine(std::move(replacement));
    }
    void run_frame(std::span<std::int32_t> framebuffer, bool) override {
        require_loaded();
        if (framebuffer.size() < Ppu::frame_pixels) throw std::invalid_argument("Framebuffer trop petit");
        apply_cheats();
        // Le câble distant a pu faire avancer une partie de cette image (service_remote_link).
        int ppu_cycles = std::exchange(carried_cycles_, 0);
        // Un câble distant livre les transferts du partenaire à chaque ligne,
        // moins qu'un bit à 8 kHz.
        if (link_endpoint_ != nullptr) link_endpoint_->poll();
        int next_poll = ppu_cycles + remote_poll_interval;
        while (ppu_cycles < frame_cycles) {
            const int advanced = machine_->step();
            if (advanced <= 0) break;
            ppu_cycles += advanced;
            if (link_endpoint_ != nullptr && ppu_cycles >= next_poll) {
                link_endpoint_->poll();
                next_poll = ppu_cycles + remote_poll_interval;
            }
        }
        // Un transfert demandé, ou attendu d'un partenaire, compte comme usage du port.
        if (machine_->serial.take_activity() || machine_->serial.transfer_active()) frames_since_link_use_ = 0;
        else if (frames_since_link_use_ < link_use_frames) ++frames_since_link_use_;
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
        const std::span<const std::uint8_t> wram = m.bus.wram;
        std::size_t count = 0;
        // Copie par blocs : un frontend Libretro relit tout l'espace à chaque
        // image. La ROM se lit fenêtre de 16 Kio par fenêtre, telle que le
        // contrôleur de cartouche la présente au CPU.
        const auto copy = [&](std::span<const std::uint8_t> source, std::uint64_t base, std::uint64_t a) {
            const auto offset = static_cast<std::size_t>(a - base);
            const auto length = std::min(out.size() - count, source.size() - offset);
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), length,
                        out.begin() + static_cast<std::ptrdiff_t>(count));
            count += length;
        };
        while (count < out.size()) {
            const auto a = static_cast<std::uint64_t>(address) + count;
            const auto before = count;
            if (a < 0x8000) {
                const auto length = std::min<std::size_t>(out.size() - count, 0x4000 - (a & 0x3fff));
                m.cartridge->read_rom_window(static_cast<int>(a), out.subspan(count, length));
                count += length;
            }
            else if (a < 0xa000) copy(std::span(m.ppu.vram).first(0x2000), 0x8000, a);
            else if (a < 0xc000 && a - 0xa000 < ram.size())
                copy(ram.first(std::min<std::size_t>(ram.size(), 0x2000)), 0xa000, a);
            else if (a >= 0xc000 && a < 0xe000) copy(wram.first(0x2000), 0xc000, a);
            else if (a >= 0xe000 && a < 0xfe00) copy(wram.first(0x1e00), 0xe000, a);
            else if (a >= 0xfe00 && a < 0xfea0) copy(m.ppu.oam, 0xfe00, a);
            else if (a >= 0xff00 && a < 0xff80) out[count++] = static_cast<std::uint8_t>(m.bus.read_io(static_cast<int>(a)));
            else if (a >= 0xff80 && a < 0xffff) copy(m.bus.hram, 0xff80, a);
            else if (a == 0xffff) out[count++] = static_cast<std::uint8_t>(m.interrupts.enable);
            else if (a >= 0x10000 && a < 0x16000 && gb::is_cgb_hardware(m.hardware_mode))
                copy(wram.subspan(0x2000, 0x6000), 0x10000, a);
            else if (a >= 0x16000 && a < 0x34000 && a - 0x14000 < ram.size())
                copy(ram.first(std::min<std::size_t>(ram.size(), 0x20000)).subspan(0x2000), 0x16000, a);
            // Hors de l'espace (zone inutilisée FEA0-FEFF, banques absentes) :
            // la lecture s'arrête, et un succès qui en dépend est signalé.
            if (count == before) break;
        }
        return count;
    }
    [[nodiscard]] bool connect_link_endpoint(LinkEndpoint* endpoint) noexcept override {
        if (machine_ != nullptr && !machine_->serial.connect(endpoint)) return false;
        link_endpoint_ = endpoint;
        return true;
    }
    [[nodiscard]] bool connect_remote_link(RemoteLink* link, int) override {
        if (link == nullptr) {
            if (remote_endpoint_ != nullptr && link_endpoint_ == remote_endpoint_.get())
                static_cast<void>(connect_link_endpoint(nullptr));
            remote_endpoint_.reset();
            return true;
        }
        // Un câble local occupe déjà la prise.
        if (link_endpoint_ != nullptr && link_endpoint_ != remote_endpoint_.get()) return false;
        auto endpoint = std::make_unique<RemoteLinkEndpoint>(*link);
        if (!connect_link_endpoint(endpoint.get())) return false;
        remote_endpoint_ = std::move(endpoint);
        return true;
    }
    void service_remote_link() override {
        if (remote_endpoint_ == nullptr || machine_ == nullptr) return;
        remote_endpoint_->poll();
        // Entre deux images (ou console en pause), un octet lu trop tôt
        // attendrait l'image suivante, et l'autre console avec lui : celle-ci
        // avance dès maintenant jusqu'à son cycle, dans l'image à venir, que
        // run_frame terminera ensuite.
        while (remote_endpoint_->deferred() && carried_cycles_ < frame_cycles - remote_poll_interval) {
            const int advanced = machine_->step();
            if (advanced <= 0) break;
            carried_cycles_ += advanced;
            remote_endpoint_->poll();
        }
    }
    [[nodiscard]] bool link_wanted() const noexcept override {
        return machine_ != nullptr && frames_since_link_use_ < link_use_frames;
    }
    [[nodiscard]] std::unique_ptr<LinkCable> create_link_cable(std::span<Core* const> players) override;
    // Pour le câble link : la machine, après les vérifications et les codes de triche de run_frame.
    [[nodiscard]] Machine& machine_for_frame(std::span<const std::int32_t> framebuffer) {
        require_loaded();
        if (framebuffer.size() < Ppu::frame_pixels) throw std::invalid_argument("Framebuffer trop petit");
        apply_cheats();
        return *machine_;
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
    void set_cheats(std::span<const Cheat> cheats) override {
        std::vector<MemoryBus::RomPatch> patches;
        std::vector<CheatOperation> writes;
        for (const auto& cheat : cheats) {
            if (cheat.console != Console::game_boy) throw std::invalid_argument("Code de triche d'une autre console");
            for (const auto& operation : cheat.operations) {
                if (operation.action == CheatAction::rom_patch) {
                    patches.push_back({static_cast<std::uint16_t>(operation.address),
                                       static_cast<std::uint8_t>(operation.value), operation.original});
                } else if (operation.action == CheatAction::write) {
                    writes.push_back(operation);
                }
            }
        }
        rom_patches_ = std::move(patches);
        cheat_writes_ = std::move(writes);
        if (machine_) machine_->bus.rom_patches = rom_patches_;
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
    /**
     * Codes GameShark : l'appareil récrit ses octets à chaque image. La banque
     * du code choisit la RAM de cartouche, ou la WRAM de la GBC pour 91 à 97 ;
     * ailleurs, l'octet s'écrit là où le CPU l'écrirait.
     */
    void apply_cheats() {
        auto& machine = *machine_;
        for (const auto& write : cheat_writes_) {
            const auto address = static_cast<int>(write.address);
            const auto value = static_cast<int>(write.value);
            if (address >= 0xa000 && address <= 0xbfff) {
                machine.cartridge->write_ram_bank(write.bank, address, value);
            } else if (address >= 0xd000 && address <= 0xdfff && write.bank >= 0x91 && write.bank <= 0x97 &&
                       gb::cgb_features_enabled(machine.hardware_mode)) {
                machine.bus.wram[static_cast<std::size_t>((write.bank & 7) * 0x1000 + address - 0xd000)] =
                    static_cast<std::uint8_t>(value);
            } else {
                machine.bus.write(address, value);
            }
        }
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
        replacement->bus.rom_patches = rom_patches_;
        machine_ = std::move(replacement);
        carried_cycles_ = 0;  // la nouvelle machine commence son image
    }
    void reconnect_current_endpoints() noexcept {
        if (!machine_) return;
        static_cast<void>(machine_->serial.connect(link_endpoint_));
        static_cast<void>(machine_->infrared_router.connect(infrared_endpoint_));
    }
    void require_loaded() const {
        if (!machine_) throw std::logic_error("Aucune ROM chargée");
    }

    // Avant la machine : détruit après elle, quand son port s'en est détaché.
    std::unique_ptr<RemoteLinkEndpoint> remote_endpoint_;
    std::unique_ptr<Machine> machine_;
    RomImage loaded_rom_;
    std::array<std::uint8_t, 32> rom_hash_{};
    bool tilt_sensor_{};
    mutable std::size_t state_size_hint_{64 * 1024};
    std::optional<std::int64_t> clock_override_;
    gb::HardwareModel requested_hardware_{gb::HardwareModel::automatic};
    std::vector<std::uint8_t> boot_rom_;
    static constexpr int remote_poll_interval = 456;
    static constexpr int frame_cycles = 70'224;
    // Cycles de l'image en cours déjà joués entre deux images pour le câble distant.
    int carried_cycles_{};
    // Trois secondes sans transfert : le jeu ne cherche plus de partenaire.
    static constexpr int link_use_frames = 180;
    LinkEndpoint* link_endpoint_{};
    int frames_since_link_use_{link_use_frames};
    InfraredEndpoint* infrared_endpoint_{};
    int host_acceleration_x_{};
    int host_acceleration_y_{};
    std::vector<MemoryBus::RomPatch> rom_patches_;
    std::vector<CheatOperation> cheat_writes_;
};

/**
 * Câble link Game Boy. Les deux ports série s'échangent leurs bits par un
 * LocalLinkEndpoint, à l'instant où la console qui fournit l'horloge en
 * cadence un. Les machines avancent à tour de rôle par tranches d'une ligne
 * (456 cycles, moins qu'un bit à 8 kHz) : l'autre console a donc préparé son
 * transfert avant que ses bits arrivent, et répond à temps au suivant.
 */
class GameBoyLinkCable final : public LinkCable {
public:
    static constexpr int slice = 456;
    GameBoyLinkCable(GameBoyCore& first, GameBoyCore& second) : players_{&first, &second} {
        if (!first.connect_link_endpoint(&endpoint_) || !second.connect_link_endpoint(&endpoint_)) {
            disconnect();
            throw std::invalid_argument("Ces consoles ne se relient pas par un câble link Game Boy");
        }
    }
    GameBoyLinkCable(const GameBoyLinkCable&) = delete;
    GameBoyLinkCable& operator=(const GameBoyLinkCable&) = delete;
    ~GameBoyLinkCable() override { disconnect(); }

    [[nodiscard]] std::size_t players() const noexcept override { return players_.size(); }
    void run_frame(std::span<const std::span<std::int32_t>> framebuffers, bool) override {
        if (framebuffers.size() != players_.size()) throw std::invalid_argument("Une image par console reliée");
        constexpr int frame_cycles = 70'224;
        std::array<Machine*, 2> machines{};
        for (std::size_t index = 0; index < players_.size(); ++index)
            machines[index] = &players_[index]->machine_for_frame(framebuffers[index]);
        // Chaque machine avance d'une image, comme GameBoyCore::run_frame,
        // mais par tranches entrelacées.
        std::array<int, 2> elapsed{};
        std::array<bool, 2> stopped{};
        for (auto end = slice;; end += slice) {
            const auto target = std::min(end, frame_cycles);
            for (std::size_t index = 0; index < machines.size(); ++index) {
                while (!stopped[index] && elapsed[index] < target) {
                    const int advanced = machines[index]->step();
                    if (advanced <= 0) stopped[index] = true;
                    else elapsed[index] += advanced;
                }
            }
            if (target == frame_cycles) break;
        }
        for (std::size_t index = 0; index < machines.size(); ++index)
            std::copy(machines[index]->ppu.completed_frame.begin(), machines[index]->ppu.completed_frame.end(),
                      framebuffers[index].begin());
    }

private:
    void disconnect() noexcept {
        for (auto* player : players_) static_cast<void>(player->connect_link_endpoint(nullptr));
    }
    LocalLinkEndpoint endpoint_;
    std::array<GameBoyCore*, 2> players_;
};

std::unique_ptr<LinkCable> GameBoyCore::create_link_cable(std::span<Core* const> players) {
    if (players.size() != 2) throw std::invalid_argument("Le câble link Game Boy relie deux consoles");
    auto* first = dynamic_cast<GameBoyCore*>(players[0]);
    auto* second = dynamic_cast<GameBoyCore*>(players[1]);
    if (first == nullptr || second == nullptr)
        throw std::invalid_argument("Un câble link Game Boy ne relie que des Game Boy et Game Boy Color");
    return std::make_unique<GameBoyLinkCable>(*first, *second);
}

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

