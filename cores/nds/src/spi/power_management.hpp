#pragma once

#include <array>
#include <cstdint>

// Gestion de l'alimentation du DS (GBATEK, « DS Power Management Device »),
// sur le SPI de l'ARM7, appareil 0 : un octet d'index (bits 0-6 registre,
// bit 7 lecture), puis la donnée lue ou écrite. Registres du premier modèle :
// 0 contrôle (bits 0-6), 1 état de la batterie (lecture seule, 0 = bonne),
// 2 amplificateur du micro (bit 0), 3 gain du micro (bits 0-1) ; les index 4 à
// 7Fh répètent 0 à 3. L'arrêt demandé par le bit 6 n'est pas modélisé.
// Réglage au démarrage, choix de RavenCore : amplificateur du son et deux
// rétroéclairages allumés (0Dh).
namespace ravenemu::nds {

class PowerManagement {
public:
    std::uint8_t transfer(std::uint8_t value) noexcept {
        if (!selected_) {
            selected_ = true;
            index_ = value;
            return 0;
        }
        const auto reg = index_ & 3U;
        if ((index_ & 0x80U) != 0) return registers_[reg];
        static constexpr std::array<std::uint8_t, 4> writable{0x7f, 0x00, 0x01, 0x03};
        registers_[reg] = static_cast<std::uint8_t>(value & writable[reg]);
        return 0;
    }
    void deselect() noexcept { selected_ = false; }
    [[nodiscard]] std::uint8_t control() const noexcept { return registers_[0]; }

private:
    std::array<std::uint8_t, 4> registers_{0x0d, 0x00, 0x00, 0x00};
    bool selected_{};
    std::uint8_t index_{};
};

} // namespace ravenemu::nds
