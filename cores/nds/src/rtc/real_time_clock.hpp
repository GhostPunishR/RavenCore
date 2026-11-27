#pragma once

#include "machine/scheduler.hpp"

#include <array>
#include <cstdint>

// Horloge temps réel du DS (GBATEK, « DS Real-Time Clock (RTC) », Seiko
// S-35180), par le registre 0x04000138 de l'ARM7 : bit 0 données, bit 1
// horloge (/SCK), bit 2 sélection (CS), bits 4-6 leurs directions.
// - CS levé, la commande arrive sur 8 bits, poids faible en tête (fronts
//   montants de /SCK) : bits 0-3 fixés à 0110b, registre en bits 4-6, lecture
//   au bit 7. Une commande envoyée poids fort en tête (bits 4-7 à 0110b) est
//   retournée. Les paramètres suivent, poids faible en tête : la puce sort
//   chaque bit au front descendant ; un bit de données en entrée (bit 4 à 0)
//   le lit.
// - Registres : 0 état 1 (bit 0 remise à zéro, bit 1 mode 24 h, bits 4-7
//   drapeaux effacés à la lecture), 4 état 2, 2 date et heure (7 octets BCD :
//   année, mois, jour, jour de la semaine, heure, minute, seconde), 6 heure
//   (3 octets), 1 INT1 (fréquence, ou alarme 1 si état 2 bit 2), 5 alarme 2,
//   3 réglage, 7 registre libre. Heure : bit 6 = après-midi ; en mode 12 h,
//   midi s'écrit 00h.
// L'heure avance avec le temps émulé (33 513 982 cycles par seconde) depuis
// la date réglée par l'hôte ou par le jeu : l'émulation reste déterministe.
// Jour de la semaine : 0 = lundi, choix de RavenCore (GBATEK : « usually
// 0=Monday? ») ; une écriture du jeu en décale le compte. Au démarrage :
// mode 24 h, sans drapeau, 1er janvier 2000 à 0 h tant que l'hôte ne règle
// rien.
// Pas encore : interruptions INT1 et INT2 (fréquences, alarmes).
namespace ravenemu::nds {

struct DateTime {
    unsigned year{2000};  // 2000 à 2099
    unsigned month{1};
    unsigned day{1};
    unsigned hour{};
    unsigned minute{};
    unsigned second{};
};

class RealTimeClock {
public:
    void set_date_time(const DateTime& date_time, Cycles now) noexcept;
    [[nodiscard]] DateTime date_time(Cycles now) const noexcept;
    [[nodiscard]] unsigned weekday(Cycles now) const noexcept;  // 0 = lundi

    // Registre 0x04000138 (16 bits).
    [[nodiscard]] std::uint32_t read() const noexcept;
    void write(std::uint32_t value, std::uint32_t mask, Cycles now) noexcept;

private:
    [[nodiscard]] std::int64_t seconds(Cycles now) const noexcept;
    void on_rising(bool data, Cycles now) noexcept;
    void on_falling() noexcept;
    void command_received(std::uint8_t command, Cycles now) noexcept;
    void parameter_received(std::uint8_t value, Cycles now) noexcept;
    void apply_write(Cycles now) noexcept;
    void reset(Cycles now) noexcept;
    [[nodiscard]] unsigned parameter_length() const noexcept;
    [[nodiscard]] std::uint8_t encode_hour(unsigned hour) const noexcept;
    [[nodiscard]] unsigned decode_hour(std::uint8_t value) const noexcept;

    std::uint32_t lines_{};  // registre tel que l'ARM7 l'a écrit
    bool output_{};          // bit sorti par la puce
    // Commande en cours.
    bool has_command_{};
    bool reading_{};
    unsigned register_{};
    std::uint8_t shift_{};
    unsigned bit_{};
    std::array<std::uint8_t, 7> buffer_{};
    unsigned index_{};
    // Registres.
    std::uint8_t status1_{0x02};
    std::uint8_t status2_{};
    std::uint8_t adjust_{};
    std::uint8_t free_{};
    std::array<std::uint8_t, 3> alarm1_{};
    std::array<std::uint8_t, 3> alarm2_{};
    // Heure : secondes depuis le 1er janvier 2000 à `base_cycles_`.
    std::int64_t base_seconds_{};
    Cycles base_cycles_{};
    unsigned weekday_offset_{};
};

} // namespace ravenemu::nds
