#pragma once

#include "support/bits.hpp"

namespace ravenemu::gba {

class Rtc {
public:
    using Clock = std::function<std::int64_t()>;
    // Appelé quand le jeu change un réglage que la pile du composant conserve.
    using Changed = std::function<void()>;
    // Réglages enregistrés avec la sauvegarde : bloc de 16 octets de mGBA.
    static constexpr std::size_t battery_size = 16;

    explicit Rtc(Clock clock, Changed changed = {}) : clock_(std::move(clock)), changed_(std::move(changed)) {}

    int pin_state(int data, int direction) const noexcept {
        auto result = data & direction;
        if ((direction & 2) == 0) result |= sio_out_ << 1;
        return result & 0x0f;
    }
    void write(int data, int direction) {
        // SCK/CS sont des entrées du composant : un GPIO relâché ne doit pas
        // maintenir une ancienne sortie haute. SIO est tenu haut hors lecture.
        const auto new_sck = (data & direction) & 1;
        const auto new_sio = (direction & 2) ? (data >> 1) & 1 : sio_out_;
        const auto new_cs = ((data & direction) >> 2) & 1;
        if (new_cs == 0) { phase_ = idle; sio_out_ = 1; }
        else if (chip_select_ == 0) {
            phase_ = command_phase;
            shift_ = 0;
            bit_index_ = 0;
        } else if (sck_ == 0 && new_sck == 1) on_rise(new_sio);
        else if (sck_ == 1 && new_sck == 0) on_fall();
        sck_ = new_cs ? new_sck : 1;
        sio_ = new_sio;
        chip_select_ = new_cs;
    }
    std::array<std::int32_t, 20> export_state() const noexcept {
        std::array<std::int32_t, 20> result{
            sck_, sio_, chip_select_, sio_out_, phase_, shift_, bit_index_, command_,
            byte_index_, byte_count_, status_, static_cast<std::int32_t>(static_cast<std::uint64_t>(offset_seconds_) >> 32U),
            static_cast<std::int32_t>(offset_seconds_),
        };
        std::copy(transfer_.begin(), transfer_.end(), result.begin() + 13);
        return result;
    }
    void import_state(std::span<const std::int32_t> values) {
        if (values.size() != 20) throw SaveStateError("État RTC GBA invalide");
        sck_ = values[0]; sio_ = values[1]; chip_select_ = values[2]; sio_out_ = values[3];
        phase_ = values[4]; shift_ = values[5]; bit_index_ = values[6]; command_ = values[7];
        byte_index_ = values[8]; byte_count_ = values[9]; status_ = values[10];
        offset_seconds_ = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(u32(values[11])) << 32U) | u32(values[12]));
        std::copy_n(values.begin() + 13, 7, transfer_.begin());
        validate_state();
        status_ &= writable_status;
    }

    /**
     * Réglages de l'horloge à placer après la mémoire de sauvegarde, comme le
     * fait mGBA : date et heure du jeu en BCD (année, mois, jour, jour de la
     * semaine, heure, minute, seconde), registre d'état, puis l'heure de
     * l'appareil au même instant (secondes Unix, 64 bits petit-boutiste).
     * L'écart entre les deux heures est celui qu'a réglé le jeu.
     */
    [[nodiscard]] std::array<std::uint8_t, battery_size> export_battery() const {
        const auto host = clock_();
        const auto now = local_time(host + offset_seconds_);
        std::array<std::uint8_t, battery_size> result{};
        const std::array<int, 8> fields{bcd((now.tm_year + 1900) % 100),
                                        bcd(now.tm_mon + 1),
                                        bcd(now.tm_mday),
                                        now.tm_wday,
                                        bcd(now.tm_hour),
                                        bcd(now.tm_min),
                                        bcd(now.tm_sec),
                                        status_};
        std::copy(fields.begin(), fields.end(), result.begin());
        for (unsigned byte = 0; byte < 8; ++byte)
            result[8 + byte] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(host) >> (8U * byte));
        return result;
    }
    /** Relit un bloc d'[export_battery] ; un bloc vierge ou incohérent est ignoré. */
    void import_battery(std::span<const std::uint8_t> bytes) {
        if (bytes.size() != battery_size) return;
        std::uint64_t latch{};
        for (unsigned byte = 0; byte < 8; ++byte) latch |= std::uint64_t{bytes[8 + byte]} << (8U * byte);
        const auto host = static_cast<std::int64_t>(latch);
        const auto field = [&](std::size_t index, int minimum, int maximum) {
            const int value = bytes[index];
            const bool digits = (value & 15) <= 9 && (value >> 4) <= 9;
            return digits && from_bcd(value) >= minimum && from_bcd(value) <= maximum ? from_bcd(value) : -1;
        };
        std::tm date{};
        date.tm_year = 100 + field(0, 0, 99);
        date.tm_mon = field(1, 1, 12) - 1;
        date.tm_mday = field(2, 1, 31);
        date.tm_hour = field(4, 0, 23);
        date.tm_min = field(5, 0, 59);
        date.tm_sec = field(6, 0, 59);
        if (date.tm_year < 100 || date.tm_mon < 0 || date.tm_mday < 1 || date.tm_hour < 0 || date.tm_min < 0 ||
            date.tm_sec < 0 || host < -max_offset || host > max_offset)
            return;
        const auto epoch = wall_clock_epoch(date, host);
        if (!epoch || *epoch - host < -max_offset || *epoch - host > max_offset) return;
        offset_seconds_ = *epoch - host;
        // FlashGBX remplit cet octet de 0x01, valeur impossible sur le composant.
        if (bytes[7] != 1) status_ = bytes[7] & writable_status;
    }

    /** Ce que la pile du composant conserve, transmis tel quel d'une machine à la suivante. */
    struct Settings {
        std::int64_t offset_seconds{};
        int status{0x40};
    };
    [[nodiscard]] Settings settings() const noexcept { return {offset_seconds_, status_}; }
    void restore(const Settings& value) noexcept {
        offset_seconds_ = value.offset_seconds;
        status_ = value.status;
    }

private:
    // Une heure lointaine de plusieurs millénaires reste représentable
    // sans que `clock_() + offset_seconds_` puisse déborder.
    static constexpr std::int64_t max_offset = std::int64_t{1} << 40;
    static constexpr int writable_status = 0x6a; // IRQ fréquence/minute/alarme et mode 24 heures
    static constexpr int idle = 0;
    static constexpr int command_phase = 1;
    static constexpr int reading = 2;
    static constexpr int writing = 3;
    static int reverse_bits(int value) noexcept {
        auto input = static_cast<unsigned>(value) & 0xffU;
        unsigned result{};
        for (unsigned bit = 0; bit < 8; ++bit) result |= ((input >> bit) & 1U) << (7U - bit);
        return static_cast<int>(result);
    }
    // `byte_index_` indexe `transfer_` pendant une lecture ou une écriture
    // série : un état restauré doit respecter les invariants du protocole,
    // faute de quoi le prochain front d'horloge accéderait hors du tableau.
    void validate_state() const {
        const auto bit = [](int value) { return value == 0 || value == 1; };
        const auto in_range = [](auto value, auto minimum, auto maximum) {
            return value >= minimum && value <= maximum;
        };
        const bool transferring = phase_ == reading || phase_ == writing;
        const bool valid = bit(sck_) && bit(sio_) && bit(chip_select_) && bit(sio_out_) &&
            in_range(phase_, idle, writing) && in_range(shift_, 0, 0xff) &&
            in_range(bit_index_, 0, 7) && in_range(command_, 0, 7) &&
            in_range(byte_count_, 0, static_cast<int>(transfer_.size())) &&
            in_range(byte_index_, 0, byte_count_) &&
            (!transferring || byte_index_ < byte_count_) &&
            in_range(status_, 0, 0xff) &&
            in_range(offset_seconds_, -max_offset, max_offset) &&
            std::all_of(transfer_.begin(), transfer_.end(),
                        [&](int value) { return in_range(value, 0, 0xff); });
        if (!valid) throw SaveStateError("État RTC GBA invalide");
    }
    static int bcd(int value) noexcept { return ((value / 10) << 4) | (value % 10); }
    static int from_bcd(int value) noexcept { return ((value >> 4) & 15) * 10 + (value & 15); }
    static std::tm local_time(std::int64_t epoch) {
        const auto seconds = static_cast<std::time_t>(epoch);
        std::tm result{};
#if defined(_WIN32)
        localtime_s(&result, &seconds);
#else
        localtime_r(&seconds, &result);
#endif
        return result;
    }
    std::tm game_time() const { return local_time(clock_() + offset_seconds_); }
    /**
     * Instant où l'heure locale affiche [date]. Au passage à l'heure d'hiver,
     * une même heure revient deux fois : on garde l'instant le plus proche de
     * [near], ce qui rend exact le cas courant d'une horloge jamais réglée.
     */
    static std::optional<std::int64_t> wall_clock_epoch(std::tm date, std::int64_t near) {
        const auto wanted = date;
        const auto shows_wanted = [&](std::int64_t epoch) {
            const auto shown = local_time(epoch);
            return shown.tm_year == wanted.tm_year && shown.tm_mon == wanted.tm_mon &&
                   shown.tm_mday == wanted.tm_mday && shown.tm_hour == wanted.tm_hour &&
                   shown.tm_min == wanted.tm_min && shown.tm_sec == wanted.tm_sec;
        };
        const auto distance = [near](std::int64_t epoch) {
            return epoch > near ? epoch - near : near - epoch;
        };
        std::optional<std::int64_t> best;
        // Demander les deux régimes évite de supposer une transition d'une
        // heure : certains fuseaux décalent l'horloge de trente minutes.
        for (const auto daylight : {-1, 0, 1}) {
            auto candidate = date;
            candidate.tm_isdst = daylight;
            const auto found = std::mktime(&candidate);
            if (found == static_cast<std::time_t>(-1)) continue;
            const auto epoch = static_cast<std::int64_t>(found);
            if (shows_wanted(epoch) && (!best || distance(epoch) < distance(*best))) best = epoch;
        }
        return best;
    }
    // Le jeu a changé un réglage conservé par la pile : la sauvegarde est à réécrire.
    void set_settings(std::int64_t offset, int status) {
        if (offset == offset_seconds_ && status == status_) return;
        offset_seconds_ = offset;
        status_ = status;
        if (changed_) changed_();
    }
    void load_time(const std::tm& value, int at) noexcept {
        const auto hour = (status_ & 0x40) ? value.tm_hour : value.tm_hour % 12;
        transfer_[static_cast<std::size_t>(at)] = bcd(hour) | (value.tm_hour >= 12 ? 0x80 : 0);
        transfer_[static_cast<std::size_t>(at + 1)] = bcd(value.tm_min);
        transfer_[static_cast<std::size_t>(at + 2)] = bcd(value.tm_sec);
    }
    void decode_command() {
        auto byte = shift_ & 0xff;
        if ((byte & 0xf0) != 0x60) byte = reverse_bits(byte);
        if ((byte & 0xf0) != 0x60) { phase_ = idle; bit_index_ = 0; sio_out_ = 1; return; }
        command_ = (byte >> 1) & 7;
        const auto is_reading = (byte & 1) != 0;
        shift_ = 0; bit_index_ = 0; byte_index_ = 0; transfer_.fill(0);
        if (command_ == 0) {
            set_settings(0, 0);
            phase_ = idle;
        } else if (command_ == 1) {
            byte_count_ = 1;
            if (is_reading) transfer_[0] = status_;
            phase_ = is_reading ? reading : writing;
        } else if (command_ == 2 || command_ == 3) {
            const auto now = game_time();
            byte_count_ = command_ == 2 ? 7 : 3;
            if (is_reading && command_ == 2) {
                transfer_[0] = bcd((now.tm_year + 1900) % 100);
                transfer_[1] = bcd(now.tm_mon + 1);
                transfer_[2] = bcd(now.tm_mday);
                transfer_[3] = now.tm_wday;
                load_time(now, 4);
            } else if (is_reading) {
                load_time(now, 0);
            }
            phase_ = is_reading ? reading : writing;
        } else {
            phase_ = idle;
        }
    }
    void on_rise(int bit) {
        if (phase_ == command_phase) {
            shift_ = (shift_ << 1) | bit;
            if (++bit_index_ == 8) decode_command();
        } else if (phase_ == writing) {
            shift_ |= bit << bit_index_;
            if (++bit_index_ == 8) {
                transfer_[static_cast<std::size_t>(byte_index_)] = shift_ & 0xff;
                shift_ = 0; bit_index_ = 0;
                if (++byte_index_ == byte_count_) {
                    apply_written();
                    phase_ = idle;
                }
            }
        }
    }
    void on_fall() noexcept {
        if (phase_ != reading) return;
        sio_out_ = (transfer_[static_cast<std::size_t>(byte_index_)] >> bit_index_) & 1;
        if (++bit_index_ == 8) {
            bit_index_ = 0;
            if (++byte_index_ >= byte_count_) phase_ = idle;
        }
    }
    void apply_written() {
        if (command_ == 1) {
            set_settings(offset_seconds_, transfer_[0] & writable_status);
            return;
        }
        const auto host = clock_();
        auto target = local_time(host + offset_seconds_);
        const auto at = command_ == 2 ? 4U : 0U;
        const auto field = [](int byte, int minimum, int maximum) -> std::optional<int> {
            if ((byte & 15) > 9 || (byte >> 4) > 9) return std::nullopt;
            const auto value = from_bcd(byte);
            return value >= minimum && value <= maximum ? std::optional{value} : std::nullopt;
        };
        if (command_ == 2) {
            const auto year = field(transfer_[0], 0, 99), month = field(transfer_[1], 1, 12), day = field(transfer_[2], 1, 31);
            if (!year || !month || !day || transfer_[3] > 6) return;
            target.tm_year = 100 + *year;
            target.tm_mon = *month - 1;
            target.tm_mday = *day;
        }
        const auto hour = field(transfer_[at] & 0x7f, 0, (status_ & 0x40) ? 23 : 11);
        const auto minute = field(transfer_[at + 1], 0, 59), second = field(transfer_[at + 2], 0, 59);
        if (!hour || !minute || !second) return;
        target.tm_hour = *hour + (!(status_ & 0x40) && (transfer_[at] & 0x80) ? 12 : 0);
        target.tm_min = *minute;
        target.tm_sec = *second;
        const auto epoch = wall_clock_epoch(target, host + offset_seconds_);
        if (epoch && *epoch - host >= -max_offset && *epoch - host <= max_offset)
            set_settings(*epoch - host, status_);
    }

    Clock clock_;
    Changed changed_;
    int sck_{};
    int sio_{};
    int chip_select_{};
    int sio_out_{1};
    int phase_{};
    int shift_{};
    int bit_index_{};
    int command_{};
    int byte_index_{};
    int byte_count_{};
    std::array<int, 7> transfer_{};
    int status_{0x40};
    std::int64_t offset_seconds_{};
};

} // namespace ravenemu::gba
