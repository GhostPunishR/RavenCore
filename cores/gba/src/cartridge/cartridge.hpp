#pragma once

#include "cartridge/gpio.hpp"
#include "save/save_factory.hpp"

namespace ravenemu::gba {

/**
 * Ce que la ROM révèle d'elle-même : type de sauvegarde et présence de la
 * bibliothèque RTC Seiko. La détection parcourt toute la ROM (jusqu'à 32 Mio) ;
 * comme elle ne dépend que de son contenu, le cœur la fait une fois au
 * chargement et la réutilise pour chaque machine (reset, état instantané).
 */
struct RomTraits {
    GbaSaveType save_type{GbaSaveType::none};
    bool rtc{};
    bool rumble{};

    [[nodiscard]] static RomTraits detect(std::span<const std::uint8_t> rom) {
        // La détection cherche la bibliothèque Seiko que les cartouches d'origine
        // embarquent. Elle ne peut rien affirmer d'une ROM modifiée : un jeu peut
        // piloter le composant sans porter cette chaîne, et une ROM peut la
        // contenir sans que le matériel soit présent. Le choix explicite de
        // l'appelant prime donc, dans les deux sens.
        constexpr std::string_view rtc_marker = "SIIRTC_V";
        return {detect_save(rom),
                std::search(rom.begin(), rom.end(), rtc_marker.begin(), rtc_marker.end()) != rom.end(),
                has_rumble_motor(rom)};
    }
    // Moteur de vibration sur la broche 3 du GPIO (GBATEK) : Drill Dozer (V49)
    // et WarioWare: Twisted! (RZW), reconnus par le code de jeu en 0xAC.
    [[nodiscard]] static bool has_rumble_motor(std::span<const std::uint8_t> rom) noexcept {
        if (rom.size() < 0xb0U) return false;
        const std::string_view code(reinterpret_cast<const char*>(rom.data() + 0xac), 3);
        return code == "V49" || code == "RZW";
    }
};

class Cartridge {
public:
    static constexpr std::size_t max_rom_size = 0x0200'0000U;
    Cartridge(
        RomImage rom,
        std::optional<GbaSaveType> forced,
        std::optional<bool> forced_rtc,
        Rtc::Clock clock
    )
        : Cartridge(rom, RomTraits::detect(*rom), forced, forced_rtc, std::move(clock)) {}

    Cartridge(
        RomImage rom,
        RomTraits traits,
        std::optional<GbaSaveType> forced,
        std::optional<bool> forced_rtc,
        Rtc::Clock clock
    )
        : rom_(std::move(rom)),
          save_type_(forced.value_or(traits.save_type)),
          save_(make_save(save_type_)),
          rtc_detected_(traits.rtc) {
        const auto view = std::span<const std::uint8_t>{*rom_};
        if (view.size() > max_rom_size) throw RomLoadError("ROM GBA trop volumineuse");
        if (view.size() < 0xc0U) throw RomLoadError("ROM GBA trop courte");
        if (view[0xb2] != 0x96U) throw RomLoadError("Marqueur GBA 0x96 absent");
        const auto rtc = forced_rtc.value_or(rtc_detected_);
        if (rtc || traits.rumble) gpio_ = std::make_unique<Gpio>(std::move(clock), rtc, traits.rumble);
    }
    int read8(int offset) const noexcept {
        const auto index = static_cast<std::size_t>(offset);
        if (index < rom_->size()) return (*rom_)[index];
        // Au-delà de la ROM, le bus de la cartouche (adresse et données
        // multiplexées) rend l'adresse du demi-mot : (adresse / 2) & 0xFFFF.
        const auto half = (static_cast<unsigned>(offset) >> 1U) & 0xffffU;
        return static_cast<int>((half >> ((static_cast<unsigned>(offset) & 1U) * 8U)) & 0xffU);
    }
    [[nodiscard]] std::span<const std::uint8_t> rom() const noexcept { return *rom_; }
    [[nodiscard]] GbaSaveType save_type() const noexcept { return save_type_; }
    [[nodiscard]] SaveMemory* save() noexcept { return save_.get(); }
    [[nodiscard]] const SaveMemory* save() const noexcept { return save_.get(); }
    [[nodiscard]] Gpio* gpio() noexcept { return gpio_.get(); }
    [[nodiscard]] const Gpio* gpio() const noexcept { return gpio_.get(); }
    /** Vrai si la ROM porte la signature de la bibliothèque Seiko. */
    [[nodiscard]] bool rtc_detected() const noexcept { return rtc_detected_; }

private:
    RomImage rom_;
    GbaSaveType save_type_;
    std::unique_ptr<SaveMemory> save_;
    std::unique_ptr<Gpio> gpio_;
    bool rtc_detected_{};
};

} // namespace ravenemu::gba
