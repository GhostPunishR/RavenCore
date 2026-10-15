#pragma once

#include "support/bits.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace ravenemu::gba {

/**
 * Capteurs de cartouche décrits par GBATEK (« GBA Cart Solar Sensor », « Gyro
 * Sensor » et « Tilt Sensor »). Le frontend fournit la grandeur physique ; le
 * composant la convertit comme le matériel, au moment où le jeu la mesure.
 */
/** Grandeurs physiques reçues du frontend, dans les unités de l'API (voir Core). */
struct SensorInput {
    int light{};
    int rotation{};
    int tilt_x{};
    int tilt_y{};
};

namespace sensor_pins {
// Une broche du GPIO en entrée n'est plus pilotée : le composant garde le
// dernier niveau reçu, comme l'horloge Seiko.
[[nodiscard]] inline int level(int data, int direction, int bit, int previous) noexcept {
    return (direction & (1 << bit)) != 0 ? (data >> bit) & 1 : previous;
}
} // namespace sensor_pins

/**
 * Capteur solaire de Boktai 1 à 3. Un compteur 74LV4040 piloté par le GPIO
 * alimente par ses 8 bits bas un convertisseur numérique-analogique ; un
 * comparateur lève la broche 3 dès que cette tension dépasse celle de la
 * photodiode. Le jeu compte les impulsions nécessaires : E8h dans
 * l'obscurité, 50h au maximum de la jauge de Boktai.
 *
 * Broche 0 : horloge, le compteur avance sur le front descendant ; broche 1 :
 * remise à zéro, active à l'état haut ; broche 3 : sortie du comparateur. Les
 * broches 0 à 2 servent aussi à l'horloge Seiko, que le compteur ignore.
 */
class SolarSensor {
public:
    static constexpr int state_words = 3;
    static constexpr int darkness = 0xe8;
    static constexpr int full_sun = 0x50;

    /** Lumière reçue : 0 (obscurité) à 255 (plein soleil). */
    void set_light(int level) noexcept {
        level = std::clamp(level, 0, 255);
        threshold_ = darkness - ((darkness - full_sun) * level + 127) / 255;
    }
    void write(int data, int direction) noexcept {
        const auto clock = sensor_pins::level(data, direction, 0, clock_);
        const auto reset = sensor_pins::level(data, direction, 1, reset_);
        if (reset != 0) counter_ = 0;
        else if (clock_ == 1 && clock == 0) counter_ = (counter_ + 1) & 0xfff;
        clock_ = clock;
        reset_ = reset;
    }
    /** Broche 3 : 1 quand la tension du compteur dépasse celle de la photodiode. */
    [[nodiscard]] bool comparator() const noexcept { return (counter_ & 0xff) > threshold_; }

    [[nodiscard]] std::array<std::int32_t, state_words> export_state() const noexcept {
        return {counter_, clock_, reset_};
    }
    void import_state(std::span<const std::int32_t> values) {
        if (values.size() != state_words || values[0] < 0 || values[0] > 0xfff ||
            values[1] < 0 || values[1] > 1 || values[2] < 0 || values[2] > 1) {
            throw SaveStateError("État du capteur solaire GBA invalide");
        }
        counter_ = values[0]; clock_ = values[1]; reset_ = values[2];
    }

private:
    int threshold_{darkness};
    int counter_{};
    int clock_{};
    int reset_{};
};

/**
 * Gyroscope de WarioWare: Twisted!, mesuré par un convertisseur série 12 bits
 * relié au GPIO. Un front montant sur la broche 0 lance une conversion ;
 * chaque front descendant de l'horloge (broche 1) fait avancer le flux lu sur
 * la broche 2 : 4 bits nuls, les 12 bits de la mesure, du poids fort au poids
 * faible, puis des zéros. La broche 3 commande le moteur de vibration.
 *
 * Au repos la mesure vaut 6C0h ; elle monte quand la console tourne dans le
 * sens horaire, jusqu'à 9E3h pour une rotation brutale, et descend dans
 * l'autre sens jusqu'à 354h (valeurs relevées par GBATEK).
 */
class GyroSensor {
public:
    static constexpr int state_words = 4;
    static constexpr int at_rest = 0x6c0;
    static constexpr int fastest_clockwise = 0x9e3;
    static constexpr int fastest_anticlockwise = 0x354;

    /** Vitesse de rotation autour de l'axe de l'écran : -32768..32767, positive dans le sens horaire. */
    void set_rotation(int speed) noexcept {
        speed = std::clamp(speed, -32768, 32767);
        input_ = speed >= 0 ? at_rest + speed * (fastest_clockwise - at_rest) / 32767
                            : at_rest + speed * (at_rest - fastest_anticlockwise) / 32768;
    }
    void write(int data, int direction) noexcept {
        const auto start = sensor_pins::level(data, direction, 0, start_);
        const auto clock = sensor_pins::level(data, direction, 1, clock_);
        if (start_ == 0 && start == 1) {
            sample_ = input_;
            bit_ = 0;
        } else if (clock_ == 1 && clock == 0 && bit_ < stream_bits) {
            ++bit_;
        }
        start_ = start;
        clock_ = clock;
    }
    /** Broche 2 : bit courant du flux série. */
    [[nodiscard]] int data_out() const noexcept {
        if (bit_ < dummy_bits || bit_ >= stream_bits) return 0;
        return (sample_ >> (stream_bits - 1 - bit_)) & 1;
    }

    [[nodiscard]] std::array<std::int32_t, state_words> export_state() const noexcept {
        return {sample_, bit_, start_, clock_};
    }
    void import_state(std::span<const std::int32_t> values) {
        if (values.size() != state_words || values[0] < 0 || values[0] > 0xfff ||
            values[1] < 0 || values[1] > stream_bits || values[2] < 0 || values[2] > 1 ||
            values[3] < 0 || values[3] > 1) {
            throw SaveStateError("État du gyroscope GBA invalide");
        }
        sample_ = values[0]; bit_ = values[1]; start_ = values[2]; clock_ = values[3];
    }

private:
    static constexpr int dummy_bits = 4;
    static constexpr int stream_bits = 16;
    int input_{at_rest};
    int sample_{};
    int bit_{};
    int start_{};
    int clock_{};
};

/**
 * Capteur d'inclinaison de Yoshi Topsy-Turvy et Koro Koro Puzzle, logé dans
 * la moitié haute de la zone SRAM. Écrire 55h en E008000h puis AAh en
 * E008100h lance la mesure des deux axes, immédiate ici ; les valeurs sur
 * 12 bits se lisent en E008200h..E008500h, le bit 7 de E008300h signalant une
 * mesure prête.
 *
 * GBATEK relève X entre 2AFh et 477h autour de 392h, Y entre 2C3h et 480h
 * autour de 3A0h : on prend ±E0h pour ±1 g. Le sens des axes reste à
 * confirmer en jeu.
 */
class TiltSensor {
public:
    static constexpr int state_words = 3;
    static constexpr int center_x = 0x392;
    static constexpr int center_y = 0x3a0;
    static constexpr int units_per_g = 0xe0;

    /** Décalage, dans les 64 Kio de la zone SRAM, d'un registre lisible du capteur. */
    [[nodiscard]] static bool reads(int offset) noexcept {
        return offset >= 0x8200 && offset <= 0x8500 && (offset & 0xff) == 0;
    }
    [[nodiscard]] static bool writes(int offset) noexcept { return offset == 0x8000 || offset == 0x8100; }

    /** Inclinaison : -32768..32767 par axe pour -1 g..1 g. */
    void set_tilt(int x, int y) noexcept {
        input_x_ = center_x + scale(x);
        input_y_ = center_y + scale(y);
    }
    [[nodiscard]] int read(int offset) const noexcept {
        switch (offset) {
        case 0x8200: return x_ & 0xff;
        case 0x8300: return ((x_ >> 8) & 0x0f) | 0x80;
        case 0x8400: return y_ & 0xff;
        case 0x8500: return (y_ >> 8) & 0x0f;
        default: return 0;
        }
    }
    void write(int offset, int value) noexcept {
        value &= 0xff;
        if (offset == 0x8000) {
            armed_ = value == 0x55;
        } else if (offset == 0x8100) {
            if (armed_ && value == 0xaa) { x_ = input_x_; y_ = input_y_; }
            armed_ = false;
        }
    }

    [[nodiscard]] std::array<std::int32_t, state_words> export_state() const noexcept {
        return {x_, y_, armed_ ? 1 : 0};
    }
    void import_state(std::span<const std::int32_t> values) {
        if (values.size() != state_words || values[0] < 0 || values[0] > 0xfff ||
            values[1] < 0 || values[1] > 0xfff || values[2] < 0 || values[2] > 1) {
            throw SaveStateError("État du capteur d'inclinaison GBA invalide");
        }
        x_ = values[0]; y_ = values[1]; armed_ = values[2] != 0;
    }

private:
    [[nodiscard]] static int scale(int value) noexcept {
        return std::clamp(value, -32768, 32767) * units_per_g / 32768;
    }
    int input_x_{center_x};
    int input_y_{center_y};
    int x_{center_x};
    int y_{center_y};
    bool armed_{};
};

} // namespace ravenemu::gba
