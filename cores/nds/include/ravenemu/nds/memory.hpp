#pragma once

#include <ravenemu/nds/hardware.hpp>
#include <ravenemu/nds/interrupts.hpp>
#include <ravenemu/nds/ipc.hpp>
#include <ravenemu/nds/dma.hpp>

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace ravenemu::nds {

/** Bus RAM, BIOS et premiers registres MMIO NDS, sans timings ni open-bus.
 * Les accès non mappés ou non alignés lèvent ravenemu::Error.
 * Les rotations des lectures ARM non alignées relèveront du CPU.
 */
class Memory {
public:
    Memory();
    // ARM7 : 16 Kio aux vecteurs bas ; ARM9 : 4 Kio aux vecteurs hauts NDS.
    void load_bios(Processor cpu, std::span<const std::uint8_t> bytes);
    void clear_ram() noexcept; // remet aussi les périphériques à zéro ; conserve les BIOS
    [[nodiscard]] bool dma_pending(Processor cpu) const noexcept;
    // Une unité 16/32 bits, sans cycles. Renvoie false si aucun DMA immédiat en attente.
    // Un échec préserve l'unité fautive ; les unités déjà terminées restent appliquées.
    bool step_dma(Processor cpu);
    // Avance les huit timers d'autant de cycles de timer_clock_hz, sans exécuter les CPU.
    // Les débordements alimentent IF même si IME/IE sont désactivés.
    void advance_cycles(std::uint64_t cycles) noexcept;
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
    void write_io_byte(Processor cpu, std::uint32_t address, std::uint8_t value) noexcept;
    [[nodiscard]] InterruptController& controller(Processor cpu) noexcept;
    [[nodiscard]] const InterruptController& controller(Processor cpu) const noexcept;
    void apply_ipc_interrupts(Ipc::Interrupts sources) noexcept;
    [[nodiscard]] std::uint32_t peek(Processor cpu, std::uint32_t address, unsigned width) const;
    [[nodiscard]] std::uint32_t read(Processor cpu, std::uint32_t address, unsigned width);
    void validate_write(Processor cpu, std::uint32_t address, unsigned width) const;
    void write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width);
    std::vector<std::uint8_t> ram_;
    std::vector<std::uint8_t> arm9_bios_;
    std::vector<std::uint8_t> arm7_bios_;
    InterruptController arm9_interrupts_{Processor::arm9};
    InterruptController arm7_interrupts_{Processor::arm7};
    TimerBank arm9_timers_{};
    TimerBank arm7_timers_{};
    Ipc ipc_;
    DmaController arm9_dma_{Processor::arm9};
    DmaController arm7_dma_{Processor::arm7};
    std::array<std::uint8_t, 16> dma_fill_{};
    std::uint8_t wram_control_{};
};

} // namespace ravenemu::nds
