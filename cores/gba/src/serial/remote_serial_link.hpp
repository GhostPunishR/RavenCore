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
 * vient de cadencer. Les lignes d'état (mode, SO) voyagent à chaque changement.
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
    }
    // Fil d'émulation, entre deux tranches : transferts et lignes d'état du partenaire.
    void service();

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
    void follow_plug();
    void handle(const RemoteLink::Message& message);
    void serve(const RemoteLink::Transfer& transfer);
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
    std::uint32_t counter_{};
};

} // namespace ravenemu::gba
