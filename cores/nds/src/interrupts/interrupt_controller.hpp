#pragma once

#include <cstdint>

// IME, IE et IF d'un processeur (GBATEK, « DS Interrupts »). Chaque CPU a les
// siens ; seuls les bits définis pour ce CPU existent.
namespace ravenemu::nds {

namespace irq {
inline constexpr unsigned vblank = 0;
inline constexpr unsigned hblank = 1;
inline constexpr unsigned vcount = 2;
inline constexpr unsigned timer0 = 3;  // timers 0 à 3 : bits 3 à 6
inline constexpr unsigned rtc = 7;     // ARM7
inline constexpr unsigned dma0 = 8;    // DMA 0 à 3 : bits 8 à 11
inline constexpr unsigned keypad = 12;
inline constexpr unsigned gba_slot = 13;
inline constexpr unsigned ipc_sync = 16;
inline constexpr unsigned ipc_send_empty = 17;
inline constexpr unsigned ipc_receive = 18;
inline constexpr unsigned card_done = 19;
inline constexpr unsigned card_ireq = 20;
inline constexpr unsigned geometry_fifo = 21;  // ARM9
inline constexpr unsigned hinge = 22;          // ARM7
inline constexpr unsigned spi = 23;            // ARM7
inline constexpr unsigned wifi = 24;           // ARM7

// Bits existants : 0-6, 8-13, 16-21 sur l'ARM9 ; 0-13, 16-20, 22-24 sur l'ARM7.
inline constexpr std::uint32_t arm9_bits = 0x003f'3f7fU;
inline constexpr std::uint32_t arm7_bits = 0x01df'3fffU;
} // namespace irq

class InterruptController {
public:
    explicit InterruptController(std::uint32_t valid_bits) noexcept : valid_(valid_bits) {}

    void request(unsigned bit) noexcept { flags_ |= (1U << bit) & valid_; }
    // Une interruption permise attend : fin de Halt.
    [[nodiscard]] bool pending() const noexcept { return (enable_ & flags_) != 0; }
    // Ligne IRQ du CPU : IME, puis IE ET IF.
    [[nodiscard]] bool line() const noexcept { return master_ && pending(); }
    [[nodiscard]] bool master() const noexcept { return master_; }
    void set_master(bool enabled) noexcept { master_ = enabled; }
    [[nodiscard]] std::uint32_t enabled() const noexcept { return enable_; }
    [[nodiscard]] std::uint32_t flags() const noexcept { return flags_; }

    // Mots d'E/S à 0x208 (IME), 0x210 (IE) et 0x214 (IF). Écrire 1 dans IF
    // acquitte le bit.
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const noexcept {
        switch (offset) {
        case 0x208: return master_ ? 1U : 0U;
        case 0x210: return enable_;
        case 0x214: return flags_;
        default: return 0;
        }
    }
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask) noexcept {
        switch (offset) {
        case 0x208:
            if ((mask & 1U) != 0) master_ = (value & 1U) != 0;
            break;
        case 0x210: enable_ = ((enable_ & ~mask) | (value & mask)) & valid_; break;
        case 0x214: flags_ &= ~(value & mask); break;
        default: break;
        }
    }

private:
    std::uint32_t valid_;
    bool master_{};
    std::uint32_t enable_{};
    std::uint32_t flags_{};
};

} // namespace ravenemu::nds
