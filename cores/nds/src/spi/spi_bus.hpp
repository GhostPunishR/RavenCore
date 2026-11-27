#pragma once

#include "cartridge/save_memory.hpp"
#include "interrupts/interrupt_controller.hpp"
#include "machine/scheduler.hpp"
#include "spi/power_management.hpp"
#include "spi/touch_screen.hpp"

#include <cstdint>
#include <span>

// Bus SPI de l'ARM7 (GBATEK, « DS Serial Peripheral Interface Bus (SPI) ») :
// SPICNT (0x040001C0) et SPIDATA (0x040001C2). SPICNT bits 8-9 choisissent
// l'appareil : 0 gestion de l'alimentation, 1 Flash du firmware (puce de
// 256 Kio, celle des sauvegardes de cartouche, save_memory.hpp), 2 écran
// tactile. Chaque écriture de SPIDATA, bus activé (bit 15), échange un octet ;
// sans bit 11, l'appareil est relâché après l'octet. Le bus reste occupé
// (bit 7) 64 << débit cycles (bits 0-1 : 4 MHz à 512 kHz), puis l'IRQ 23 part
// si le bit 14 la demande (Event::spi). Le mode de 16 bits (bit 10), bogué,
// transfère comme celui de 8 bits.
namespace ravenemu::nds {

class SpiBus {
public:
    SpiBus();

    [[nodiscard]] std::uint32_t read(Cycles now) const noexcept;
    void write(std::uint32_t value, std::uint32_t mask, Cycles now, Scheduler& scheduler);
    // Event::spi : fin du transfert ; IRQ si SPICNT bit 14.
    void on_event(InterruptController& irq) const noexcept;

    [[nodiscard]] TouchScreen& touch_screen() noexcept { return touch_; }
    [[nodiscard]] PowerManagement& power() noexcept { return power_; }
    [[nodiscard]] SaveMemory& firmware() noexcept { return firmware_; }

private:
    [[nodiscard]] unsigned device() const noexcept { return (control_ >> 8U) & 3U; }
    void deselect() noexcept;
    void transfer(std::uint8_t value, Cycles now, Scheduler& scheduler);

    PowerManagement power_;
    SaveMemory firmware_;
    TouchScreen touch_;
    std::uint32_t control_{};
    std::uint8_t data_{};
    Cycles busy_until_{};
};

} // namespace ravenemu::nds
