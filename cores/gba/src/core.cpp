#include "machine/machine.hpp"
#include "serial/remote_serial_link.hpp"

#include "ravenemu/sha256.hpp"

#include <algorithm>

namespace ravenemu {
namespace gba {

class GbaCore final : public Core {
public:
    explicit GbaCore(std::optional<GbaSaveType> forced) : forced_(forced) {}
    [[nodiscard]] Console console() const noexcept override { return Console::game_boy_advance; }
    [[nodiscard]] VideoSpec video_spec() const noexcept override { return {240, 160, 16'777'216.0 / 280'896.0}; }
    [[nodiscard]] AudioSpec audio_spec() const noexcept override { return {Apu::sample_rate, 2}; }
    [[nodiscard]] FramebufferFormat framebuffer_format() const noexcept override {
        return FramebufferFormat::argb_8888;
    }
    [[nodiscard]] bool supports_video_frame_skipping() const noexcept override { return true; }

    void load_rom(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> battery) override {
        if (rom.size() > Cartridge::max_rom_size) throw RomLoadError("ROM GBA trop volumineuse");
        if (rom.size() < 0xc0U) throw RomLoadError("ROM GBA trop courte");
        if (rom[0xb2] != 0x96U) throw RomLoadError("Marqueur GBA 0x96 absent");
        auto image = std::make_shared<const std::vector<std::uint8_t>>(rom.begin(), rom.end());
        const auto traits = RomTraits::detect(rom);
        auto replacement = new_machine(image, traits);
        if (!battery.empty()) replacement->cartridge.import_battery(battery);
        loaded_rom_ = std::move(image);
        rom_traits_ = traits;
        rom_hash_ = detail::sha256(rom);
        install(std::move(replacement));
    }
    void reset() override {
        require_loaded();
        auto replacement = new_machine(loaded_rom_, rom_traits_);
        replacement->cartridge.keep_battery_of(machine_->cartridge);
        configure_measurement(*replacement, measuring_time_);
        install(std::move(replacement));
    }
    void run_frame(std::span<std::int32_t> framebuffer, bool render_video) override {
        require_loaded();
        if (framebuffer.size() < Ppu::screen_width * Ppu::screen_height)
            throw std::invalid_argument("Framebuffer trop petit");
        machine_->ppu.render_enabled = render_video;
        try {
            if (remote_) run_remote_frame();
            else machine_->run_frame();
        } catch (...) {
            machine_->ppu.render_enabled = true;
            throw;
        }
        machine_->ppu.render_enabled = true;
        // Un transfert demandé, un mode multijoueur ou un esclave qui attend : le jeu cherche un partenaire.
        auto& serial = machine_->serial;
        if (serial.take_activity() || serial.transferring() || serial.mode() == Serial::Mode::multiplayer ||
            serial.ready_for(Serial::Mode::normal8))
            frames_since_link_use_ = 0;
        else if (frames_since_link_use_ < link_use_frames)
            ++frames_since_link_use_;
        if (render_video) std::copy(machine_->ppu.frame.begin(), machine_->ppu.frame.end(), framebuffer.begin());
    }
    [[nodiscard]] bool connect_remote_link(RemoteLink* link, int player) override {
        if (link == nullptr) {
            remote_.reset();  // le port redevient seul
            return true;
        }
        if (player < 0 || player > 1) return false;
        remote_.reset();
        remote_ = std::make_unique<RemoteSerialLink>(*link, player);
        if (machine_) remote_->attach(machine_->serial);
        return true;
    }
    void service_remote_link() override {
        if (!remote_ || !machine_) return;
        auto& machine = *machine_;
        remote_->attach(machine.serial);
        remote_->service();
        // Entre deux images (ou console en pause), un transfert lu trop tôt
        // attendrait l'image suivante, et l'autre console avec lui : celle-ci
        // avance dès maintenant jusqu'à son cycle, moins d'un transfert plus
        // loin. run_frame reprend ensuite l'image où elle en est.
        for (int ahead = 0; remote_->deferred() && ahead <= 2 * Serial::longest_transfer;) {
            const int advanced = machine.run_for(remote_slice);
            if (advanced <= 0) break;
            ahead += advanced;
            remote_->service();
        }
    }
    [[nodiscard]] bool link_wanted() const noexcept override {
        return machine_ != nullptr && frames_since_link_use_ < link_use_frames;
    }
    void set_button(Button button, bool pressed) override {
        if (machine_) machine_->bus.keypad.set_button(button, pressed);
    }
    [[nodiscard]] std::unique_ptr<LinkCable> create_link_cable(std::span<Core* const> players) override;
    void unplug_link() noexcept {
        if (machine_) machine_->serial.refresh_status();
    }
    // Pour le câble link : la machine, après les vérifications de run_frame.
    [[nodiscard]] Machine& machine_for_frame(std::span<const std::int32_t> framebuffer) {
        require_loaded();
        if (framebuffer.size() < Ppu::screen_width * Ppu::screen_height)
            throw std::invalid_argument("Framebuffer trop petit");
        return *machine_;
    }
    std::size_t read_audio(std::span<std::int16_t> destination) override {
        return machine_ ? machine_->apu.read_samples(destination) : 0;
    }
    std::size_t read_achievement_memory(std::uint32_t address, std::span<std::uint8_t> out) const override {
        if (!machine_) return 0;
        // Espace de RetroAchievements : IWRAM en 0, EWRAM en 0x8000, puis
        // jusqu'à 64 Kio de la puce de sauvegarde en 0x48000. Copie par blocs :
        // un frontend Libretro relit tout l'espace à chaque image.
        std::span<const std::uint8_t> save;
        if (const auto* memory = machine_->cartridge.save()) save = memory->data();
        const std::array<std::pair<std::uint64_t, std::span<const std::uint8_t>>, 3> regions{{
            {0x0, machine_->bus.iwram},
            {0x8000, machine_->bus.ewram},
            {0x48000, save.first(std::min<std::size_t>(save.size(), 0x10000))},
        }};
        std::size_t count = 0;
        while (count < out.size()) {
            const auto a = static_cast<std::uint64_t>(address) + count;
            const auto region = std::find_if(regions.begin(), regions.end(), [a](const auto& entry) {
                return a >= entry.first && a - entry.first < entry.second.size();
            });
            if (region == regions.end()) break;
            const auto offset = static_cast<std::size_t>(a - region->first);
            const auto length = std::min(out.size() - count, region->second.size() - offset);
            std::copy_n(region->second.begin() + static_cast<std::ptrdiff_t>(offset), length,
                        out.begin() + static_cast<std::ptrdiff_t>(count));
            count += length;
        }
        return count;
    }
    [[nodiscard]] bool has_battery_ram() const noexcept override { return machine_ && machine_->cartridge.save(); }
    [[nodiscard]] bool battery_ram_dirty() const noexcept override {
        return machine_ && machine_->cartridge.save() && machine_->cartridge.save()->dirty();
    }
    std::optional<BatterySnapshot> snapshot_battery_ram() override {
        if (!machine_ || !machine_->cartridge.save()) return std::nullopt;
        return BatterySnapshot{machine_->cartridge.export_battery(), machine_->cartridge.save()->generation()};
    }
    void acknowledge_battery_ram_saved(std::uint64_t generation) noexcept override {
        if (machine_ && machine_->cartridge.save()) machine_->cartridge.save()->acknowledge(generation);
    }
    [[nodiscard]] GbaSaveType gba_save_type() const noexcept override {
        return machine_ ? machine_->cartridge.save_type() : GbaSaveType::none;
    }
    void set_gba_forced_save_type(std::optional<GbaSaveType> value) noexcept override { forced_ = value; }
    /**
     * Horloge réellement présente sur la cartouche émulée.
     *
     * Distinct du réglage : il dit ce que l'appelant demande, celui-ci ce que le
     * jeu voit. C'est cette valeur-là qu'une interface doit montrer.
     */
    [[nodiscard]] bool gba_rtc_active() const noexcept override {
        return machine_ != nullptr && machine_->cartridge.gpio() != nullptr && machine_->cartridge.gpio()->has_rtc();
    }
    [[nodiscard]] bool rumble_active() const noexcept override {
        return machine_ != nullptr && machine_->cartridge.gpio() != nullptr &&
               machine_->cartridge.gpio()->rumble_active();
    }
    [[nodiscard]] CartridgeSensors cartridge_sensors() const noexcept override {
        return machine_ ? machine_->cartridge.sensors() : CartridgeSensors{};
    }
    // Grandeurs gardées par le cœur : elles survivent au reset et au
    // chargement d'un état, qui remplacent la machine.
    void set_solar_light(int level) noexcept override {
        sensor_input_.light = level;
        apply_sensor_input();
    }
    void set_rotation_speed(int speed) noexcept override {
        sensor_input_.rotation = speed;
        apply_sensor_input();
    }
    void set_gba_tilt(int x, int y) noexcept override {
        sensor_input_.tilt_x = x;
        sensor_input_.tilt_y = y;
        apply_sensor_input();
    }
    void set_gba_forced_rtc(std::optional<bool> value) noexcept override { forced_rtc_ = value; }
    void set_clock_epoch(std::optional<std::int64_t> value) noexcept override { clock_override_ = value; }
    void set_measuring_time(bool value) noexcept override {
        measuring_time_ = value;
        if (machine_) configure_measurement(*machine_, value);
    }
    [[nodiscard]] bool measuring_time() const noexcept override { return measuring_time_; }
    [[nodiscard]] std::vector<DiagnosticMessage> drain_diagnostics() override {
        return machine_ ? machine_->bus.diagnostics.drain() : std::vector<DiagnosticMessage>{};
    }
    [[nodiscard]] std::optional<GbaDebugSnapshot> debug_snapshot() const override {
        if (!machine_) return std::nullopt;
        const auto& m = *machine_;
        GbaDebugSnapshot result{};
        result.instructions_per_frame = m.bus.diagnostics.instructions_last_frame;
        result.program_counter = m.cpu.state.regs[15];
        result.thumb = m.cpu.state.thumb;
        result.halted = m.cpu.state.halted;
        result.last_swi = m.bus.diagnostics.last_swi;
        result.last_interrupt_mask = m.bus.diagnostics.last_interrupt_mask;
        result.vcount = m.ppu.vcount;
        result.last_dma_channel = m.dma.last_channel;
        result.dma_active = m.dma.active();
        result.fifo_a_size = m.apu.fifo_size(0);
        result.fifo_b_size = m.apu.fifo_size(1);
        result.fifo_a_empty_reads = m.apu.fifo_empty_reads(0);
        result.fifo_b_empty_reads = m.apu.fifo_empty_reads(1);
        result.audio_underruns = m.apu.underruns;
        result.unsupported_swi_count = m.bus.diagnostics.count(DiagnosticEvent::unsupported_swi);
        result.undefined_instruction_count = m.bus.diagnostics.count(DiagnosticEvent::undefined_instruction);
        result.unsupported_access_count = m.bus.diagnostics.count(DiagnosticEvent::unsupported_access);
        result.missing_interrupt_count = m.bus.diagnostics.count(DiagnosticEvent::missing_interrupt);
        result.decompression_error_count = m.bus.diagnostics.count(DiagnosticEvent::decompression_error);
        result.first_unsupported_address = m.bus.diagnostics.first_unsupported_address;
        result.dispcnt = io_half(m.bus, 0);
        result.bg0_control = io_half(m.bus, 8);
        result.bg1_control = io_half(m.bus, 0x0a);
        result.bg2_control = io_half(m.bus, 0x0c);
        result.bg3_control = io_half(m.bus, 0x0e);
        result.blend_control = io_half(m.bus, 0x50);
        result.blend_alpha = io_half(m.bus, 0x52);
        result.blend_brightness = io_half(m.bus, 0x54);
        result.window_inside = io_half(m.bus, 0x48);
        result.window_outside = io_half(m.bus, 0x4a);
        result.luma_min = m.ppu.frame_luma_min;
        result.luma_max = m.ppu.frame_luma_max;
        result.luma_mean = m.ppu.frame_luma_mean;
        result.layer_pixels.assign(m.ppu.layer_pixels.begin(), m.ppu.layer_pixels.end());
        result.bg2_reference_x = signed28(io_word(m.bus, 0x28)) >> 8;
        result.bg2_reference_y = signed28(io_word(m.bus, 0x2c)) >> 8;
        result.bg2_scale_x = io_half(m.bus, 0x20);
        result.bg2_scale_y = io_half(m.bus, 0x26);
        result.bg2_matrix_writes = m.bus.diagnostics.bg2_matrix_writes;
        result.bg2_reference_writes = m.bus.diagnostics.bg2_reference_writes;
        result.swi_counts.assign(m.bus.diagnostics.swi_counts.begin(), m.bus.diagnostics.swi_counts.end());
        result.ppu_millis = static_cast<double>(m.bus.diagnostics.ppu_nanos_last_frame) / 1'000'000.0;
        result.dma_millis = static_cast<double>(m.bus.diagnostics.dma_nanos_last_frame) / 1'000'000.0;
        result.apu_millis = static_cast<double>(m.bus.diagnostics.apu_nanos_last_frame) / 1'000'000.0;
        return result;
    }

    [[nodiscard]] std::vector<std::uint8_t> save_state() const override {
        require_loaded();
        auto& m = *machine_;
        BinaryWriter out(768U * 1024U);
        out.u32(0x52564e53U);
        out.u16(state_version);
        out.u8(2);
        out.raw(rom_hash_);
        const auto banks = m.cpu.state.export_banks();
        for (const auto value : m.cpu.state.regs) out.i32(value);
        out.i32(m.cpu.state.cpsr());
        out.boolean(m.cpu.state.halted);
        out.i32(static_cast<int>(banks.size()));
        for (const auto value : banks) out.i32(value);
        out.raw(m.bus.ewram);
        out.raw(m.bus.iwram);
        out.raw(m.bus.io);
        out.raw(m.bus.palette);
        out.raw(m.bus.vram);
        out.raw(m.bus.oam);
        out.raw(m.bus.fallback_sram);
        out.i32(m.bus.keypad.pressed_bits);
        const auto ppu_state = m.ppu.state_fields();
        out.i32(static_cast<int>(ppu_state.size()));
        for (const auto value : ppu_state) out.i32(value);
        for (const auto pixel : m.ppu.frame) out.i32(pixel);
        out.i32(m.interrupts.enable);
        out.i32(m.interrupts.flags);
        out.boolean(m.interrupts.master_enable);
        for (const auto value : m.timers.export_state()) out.i32(value);
        for (const auto value : m.dma.export_state()) out.i32(value);
        const auto& wait = m.bios.wait_state();
        out.boolean(wait.has_value());
        out.i32(wait ? wait->interrupt_mask : 0);
        out.boolean(wait && wait->discard_old_flags);
        const auto* save = m.cartridge.save();
        out.i32(static_cast<int>(m.cartridge.save_type()));
        if (save) save->save_state(out);
        const auto* gpio = m.cartridge.gpio();
        out.boolean(gpio != nullptr);
        if (gpio)
            for (const auto value : gpio->export_state()) out.i32(value);
        m.apu.save(out);
        // Format 12 : champs ajoutés en fin d'état, absents du format 11.
        out.u32(m.bus.bios_latch);
        out.boolean(m.bus.executing_bios);
        const auto pipeline = m.cpu.pipeline();
        out.i32(pipeline.opcodes[0]);
        out.i32(pipeline.opcodes[1]);
        out.boolean(pipeline.valid);
        out.boolean(pipeline.thumb);
        out.u32(m.bus.last_fetch);
        out.i32(m.serial.export_remaining());
        out.i32(m.bus.sequential_address());
        // Format 13 : capteurs de cartouche.
        for (const auto value : m.cartridge.export_sensor_state()) out.i32(value);
        // Format 14 : tampon de préchargement et démarrage des timers.
        const auto& prefetch = m.bus.prefetch();
        out.u32(prefetch.head);
        out.i32(prefetch.count);
        out.i32(prefetch.progress);
        out.boolean(prefetch.active);
        for (const auto value : m.timers.export_start_delay()) out.i32(value);
        // Format 14 aussi : transfert en cours sur le câble link.
        for (const auto value : m.serial.export_link_state()) out.i32(value);
        // Format 15 : recharges de timer pas encore en service, âge de la ligne
        // d'IRQ, temps restant d'une fonction du BIOS interrompue. Format 16 :
        // retour d'un Halt du BIOS pas encore réveillé, puis SIOMULTI0-3.
        for (const auto value : m.timers.export_pending_reloads()) out.i32(value);
        out.i32(m.irq_line_age());
        for (const auto value : m.export_bios_busy()) out.i32(value);
        for (const auto value : m.serial.multiplayer_data()) out.i32(value);
        return std::move(out).take();
    }
    void load_state(std::span<const std::uint8_t> bytes) override {
        require_loaded();
        if (bytes.size() > (1U << 20U)) throw SaveStateError("État GBA trop volumineux");
        BinaryReader in(bytes);
        if (in.u32() != 0x52564e53U) throw SaveStateError("Ce fichier n'est pas un état RavenEmu");
        // Les formats 11 et 12 ne diffèrent que par les champs ajoutés en fin d'état.
        const auto version = in.u16();
        if (version < 11 || version > state_version) throw SaveStateError("Version d'état GBA non prise en charge");
        if (in.u8() != 2) throw SaveStateError("État issu d'une autre console");
        std::array<std::uint8_t, 32> hash{};
        in.raw(hash);
        if (hash != rom_hash_) throw SaveStateError("État issu d'une autre ROM");
        auto replacement = new_machine(loaded_rom_, rom_traits_);
        auto& m = *replacement;
        std::array<std::int32_t, 16> registers{};
        for (auto& value : registers) value = in.i32();
        const auto cpsr = in.i32();
        m.cpu.state.halted = in.boolean();
        if (in.i32() != 28) throw SaveStateError("État GBA corrompu (banques)");
        std::array<std::int32_t, 28> banks{};
        for (auto& value : banks) value = in.i32();
        m.cpu.state.import_banks(banks);
        m.cpu.state.set_control_raw(cpsr);
        m.cpu.state.regs = registers;
        read_array(in, m.bus.ewram);
        read_array(in, m.bus.iwram);
        read_array(in, m.bus.io);
        m.bus.sync_timing_from_io();
        read_array(in, m.bus.palette);
        read_array(in, m.bus.vram);
        read_array(in, m.bus.oam);
        read_array(in, m.bus.fallback_sram);
        m.bus.keypad.pressed_bits = in.bounded_i32(0, 0x3ff);
        if (in.i32() != Ppu::state_field_count) throw SaveStateError("État GBA corrompu (PPU)");
        std::array<std::int32_t, Ppu::state_field_count> ppu_state{};
        for (auto& value : ppu_state) value = in.i32();
        m.ppu.restore_state(ppu_state);
        for (auto& pixel : m.ppu.frame) pixel = in.i32();
        m.interrupts.enable = in.bounded_i32(0, 0xffff);
        m.interrupts.flags = in.bounded_i32(0, 0xffff);
        m.interrupts.master_enable = in.boolean();
        std::array<std::int32_t, 16> timer_state{};
        for (auto& value : timer_state) value = in.i32();
        m.timers.import_state(timer_state);
        std::array<std::int32_t, DmaController::state_words> dma_state{};
        for (auto& value : dma_state) value = in.i32();
        m.dma.import_state(dma_state);
        const auto waiting = in.boolean();
        const auto wait_mask = in.i32();
        const auto wait_discard = in.boolean();
        m.bios.restore_wait_state(waiting ? std::optional{Bios::WaitState{wait_mask, wait_discard}} : std::nullopt);
        if (in.i32() != static_cast<int>(m.cartridge.save_type())) {
            throw SaveStateError("État GBA issu d'un autre type de mémoire de sauvegarde");
        }
        if (auto* save = m.cartridge.save()) save->load_state(in);
        auto* gpio = m.cartridge.gpio();
        const auto saved_gpio = in.boolean();
        // Un état antérieur au moteur de vibration n'a pas de GPIO : la broche
        // repart au repos. Toute autre différence trahit un état d'une autre ROM.
        if (saved_gpio != (gpio != nullptr) && !(gpio && !saved_gpio && !gpio->has_rtc()))
            throw SaveStateError("État GBA corrompu (GPIO)");
        if (gpio && saved_gpio) {
            std::array<std::int32_t, Gpio::state_words> gpio_state{};
            for (auto& value : gpio_state) value = in.i32();
            gpio->import_state(gpio_state);
        }
        m.apu.load(in);
        if (version >= 12) {
            m.bus.bios_latch = in.u32();
            m.bus.executing_bios = in.boolean();
            Cpu::Pipeline pipeline;
            pipeline.opcodes[0] = in.i32();
            pipeline.opcodes[1] = in.i32();
            pipeline.valid = in.boolean();
            pipeline.thumb = in.boolean();
            m.cpu.restore_pipeline(pipeline);
            m.bus.last_fetch = in.u32();
            m.serial.import_remaining(in.i32());
            m.bus.restore_sequential_address(in.i32());
        }
        // Avant le format 13, les capteurs repartent de leur état initial.
        if (version >= 13) {
            std::array<std::int32_t, Cartridge::sensor_state_words> sensors{};
            for (auto& value : sensors) value = in.i32();
            m.cartridge.import_sensor_state(sensors);
        }
        // Avant le format 14, le tampon de préchargement repart vide.
        if (version >= 14) {
            Bus::Prefetch prefetch;
            prefetch.head = in.u32();
            prefetch.count = in.i32();
            prefetch.progress = in.i32();
            prefetch.active = in.boolean();
            m.bus.restore_prefetch(prefetch);
            std::array<std::int32_t, 4> delays{};
            for (auto& value : delays) value = in.i32();
            m.timers.import_start_delay(delays);
        }
        // Avant le format 14, aucun transfert ne passe par le câble.
        std::array<std::int32_t, Serial::link_state_words> link{Serial::idle};
        if (version >= 14)
            for (auto& value : link) value = in.i32();
        m.serial.import_link_state(link);
        // Avant le format 15, une recharge écrite servait aussitôt, une IRQ
        // en attente se prenait sans délai, et une fonction du BIOS durait
        // le temps de son SWI.
        if (version >= 15) {
            std::array<std::int32_t, 4> reloads{};
            for (auto& value : reloads) value = in.i32();
            m.timers.import_pending_reloads(reloads);
            m.restore_irq_line_age(in.i32());
            // Avant le format 16, un Halt du BIOS rend la main dès son réveil.
            std::array<std::int32_t, 3> busy{};
            for (auto& value : std::span(busy).first(version >= 16 ? 3 : 2)) value = in.i32();
            m.restore_bios_busy(busy);
        } else {
            m.restore_irq_line_age(m.interrupts.pending() ? Machine::irq_sync_cycles : -1);
        }
        // Avant le format 16, SIOMULTI0-3 étaient rangés avec SIODATA32.
        std::array<std::int32_t, 4> multiplayer{};
        for (std::size_t index = 0; index < multiplayer.size(); ++index)
            multiplayer[index] = version >= 16 ? in.i32() : m.bus.io[0x120 + 2 * index] | m.bus.io[0x121 + 2 * index] << 8;
        m.serial.import_multiplayer_data(multiplayer);
        if (!in.exhausted()) throw SaveStateError("État GBA corrompu (données excédentaires)");
        if (auto* save = m.cartridge.save()) {
            const auto* previous = machine_->cartridge.save();
            save->mark_restored_after(previous ? previous->generation() : 0);
        }
        configure_measurement(m, measuring_time_);
        install(std::move(replacement));
    }

private:
    static constexpr std::uint16_t state_version = 16;

    template <std::size_t N>
    static void read_array(BinaryReader& in, std::array<std::uint8_t, N>& output) {
        in.raw(std::span<std::uint8_t>{output});
    }
    static int io_half(const Bus& bus, int offset) noexcept {
        return bus.io[static_cast<std::size_t>(offset)] | bus.io[static_cast<std::size_t>(offset + 1)] << 8;
    }
    static std::int32_t io_word(const Bus& bus, int offset) noexcept {
        return i32(static_cast<std::uint32_t>(io_half(bus, offset)) |
                   static_cast<std::uint32_t>(io_half(bus, offset + 2)) << 16U);
    }
    static std::int32_t signed28(std::int32_t value) noexcept { return sign_extend(u32(value) & 0x0fff'ffffU, 28); }
    void configure_measurement(Machine& machine, bool value) const noexcept {
        machine.bus.diagnostics.measuring_time = value;
        machine.apu.set_measuring(value);
        machine.apu.on_batch_nanos = [&machine](std::int64_t nanos) {
            machine.bus.diagnostics.apu_nanos += nanos;
        };
        machine.ppu.collect_layer_stats = value;
        machine.ppu.collect_frame_stats = value;
    }
    [[nodiscard]] std::int64_t current_epoch() const noexcept {
        if (clock_override_) return *clock_override_;
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    [[nodiscard]] std::unique_ptr<Machine> new_machine(RomImage rom, const RomTraits& traits) const {
        auto machine = std::make_unique<Machine>(rom, traits, forced_, forced_rtc_, [this] { return current_epoch(); });
        machine->cartridge.set_sensor_input(sensor_input_);
        return machine;
    }
    void apply_sensor_input() noexcept {
        if (machine_) machine_->cartridge.set_sensor_input(sensor_input_);
    }
    void require_loaded() const {
        if (!machine_) throw std::logic_error("Aucune ROM chargée");
    }
    // L'ancienne machine disparaît : le câble distant oublie son port sans y toucher.
    void install(std::unique_ptr<Machine> replacement) noexcept {
        if (remote_) remote_->forget();
        machine_ = std::move(replacement);
    }
    // Une image par tranches, comme le câble local : entre deux, la console
    // répond aux transferts du partenaire distant, au plus tard une tranche
    // après. Un enfant trop en avance sur le parent l'attend d'abord.
    void run_remote_frame() {
        auto& machine = *machine_;
        remote_->attach(machine.serial);
        machine.bus.diagnostics.begin_frame();
        constexpr auto frame_cycles = Ppu::cycles_per_frame();
        auto position = frame_cycles - machine.ppu.cycles_until_next_frame();
        while (position < frame_cycles) {
            remote_->keep_pace();
            remote_->service();
            position += machine.run_for(std::min(remote_slice, frame_cycles - position));
        }
    }

    static constexpr int remote_slice = 1024;
    // Trois secondes sans usage du port : le jeu ne cherche plus de partenaire.
    static constexpr int link_use_frames = 180;
    int frames_since_link_use_{link_use_frames};
    std::optional<GbaSaveType> forced_;
    std::optional<bool> forced_rtc_;
    SensorInput sensor_input_{};
    std::unique_ptr<Machine> machine_;
    RomImage loaded_rom_;
    RomTraits rom_traits_{};
    std::array<std::uint8_t, 32> rom_hash_{};
    std::optional<std::int64_t> clock_override_;
    bool measuring_time_{};
    // Après la machine : détruit avant elle, il rend son port seul.
    std::unique_ptr<RemoteSerialLink> remote_;
};

/**
 * Câble link GBA. Les machines avancent par tranches de `slice` cycles, à
 * tour de rôle, la console qui a lancé le dernier transfert en premier : les
 * autres apprennent ainsi un départ avant de l'atteindre. Un transfert
 * multijoueur dure plus qu'une tranche, si bien que chaque console lit les
 * autres et reçoit le résultat au cycle exact de son horloge. En mode normal,
 * un transfert plus court qu'une tranche lit le partenaire là où il en est.
 */
class GbaLinkCable final : public LinkCable {
public:
    static constexpr int slice = 1024;
    explicit GbaLinkCable(std::vector<GbaCore*> players) : players_(std::move(players)) {}
    GbaLinkCable(const GbaLinkCable&) = delete;
    GbaLinkCable& operator=(const GbaLinkCable&) = delete;
    // Débranché pour de bon : chaque console relit SIOCNT comme sans câble.
    ~GbaLinkCable() override {
        for (auto* player : players_) player->unplug_link();
    }

    [[nodiscard]] std::size_t players() const noexcept override { return players_.size(); }
    void run_frame(std::span<const std::span<std::int32_t>> framebuffers, bool render_video) override {
        if (framebuffers.size() != players_.size()) throw std::invalid_argument("Une image par console reliée");
        constexpr auto frame_cycles = Ppu::cycles_per_frame();
        const auto count = players_.size();
        std::array<Machine*, LinkHub::max_players> machines{};
        std::array<int, LinkHub::max_players> position{};
        for (std::size_t index = 0; index < count; ++index)
            machines[index] = &players_[index]->machine_for_frame(framebuffers[index]);
        // Chaque machine reprend là où sa trame en est ; l'instant 0 du câble
        // est le début de cette trame, pour toutes.
        for (std::size_t index = 0; index < count; ++index) {
            auto& machine = *machines[index];
            position[index] = frame_cycles - machine.ppu.cycles_until_next_frame();
            hub_.attach(static_cast<int>(index), machine.serial, machine.serial.clock() - position[index]);
            machine.ppu.render_enabled = render_video;
            machine.bus.diagnostics.begin_frame();
        }
        hub_.refresh();
        try {
            for (auto end = slice;; end += slice) {
                const auto target = std::min(end, frame_cycles);
                // Ordre fixé pour la tranche : un transfert lancé pendant
                // celle-ci ne réordonne que la suivante.
                const auto first = static_cast<std::size_t>(hub_.first());
                for (std::size_t turn = 0; turn < count; ++turn) {
                    const auto index = (first + turn) % count;
                    if (position[index] < target) position[index] += machines[index]->run_for(target - position[index]);
                }
                if (target == frame_cycles) break;
            }
        } catch (...) {
            release(machines);
            throw;
        }
        release(machines);
        if (!render_video) return;
        for (std::size_t index = 0; index < count; ++index)
            std::copy(machines[index]->ppu.frame.begin(), machines[index]->ppu.frame.end(), framebuffers[index].begin());
    }

private:
    // Entre deux images, les machines ne sont plus reliées : un cœur peut
    // changer de ROM ou d'état, sa nouvelle machine est reprise à l'image suivante.
    void release(const std::array<Machine*, LinkHub::max_players>& machines) noexcept {
        for (auto* machine : machines)
            if (machine) machine->ppu.render_enabled = true;
        hub_.detach_all();
    }
    std::vector<GbaCore*> players_;
    LinkHub hub_;
};

std::unique_ptr<LinkCable> GbaCore::create_link_cable(std::span<Core* const> players) {
    if (players.size() < 2 || players.size() > LinkHub::max_players)
        throw std::invalid_argument("Un câble link GBA relie deux à quatre consoles");
    std::vector<GbaCore*> cores;
    for (auto* player : players) {
        auto* core = dynamic_cast<GbaCore*>(player);
        if (core == nullptr) throw std::invalid_argument("Un câble link GBA ne relie que des GBA");
        cores.push_back(core);
    }
    return std::make_unique<GbaLinkCable>(std::move(cores));
}

}  // namespace gba

std::unique_ptr<Core> make_gba_core(std::optional<GbaSaveType> forced_save_type) {
    return std::make_unique<gba::GbaCore>(forced_save_type);
}
}  // namespace ravenemu
