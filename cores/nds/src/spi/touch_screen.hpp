#pragma once

#include <array>
#include <cstdint>

// Contrôleur de l'écran tactile (GBATEK, « DS Touch Screen Controller (TSC) »,
// TSC2046), sur le SPI de l'ARM7, appareil 2. Un octet de commande (bit 7 de
// départ, voie en bits 4-6, conversion sur 8 bits si bit 3) ; la réponse suit :
// un bit nul, puis le résultat, poids fort en tête, puis des zéros. Une
// nouvelle commande peut partir pendant la réponse.
// Voies : 1 Y et 5 X, d'après le point touché et le calibrage des réglages
// utilisateur (16 pas par point, user_settings.hpp) : (point + 1) × 16 ;
// relâché, X = 000h et Y = FFFh (GBATEK). Choix de RavenCore, faute de
// valeurs dans GBATEK : Z1 = 400h et Z2 = 800h touché, 000h et FFFh relâché ;
// températures 0 et 7 à 800h ; batterie (2) à 000h comme sur la console ;
// micro (6) au milieu, 800h, faute de micro modélisé.
namespace ravenemu::nds {

class TouchScreen {
public:
    // Point touché par l'hôte, en pixels de l'écran du bas.
    void set_touch(bool pressed, unsigned x, unsigned y) noexcept {
        pressed_ = pressed;
        x_ = x < 256 ? x : 255;
        y_ = y < 192 ? y : 191;
    }
    [[nodiscard]] bool pressed() const noexcept { return pressed_; }

    std::uint8_t transfer(std::uint8_t value) noexcept {
        const auto reply = position_ < reply_.size() ? reply_[position_++] : std::uint8_t{0};
        if ((value & 0x80U) != 0) start(value);
        return reply;
    }
    void deselect() noexcept { position_ = static_cast<unsigned>(reply_.size()); }

private:
    [[nodiscard]] std::uint32_t convert(unsigned channel) const noexcept {
        switch (channel) {
        case 1: return pressed_ ? (y_ + 1U) * 16U : 0xfffU;
        case 2: return 0;
        case 3: return pressed_ ? 0x400U : 0U;
        case 4: return pressed_ ? 0x800U : 0xfffU;
        case 5: return pressed_ ? (x_ + 1U) * 16U : 0U;
        default: return 0x800U;
        }
    }
    // Résultat de 12 bits sur 16 : bit nul, 12 bits, 3 zéros ; sur 8 bits :
    // bit nul, 8 bits, 7 zéros.
    void start(std::uint8_t command) noexcept {
        const auto value = convert((command >> 4U) & 7U);
        const auto word = (command & 0x08U) != 0 ? (value >> 4U) << 7U : value << 3U;
        reply_ = {static_cast<std::uint8_t>(word >> 8U), static_cast<std::uint8_t>(word)};
        position_ = 0;
    }

    bool pressed_{};
    unsigned x_{};
    unsigned y_{};
    std::array<std::uint8_t, 2> reply_{};
    unsigned position_{2};
};

} // namespace ravenemu::nds
