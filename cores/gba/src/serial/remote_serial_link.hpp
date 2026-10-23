#pragma once
#include "serial/serial.hpp"

#include <ravenemu/remote_link.hpp>

#include <optional>

namespace ravenemu::gba {

/**
 * Port série GBA relié à une console d'un autre appareil par un RemoteLink :
 * deux joueurs, `player` 0 étant le parent des jeux multijoueurs.
 *
 * La console qui cadence un transfert envoie sa donnée au départ et attend la
 * réponse à la fin, à son propre cycle : son temps émulé ne bouge pas pendant
 * l'attente. L'autre console entre dans le transfert quand elle lit le
 * message, pour sa durée sur sa propre horloge, comme un enfant que le parent
 * vient de cadencer ; mais pas avant que son horloge ait avancé, depuis le
 * transfert précédent, de l'écart que le parent a laissé entre les deux. Sans
 * cela, deux messages lus d'affilée (entre deux images) feraient répondre au
 * second la donnée du premier, avant que le jeu ait traité son interruption.
 * Les lignes d'état (mode, SO) voyagent à chaque changement.
 */
class RemoteSerialLink final : public SerialLink {
public:
    RemoteSerialLink(RemoteLink& link, int player) noexcept : link_(link), player_(player) {}
    RemoteSerialLink(const RemoteSerialLink&) = delete;
    RemoteSerialLink& operator=(const RemoteSerialLink&) = delete;
    ~RemoteSerialLink() override { detach(); }

    // Le port de la machine courante : une machine remplacée est reprise ici.
    void attach(Serial& serial) noexcept;
    void detach() noexcept;
    // La machine du port a disparu : l'oublier sans y toucher.
    void forget() noexcept {
        serial_ = nullptr;
        pending_.reset();
        forget_timing();
    }
    // Fil d'émulation, entre deux tranches : transferts et lignes d'état du partenaire.
    void service();
    // Un transfert du partenaire attend que cette console atteigne son cycle.
    [[nodiscard]] bool deferred() const noexcept { return deferred_.has_value(); }

    [[nodiscard]] int players() const noexcept override { return link_.plugged() ? 2 : 1; }
    [[nodiscard]] bool all_multiplayer() const noexcept override;
    [[nodiscard]] bool partner_so_low(const Serial&) const noexcept override { return partner_so_low_; }
    [[nodiscard]] std::uint32_t start(Serial& master, Serial::Mode mode, int duration) override;
    void capture(Serial& participant) override;
    void finish(const Serial& caller) override;
    void refresh() noexcept override;

private:
    struct Pending {
        std::uint32_t id{};
        std::uint32_t generation{};
        Serial::Mode mode{};
        std::uint32_t data{};
        // Réponse arrivée avant la fin du transfert, lue entre deux tranches.
        std::optional<std::uint32_t> answer;
    };
    // Un transfert du partenaire lu trop tôt : cette console y entre à son
    // cycle, et répond à ce moment-là.
    struct Deferred {
        std::uint32_t id{};
        Serial::Mode mode{};
        std::uint32_t data{};
    };
    void follow_plug();
    void handle(const RemoteLink::Message& message);
    void serve(const RemoteLink::Transfer& transfer);
    void reply(std::uint32_t id, std::uint32_t data);
    void forget_timing() noexcept {
        deferred_.reset();
        last_start_.reset();
        last_join_.reset();
    }
    void publish_status();

    RemoteLink& link_;
    const int player_;
    Serial* serial_{};
    bool plugged_{};
    std::uint32_t generation_{};
    RemoteLink::Mode partner_mode_{RemoteLink::Mode::other};
    bool partner_so_low_{};
    std::optional<RemoteLink::Status> published_;
    std::optional<Pending> pending_;
    std::optional<Deferred> deferred_;
    std::optional<std::int64_t> last_start_;  // départ du dernier transfert cadencé ici
    std::optional<std::int64_t> last_join_;   // entrée dans le dernier transfert du partenaire
    std::uint32_t counter_{};
};

} // namespace ravenemu::gba
