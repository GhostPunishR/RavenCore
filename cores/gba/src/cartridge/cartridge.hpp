#pragma once

#include "cartridge/gpio.hpp"
#include "save/save_factory.hpp"

namespace ravenemu::gba {

class Cartridge {
public:
    static constexpr std::size_t max_rom_size = 0x0200'0000U;
    Cartridge(
        RomImage rom,
        std::optional<GbaSaveType> forced,
        std::optional<bool> forced_rtc,
        Rtc::Clock clock
    )
        : rom_(std::move(rom)),
          save_type_(forced.value_or(detect_save(std::span<const std::uint8_t>{*rom_}))),
          save_(make_save(save_type_)) {
        const auto view = std::span<const std::uint8_t>{*rom_};
        if (view.size() > max_rom_size) throw RomLoadError("ROM GBA trop volumineuse");
        if (view.size() < 0xc0U) throw RomLoadError("ROM GBA trop courte");
        if (view[0xb2] != 0x96U) throw RomLoadError("Marqueur GBA 0x96 absent");
        // La détection cherche la bibliothèque Seiko que les cartouches d'origine
        // embarquent. Elle ne peut rien affirmer d'une ROM modifiée : un jeu peut
        // piloter le composant sans porter cette chaîne, et une ROM peut la
        // contenir sans que le matériel soit présent. Le choix explicite de
        // l'appelant prime donc, dans les deux sens.
        constexpr std::string_view rtc_marker = "SIIRTC_V";
        rtc_detected_ = std::search(view.begin(), view.end(), rtc_marker.begin(), rtc_marker.end()) != view.end();
        if (forced_rtc.value_or(rtc_detected_)) {
            gpio_ = std::make_unique<Gpio>(std::move(clock));
        }
    }
    int read8(int offset) const noexcept {
        const auto index = static_cast<std::size_t>(offset);
        return index < rom_->size() ? (*rom_)[index] : 0;
    }
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
