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
    CartridgeSensors sensors{};

    [[nodiscard]] static RomTraits detect(std::span<const std::uint8_t> rom) {
        // La détection cherche la bibliothèque Seiko que les cartouches d'origine
        // embarquent. Elle ne peut rien affirmer d'une ROM modifiée : un jeu peut
        // piloter le composant sans porter cette chaîne, et une ROM peut la
        // contenir sans que le matériel soit présent. Le choix explicite de
        // l'appelant prime donc, dans les deux sens.
        constexpr std::string_view rtc_marker = "SIIRTC_V";
        return {detect_save(rom),
                std::search(rom.begin(), rom.end(), rtc_marker.begin(), rtc_marker.end()) != rom.end(),
                has_rumble_motor(rom), detect_sensors(rom)};
    }
    // Moteur de vibration sur la broche 3 du GPIO (GBATEK) : Drill Dozer (V49)
    // et WarioWare: Twisted! (RZW), reconnus par le code de jeu en 0xAC.
    [[nodiscard]] static bool has_rumble_motor(std::span<const std::uint8_t> rom) noexcept {
        const auto code = game_code(rom);
        return code == "V49" || code == "RZW";
    }
    // Capteurs, reconnus eux aussi par le code de jeu : solaire de Boktai 1 à 3
    // (U3I, U32, U33), gyroscope de WarioWare: Twisted! (RZW), inclinaison de
    // Yoshi Topsy-Turvy (KYG) et de Koro Koro Puzzle (KHP).
    [[nodiscard]] static CartridgeSensors detect_sensors(std::span<const std::uint8_t> rom) noexcept {
        const auto code = game_code(rom);
        return {code == "U3I" || code == "U32" || code == "U33", code == "RZW", code == "KYG" || code == "KHP"};
    }
    // Code de jeu sans la lettre de région : le même matériel dans chaque version.
    [[nodiscard]] static std::string_view game_code(std::span<const std::uint8_t> rom) noexcept {
        if (rom.size() < 0xb0U) return {};
        return {reinterpret_cast<const char*>(rom.data() + 0xac), 3};
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
        const auto& sensors = traits.sensors;
        if (rtc || traits.rumble || sensors.solar || sensors.rotation) {
            // Un réglage d'horloge changé par le jeu est à enregistrer, comme une écriture en mémoire.
            auto changed = [save = save_.get()] {
                if (save) save->mark_modified();
            };
            gpio_ = std::make_unique<Gpio>(std::move(clock), std::move(changed), rtc, traits.rumble, sensors.solar,
                                           sensors.rotation);
        }
        if (sensors.tilt) tilt_ = std::make_unique<TiltSensor>();
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
    [[nodiscard]] Rtc* rtc() noexcept { return gpio_ ? gpio_->rtc() : nullptr; }
    [[nodiscard]] const Rtc* rtc() const noexcept { return gpio_ ? gpio_->rtc() : nullptr; }
    /**
     * Sauvegarde telle que le frontend la conserve : la mémoire de la puce,
     * suivie, sur une cartouche à horloge, des réglages de celle-ci (bloc de
     * 16 octets de mGBA). Vide sans mémoire de sauvegarde.
     */
    [[nodiscard]] std::vector<std::uint8_t> export_battery() const {
        if (!save_) return {};
        auto result = save_->data();
        if (const auto* clock = rtc()) {
            const auto settings = clock->export_battery();
            result.insert(result.end(), settings.begin(), settings.end());
        }
        return result;
    }
    /** Relit [export_battery] ; une sauvegarde sans réglages d'horloge laisse celle-ci à l'heure de l'appareil. */
    void import_battery(std::span<const std::uint8_t> battery) {
        if (!save_) return;
        save_->import(battery);
        const auto size = save_->data().size();
        if (auto* clock = rtc(); clock && battery.size() >= size + Rtc::battery_size)
            clock->import_battery(battery.subspan(size, Rtc::battery_size));
    }
    /**
     * Après un reset, la nouvelle cartouche reprend ce que gardait l'ancienne :
     * mémoire, réglages de l'horloge, et une écriture pas encore enregistrée
     * reste à enregistrer.
     */
    void keep_battery_of(const Cartridge& previous) {
        if (save_ && previous.save_) {
            save_->import(previous.save_->data());
            if (previous.save_->dirty()) save_->mark_restored_after(previous.save_->generation());
        }
        if (auto* clock = rtc(); clock && previous.rtc()) clock->restore(previous.rtc()->settings());
    }
    /** Vrai si la ROM porte la signature de la bibliothèque Seiko. */
    [[nodiscard]] bool rtc_detected() const noexcept { return rtc_detected_; }
    /** Capteur d'inclinaison logé dans la zone SRAM, ou nullptr. */
    [[nodiscard]] TiltSensor* tilt() noexcept { return tilt_.get(); }
    [[nodiscard]] CartridgeSensors sensors() const noexcept {
        return {gpio_ && gpio_->has_solar(), gpio_ && gpio_->has_gyro(), tilt_ != nullptr};
    }
    /** Grandeurs physiques fournies par le frontend ; ignorées sans le capteur. */
    void set_sensor_input(const SensorInput& input) noexcept {
        if (auto* sensor = gpio_ ? gpio_->solar() : nullptr) sensor->set_light(input.light);
        if (auto* sensor = gpio_ ? gpio_->gyro() : nullptr) sensor->set_rotation(input.rotation);
        if (tilt_) tilt_->set_tilt(input.tilt_x, input.tilt_y);
    }

    // État des capteurs (format d'état 13) : GPIO puis inclinaison ; des zéros
    // pour un composant absent.
    static constexpr int sensor_state_words = Gpio::sensor_state_words + TiltSensor::state_words;
    [[nodiscard]] std::array<std::int32_t, sensor_state_words> export_sensor_state() const noexcept {
        std::array<std::int32_t, sensor_state_words> result{};
        if (gpio_) {
            const auto values = gpio_->export_sensor_state();
            std::copy(values.begin(), values.end(), result.begin());
        }
        if (tilt_) {
            const auto values = tilt_->export_state();
            std::copy(values.begin(), values.end(), result.begin() + Gpio::sensor_state_words);
        }
        return result;
    }
    void import_sensor_state(std::span<const std::int32_t> values) {
        if (values.size() != sensor_state_words) throw SaveStateError("État des capteurs GBA invalide");
        // Les valeurs d'un composant absent sont vérifiées puis ignorées : un
        // état incohérent est refusé en entier. Les grandeurs du frontend restent.
        const auto pins = values.first(Gpio::sensor_state_words);
        const auto tilt = values.subspan(Gpio::sensor_state_words);
        if (gpio_) {
            gpio_->import_sensor_state(pins);
        } else {
            SolarSensor{}.import_state(pins.first(SolarSensor::state_words));
            GyroSensor{}.import_state(pins.subspan(SolarSensor::state_words));
        }
        if (tilt_) tilt_->import_state(tilt);
        else TiltSensor{}.import_state(tilt);
    }

private:
    RomImage rom_;
    GbaSaveType save_type_;
    std::unique_ptr<SaveMemory> save_;
    std::unique_ptr<Gpio> gpio_;
    std::unique_ptr<TiltSensor> tilt_;
    bool rtc_detected_{};
};

} // namespace ravenemu::gba
