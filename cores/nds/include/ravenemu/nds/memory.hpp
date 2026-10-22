#pragma once

#include <ravenemu/nds/hardware.hpp>
#include <ravenemu/nds/cp15.hpp>
#include <ravenemu/nds/interrupts.hpp>
#include <ravenemu/nds/ipc.hpp>
#include <ravenemu/nds/input.hpp>
#include <ravenemu/nds/spi.hpp>
#include <ravenemu/nds/gamecard.hpp>
#include <ravenemu/nds/dma.hpp>
#include <ravenemu/nds/lcd_timing.hpp>
#include <ravenemu/nds/video.hpp>

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace ravenemu::nds {
namespace detail { class DataBus; }

/** Bus RAM, BIOS, TCM ARM9 et premiers registres MMIO, sans timings ni open-bus.
 * Les accès non mappés ou non alignés lèvent ravenemu::Error.
 * Les rotations des lectures ARM non alignées relèveront du CPU.
 */
class Memory {
public:
    Memory();
    void load_gamecard(const Cartridge& cartridge, Gamecard::Mode mode = Gamecard::Mode::main_data);
    [[nodiscard]] std::uint32_t gamecard_chip_id() const noexcept { return gamecard_.chip_id(); }
    void load_backup(BackupType type, std::span<const std::uint8_t> bytes = {}, std::uint8_t status = 0) {
        gamecard_.load_backup(type,bytes,status);
    }
    [[nodiscard]] const Backup& backup() const noexcept { return gamecard_.backup(); }
    [[nodiscard]] bool backup_dirty() const noexcept { return backup().dirty(); }
    [[nodiscard]] BackupSnapshot snapshot_backup() const { return backup().snapshot(); }
    void acknowledge_backup_saved(std::uint64_t generation) noexcept { gamecard_.acknowledge_backup_saved(generation); }
    void set_input(InputState state) noexcept;
    void set_key(Key key, bool pressed);
    // Pixels d'origine (0,0) ; exige un étalonnage valide pour un contact.
    // pressed=false relâche le stylet même sans firmware, en conservant les ADC.
    void set_touch_position(std::uint16_t x, std::uint16_t y, bool pressed = true);
    [[nodiscard]] const InputState& input_state() const noexcept { return input_.state(); }
    void set_battery_low(bool low) noexcept { spi_.set_battery_low(low); }
    [[nodiscard]] const PowerManagement& power_management() const noexcept { return spi_.power_management(); }
    [[nodiscard]] bool powered_off() const noexcept { return power_management().powered_off(); }
    // Contrôle système ARM9 ; sans effet sur les accès ARM7 ou DMA.
    [[nodiscard]] Cp15& cp15() noexcept { return cp15_; }
    [[nodiscard]] const Cp15& cp15() const noexcept { return cp15_; }
    [[nodiscard]] std::uint32_t exception_base(Processor cpu) const noexcept {
        return cpu == Processor::arm9 ? cp15_.exception_base() : 0U;
    }
    // Fetch ARM9 : ITCM visible, DTCM ignorée. Aucun retrait de FIFO MMIO.
    [[nodiscard]] std::uint16_t fetch16(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::uint32_t fetch32(Processor cpu, std::uint32_t address) const;
    // ARM7 : 16 Kio aux vecteurs bas ; ARM9 : 4 Kio aux vecteurs hauts NDS.
    void load_bios(Processor cpu, std::span<const std::uint8_t> bytes);
    void load_firmware(std::span<const std::uint8_t> bytes) { spi_.load_firmware(bytes); }
    [[nodiscard]] const Firmware& firmware() const noexcept { return spi_.firmware(); }
    [[nodiscard]] bool firmware_dirty() const noexcept { return firmware().dirty(); }
    [[nodiscard]] FirmwareSnapshot snapshot_firmware() const { return firmware().snapshot(); }
    void acknowledge_firmware_saved(std::uint64_t generation) noexcept { spi_.acknowledge_firmware_saved(generation); }
    void clear_ram() noexcept; // remet les périphériques à zéro ; conserve BIOS/firmware/cartouche/EEPROM
    [[nodiscard]] VideoFrame render_frame() const { return video_.render_frame(display_control_, graphics_power_, display_control_b_); }
    [[nodiscard]] bool dma_pending(Processor cpu) const noexcept;
    // Une unité 16/32 bits, sans cycles. Renvoie false si aucun DMA déclenché en attente.
    // Un échec préserve l'unité fautive ; les unités déjà terminées restent appliquées.
    bool step_dma(Processor cpu);
    // Avance timers, SPI, LCD, FIFO et capture vidéo, sans exécuter CPU ni unités DMA.
    // Une source vidéo non prise en charge peut lever Error ; les lignes terminées restent appliquées.
    // Les événements LCD peuvent déclencher les DMA VBlank/HBlank/FIFO vidéo.
    void advance_cycles(std::uint64_t cycles);
    [[nodiscard]] std::uint32_t cycles_until_frame() const noexcept { return lcd_.cycles_until_frame(); }
    // Sources : masque de bits IF, pas un numéro de source.
    void request_interrupt(Processor cpu, std::uint32_t sources) noexcept;
    [[nodiscard]] bool irq_pending(Processor cpu) const noexcept;
    [[nodiscard]] std::uint8_t wram_control() const noexcept { return wram_control_; }
    void set_wram_control(std::uint8_t value) noexcept { wram_control_ = value & 3U; }
    // Lectures bus : peuvent retirer un mot de la FIFO IPC et lever des IRQ.
    [[nodiscard]] std::uint8_t read8(Processor cpu, std::uint32_t address);
    [[nodiscard]] std::uint16_t read16(Processor cpu, std::uint32_t address);
    [[nodiscard]] std::uint32_t read32(Processor cpu, std::uint32_t address);
    // Inspection sans effets de bord, y compris sur une FIFO vide.
    [[nodiscard]] std::uint8_t peek8(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::uint16_t peek16(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::uint32_t peek32(Processor cpu, std::uint32_t address) const;
    void write8(Processor cpu, std::uint32_t address, std::uint8_t value);
    void write16(Processor cpu, std::uint32_t address, std::uint16_t value);
    void write32(Processor cpu, std::uint32_t address, std::uint32_t value);
    // Valide toute la plage avant d'écrire : une erreur ne modifie aucun octet.
    void write_words(Processor cpu, std::uint32_t address, std::span<const std::uint32_t> values);
    // Valide toutes les lectures avant leurs effets MMIO ou l'écriture du résultat.
    void read_words(Processor cpu, std::uint32_t address, std::span<std::uint32_t> values);

private:
    friend class Cpu;
    friend class Machine;
    friend class detail::DataBus;
    void complete_boot() noexcept { post_flags_ = {1,1}; }
    [[nodiscard]] bool consume_arm7_halt() noexcept {
        const bool requested = arm7_halt_requested_; arm7_halt_requested_ = false; return requested;
    }
    [[nodiscard]] bool arm7_halt_wake_pending() const noexcept {
        return (arm7_interrupts_.enable() & arm7_interrupts_.flags()) != 0;
    }
    void validate_power_write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width) const;
    [[nodiscard]] std::uint8_t power_byte(Processor cpu, unsigned offset) const noexcept;
    void write_power_byte(Processor cpu, unsigned offset, std::uint8_t value, bool from_bios) noexcept;
    enum class Access { data, instruction, external };
    void advance_clock(std::uint64_t cycles) noexcept; // horloges et événements sans capture
    [[nodiscard]] DmaController& dma(Processor cpu) noexcept;
    [[nodiscard]] const DmaController& dma(Processor cpu) const noexcept;
    struct Timer {
        std::uint16_t counter{}, reload{}, phase{};
        std::uint8_t control{};
    };
    using TimerBank = std::array<Timer, 4>;
    [[nodiscard]] TimerBank& timers(Processor cpu) noexcept;
    [[nodiscard]] const TimerBank& timers(Processor cpu) const noexcept;
    [[nodiscard]] std::uint8_t timer_byte(Processor cpu, unsigned offset) const noexcept;
    void write_timer_byte(Processor cpu, unsigned offset, std::uint8_t value) noexcept;
    [[nodiscard]] static std::uint32_t advance_timers(TimerBank& bank, std::uint64_t cycles) noexcept;
    [[nodiscard]] std::size_t index(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint8_t> bios_byte(Processor cpu, std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint8_t> io_byte(Processor cpu, std::uint32_t address) const noexcept;
    void write_io_byte(Processor cpu, std::uint32_t address, std::uint8_t value, bool from_bios = false) noexcept;
    [[nodiscard]] InterruptController& controller(Processor cpu) noexcept;
    [[nodiscard]] const InterruptController& controller(Processor cpu) const noexcept;
    void apply_ipc_interrupts(Ipc::Interrupts sources) noexcept;
    void apply_input_interrupts(Input::Events events) noexcept;
    [[nodiscard]] Processor gamecard_owner() const noexcept { return (exmem_shared_ & 0x800U) ? Processor::arm7 : Processor::arm9; }
    void apply_gamecard_events(Gamecard::Events events) noexcept;
    void pause_gamecard_dma() noexcept;
    [[nodiscard]] std::uint32_t read_gamecard_data(Processor cpu) noexcept;
    void validate_exmem_write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width) const;
    [[nodiscard]] std::uint32_t peek(Processor cpu, std::uint32_t address, unsigned width, Access access = Access::data) const;
    [[nodiscard]] std::uint32_t read(Processor cpu, std::uint32_t address, unsigned width);
    void validate_write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width, Access access = Access::data) const;
    void write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width,
               Access access = Access::data, bool from_bios = false);
    void write_words(Processor cpu, std::uint32_t address, std::span<const std::uint32_t> values, bool from_bios);
    Cp15 cp15_;
    std::vector<std::uint8_t> ram_;
    std::vector<std::uint8_t> arm9_bios_;
    std::vector<std::uint8_t> arm7_bios_;
    InterruptController arm9_interrupts_{Processor::arm9};
    InterruptController arm7_interrupts_{Processor::arm7};
    TimerBank arm9_timers_{};
    TimerBank arm7_timers_{};
    Ipc ipc_;
    Input input_;
    Spi spi_;
    Gamecard gamecard_;
    std::array<std::uint8_t, 2> exmem_local_{};
    std::uint16_t exmem_shared_{};
    DmaController arm9_dma_{Processor::arm9};
    DmaController arm7_dma_{Processor::arm7};
    std::array<std::uint8_t, 16> dma_fill_{};
    LcdTiming lcd_;
    VideoMemory video_;
    std::uint32_t display_control_{};
    std::uint32_t display_control_b_{};
    std::uint16_t graphics_power_{};
    std::uint8_t wram_control_{};
    std::array<std::uint8_t,2> post_flags_{};
    std::uint8_t halt_control_{};
    bool arm7_halt_requested_{};
};

} // namespace ravenemu::nds
