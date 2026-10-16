#pragma once

#include "memory/bus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace ravenemu::gba {

class LinkHub;

/**
 * Port série (GBATEK, « GBA Communication Ports ») : modes normal (8 ou
 * 32 bits) et multijoueur, seul ou relié à d'autres consoles par un LinkHub.
 *
 * Sans câble, le matériel termine pourtant seul les transferts qu'il cadence :
 * - mode normal, horloge interne : 8 ou 32 bits à 256 kHz ou 2 MHz ; la ligne
 *   SI reste haute, si bien que les données reçues valent toutes 1 ;
 * - mode multijoueur : la console reçoit 0xFFFF de chaque enfant absent et
 *   garde sa propre donnée dans SIOMULTI0.
 * Avec une horloge externe (mode normal esclave), le transfert attend qu'un
 * partenaire le cadence, comme sur la console. Le mode UART n'est pas émulé.
 *
 * Relié, le parent (joueur 0) lance les transferts multijoueurs ; en mode
 * normal, la console à horloge interne cadence celle qui attend avec une
 * horloge externe. Chaque console lit les données des autres à l'instant du
 * départ et reçoit le résultat à l'instant de la fin, sur sa propre horloge.
 */
class Serial {
public:
    static constexpr int idle = std::numeric_limits<int>::max();
    enum class Mode { normal8, normal32, multiplayer, other };
    // Durée d'un transfert multijoueur en cycles, selon le débit (9600, 38400,
    // 57600 ou 115200 bauds) et le nombre de consoles reliées (1 à 4) :
    // mesures sur console publiées par mGBA.
    static constexpr std::array<std::array<int, 4>, 4> multiplayer_cycles{{
        {31976, 63427, 94884, 125829},
        {8378, 16241, 24104, 31457},
        {5750, 10998, 16241, 20972},
        {3140, 5755, 8376, 10486},
    }};
    static constexpr int longest_transfer = 125829;
    // Données reçues à la fin d'un transfert relié : SIOMULTI0..3 en mode
    // multijoueur ; en mode normal, le mot reçu (demi-mot bas puis haut).
    using Received = std::array<std::uint16_t, 4>;

    Serial(Bus& bus, InterruptController& interrupts) : bus_(bus), interrupts_(interrupts) {}
    Serial(const Serial&) = delete;
    Serial& operator=(const Serial&) = delete;

    // Écriture de SIOCNT, déjà rangée dans les registres d'E/S.
    void control_write(int value);
    // Écriture de RCNT : le mode du port peut changer.
    void rcnt_write();
    [[nodiscard]] int cycles_until_completion() const noexcept { return std::min(remaining_, join_in_); }
    void tick(int cycles) {
        clock_ += cycles;
        // Une fin précède toujours le départ suivant sur la même horloge.
        if (remaining_ != idle && (remaining_ -= cycles) <= 0) complete();
        if (join_in_ != idle && (join_in_ -= cycles) <= 0) join_now(-join_in_);
    }

    // Cycles écoulés depuis la création : l'horloge du câble en découle.
    [[nodiscard]] std::int64_t clock() const noexcept { return clock_; }
    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] int player() const noexcept { return player_; }
    [[nodiscard]] bool linked() const noexcept;
    void attach(LinkHub* hub, int player) noexcept {
        hub_ = hub;
        player_ = hub ? player : 0;
    }
    // Recalcule les bits d'état de SIOCNT (SI, SD, numéro de joueur).
    void refresh_status() noexcept;

    // Côté câble : ce que la console envoie, son niveau au repos, et son
    // entrée dans un transfert lancé par une autre.
    [[nodiscard]] std::uint32_t outgoing(Mode mode) const noexcept;
    [[nodiscard]] std::uint32_t idle_level(Mode mode) const noexcept;
    [[nodiscard]] bool ready_for(Mode mode) const noexcept;
    [[nodiscard]] bool so_low() const noexcept;
    [[nodiscard]] bool transferring() const noexcept { return remaining_ != idle; }
    void join(std::int64_t start_in, int duration);
    void leave() noexcept;
    void mark_busy() noexcept;
    void receive(const Received& value) noexcept { received_ = value; }

    // État instantané du câble. Ordre : départ attendu, sa durée, transfert
    // relié, résultat présent, puis les quatre demi-mots reçus.
    static constexpr std::size_t link_state_words = 8;
    [[nodiscard]] std::array<std::int32_t, link_state_words> export_link_state() const noexcept;
    void import_link_state(std::span<const std::int32_t> state);
    [[nodiscard]] int export_remaining() const noexcept { return remaining_; }
    void import_remaining(int remaining) {
        if (remaining != idle && (remaining <= 0 || remaining > longest_transfer))
            throw SaveStateError("État série GBA invalide");
        remaining_ = remaining;
    }

private:
    void start_multiplayer();
    void start_normal();
    void join_now(int late);
    void begin_joined(std::int64_t remaining);
    void complete();
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
    LinkHub* hub_{};
    int player_{};
    std::int64_t clock_{};
    int remaining_{idle};             // cycles avant la fin du transfert en cours
    int join_in_{idle};               // cycles avant le départ d'un transfert lancé ailleurs
    int join_duration_{};             // durée de ce transfert
    bool busy_{};                     // bit Start/Busy de SIOCNT
    bool linked_transfer_{};          // le transfert en cours passe par le câble
    std::optional<Received> received_;
};

inline void serial_control_write(Serial* serial, int value) {
    if (serial) serial->control_write(value);
}
inline void serial_rcnt_write(Serial* serial) {
    if (serial) serial->rcnt_write();
}

/**
 * Câble link entre 2 à 4 consoles d'un même processus : les machines
 * avancent par tranches, à tour de rôle, et le câble traduit l'instant d'un
 * transfert d'une horloge à l'autre. Le joueur 0 est le parent.
 */
class LinkHub {
public:
    static constexpr int max_players = 4;
    // `origin` : valeur de l'horloge série du joueur à l'instant 0 du câble.
    void attach(int player, Serial& serial, std::int64_t origin) noexcept;
    void detach_all() noexcept;
    [[nodiscard]] int players() const noexcept;
    [[nodiscard]] Serial* serial(int player) const noexcept {
        return player >= 0 && player < max_players ? slots_[static_cast<std::size_t>(player)].serial : nullptr;
    }
    // Joueur qui a lancé le dernier transfert : il avance en premier, si
    // bien que les autres apprennent le départ avant de l'atteindre.
    [[nodiscard]] int first() const noexcept { return first_; }
    void refresh() noexcept;

    // Rend false si le câble est déjà pris : la console transfère alors seule.
    [[nodiscard]] bool start(Serial& master, Serial::Mode mode, int duration);
    void capture(Serial& participant);
    // Fin du transfert, à l'instant où `caller` l'atteint le premier.
    void finish(const Serial& caller);

private:
    [[nodiscard]] std::int64_t now(const Serial& serial) const noexcept;
    struct Slot {
        Serial* serial{};
        std::int64_t origin{};
    };
    struct Transfer {
        bool active{};
        Serial::Mode mode{};
        int master{};
        std::array<std::uint32_t, max_players> sent{};
        std::array<bool, max_players> participates{};
        std::array<bool, max_players> captured{};
    };
    std::array<Slot, max_players> slots_{};
    Transfer transfer_{};
    int first_{};
};

} // namespace ravenemu::gba
