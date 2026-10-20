#pragma once

#include <ravenemu/nds/hardware.hpp>

#include <array>
#include <cstdint>

namespace ravenemu::nds {

// Ordre des bits de InputState::pressed_keys : dix boutons GBA puis X/Y.
// Enum propre à la DS : ne change pas ravenemu::Button ni les autres cœurs.
enum class Key : std::uint8_t { a, b, select, start, right, left, up, down, r, l, x, y };

struct InputState {
    std::uint16_t pressed_keys{}; // 1 = pressé, bits 0..11 ; les autres sont ignorés
    bool pen_down{};             // état /PENIRQ seulement ; pas encore de coordonnées SPI
    bool lid_closed{};
    bool debug_pressed{};
    bool operator==(const InputState&) const = default;
};

/** Entrées partagées, KEYCNT et historique de condition propres à chaque CPU.
 * Les IRQ clavier sont produites sur le front faux->vrai de la combinaison.
 * Écrire KEYCNT change la référence de comparaison, sans produire d'IRQ.
 */
class Input {
public:
    struct Events { std::uint32_t arm9{}, arm7{}; };
    void reset() noexcept { *this = {}; }
    [[nodiscard]] const InputState& state() const noexcept { return state_; }
    [[nodiscard]] Events set_state(InputState state) noexcept;
    [[nodiscard]] Events set_key(Key key, bool pressed);
    [[nodiscard]] std::uint16_t key_input() const noexcept { return static_cast<std::uint16_t>(~state_.pressed_keys & 0x3ffU); }
    [[nodiscard]] std::uint16_t extended_input() const noexcept;
    [[nodiscard]] std::uint16_t control(Processor cpu) const noexcept { return controls_[cpu == Processor::arm9 ? 0 : 1]; }
    void write_control(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept;

private:
    [[nodiscard]] bool matches(unsigned cpu) const noexcept;
    InputState state_;
    std::array<std::uint16_t, 2> controls_{};
    std::array<bool, 2> matched_{};
};

} // namespace ravenemu::nds
