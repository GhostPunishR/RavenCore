#pragma once

#include "memory/bus.hpp"

#include <limits>

namespace ravenemu::gba {

/**
 * Port série sans câble branché (GBATEK, « GBA Communication Ports »).
 *
 * Un jeu qui lance un transfert puis attend sa fin restait bloqué : le bit
 * Start de SIOCNT ne retombait jamais. Sans partenaire, le matériel termine
 * pourtant seul les transferts qu'il cadence :
 * - mode normal, horloge interne : 8 ou 32 bits à 256 kHz ou 2 MHz ; la ligne
 *   SI reste haute, si bien que les données reçues valent toutes 1 ;
 * - mode multijoueur : sans câble, la console est parente ; elle reçoit
 *   0xFFFF de chaque enfant absent et garde sa propre donnée dans SIOMULTI0.
 * Avec une horloge externe (mode normal enfant), rien ne se passe : sans
 * partenaire pour cadencer, le transfert attend, comme sur la console.
 */
class Serial {
public:
    static constexpr int idle = std::numeric_limits<int>::max();

    Serial(Bus& bus, InterruptController& interrupts) : bus_(bus), interrupts_(interrupts) {}

    void control_write(int value) {
        if ((value & 0x80) == 0 || remaining_ != idle) return;
        if ((io_half(0x134) & 0x8000) != 0) return;  // RCNT : modes GPIO / JOY Bus
        switch ((value >> 12) & 3) {
        case 0: case 1:  // normal, 8 ou 32 bits
            if ((value & 1) == 0) return;  // horloge externe : attend un partenaire
            remaining_ = ((value & 0x1000) != 0 ? 32 : 8) * ((value & 2) != 0 ? 8 : 64);
            break;
        case 2: {  // multijoueur : 1 bit de départ, 16 de données, 1 d'arrêt
            static constexpr int cycles_per_bit[]{1748, 437, 291, 146};  // 9600..115200 bauds
            remaining_ = 18 * cycles_per_bit[value & 3];
            break;
        }
        default: return;  // UART : non émulé
        }
    }
    [[nodiscard]] int cycles_until_completion() const noexcept { return remaining_; }
    void tick(int cycles) {
        if (remaining_ == idle) return;
        remaining_ -= cycles;
        if (remaining_ <= 0) complete();
    }
    [[nodiscard]] int export_state() const noexcept { return remaining_; }
    void import_state(int remaining) {
        if (remaining != idle && (remaining <= 0 || remaining > 18 * 1748)) throw SaveStateError("État série GBA invalide");
        remaining_ = remaining;
    }

private:
    void complete() {
        remaining_ = idle;
        const auto control = io_half(0x128);
        if (((control >> 12) & 3) == 2) {
            write_io_half(0x120, io_half(0x12a));
            write_io_half(0x122, 0xffff); write_io_half(0x124, 0xffff); write_io_half(0x126, 0xffff);
        } else if ((control & 0x1000) != 0) {
            write_io_half(0x120, 0xffff); write_io_half(0x122, 0xffff);
        } else {
            bus_.io[0x12a] = 0xff;
        }
        write_io_half(0x128, control & ~0x80);
        if ((control & 0x4000) != 0) interrupts_.request(serial_interrupt);
    }
    [[nodiscard]] int io_half(int offset) const noexcept {
        return bus_.io[static_cast<std::size_t>(offset)] | bus_.io[static_cast<std::size_t>(offset + 1)] << 8;
    }
    void write_io_half(int offset, int value) noexcept {
        bus_.io[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value);
        bus_.io[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>(value >> 8);
    }

    static constexpr int serial_interrupt = 7;
    Bus& bus_;
    InterruptController& interrupts_;
    int remaining_{idle};
};

inline void serial_control_write(Serial* serial, int value) {
    if (serial) serial->control_write(value);
}

} // namespace ravenemu::gba
