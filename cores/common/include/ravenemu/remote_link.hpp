#pragma once

#include <ravenemu/link_endpoint.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <variant>

namespace ravenemu {

/**
 * Câble link vers une console d'un autre appareil. L'hôte transporte les
 * messages d'un RemoteLink à l'autre, dans l'ordre et sans perte (une
 * connexion TCP, par exemple), et branche ou débranche le câble.
 *
 * La console qui cadence un transfert envoie sa donnée puis attend, en temps
 * réel, la réponse de l'autre : son temps émulé ne bouge pas, le jeu ne voit
 * aucun retard. L'autre répond quand elle lit le message, comme une console
 * qui attend l'horloge d'un partenaire, mais jamais plus tôt, sur sa propre
 * horloge, que l'écart entre deux transferts chez celle qui cadence : son jeu
 * a toujours le temps de préparer la donnée suivante. Les deux consoles
 * appliquent la même décision : un échange ne peut pas donner deux résultats.
 *
 * receive() et set_connected() s'appellent de n'importe quel fil ; le reste,
 * du fil d'émulation. L'envoi de l'hôte peut être appelé des deux.
 */
class RemoteLink {
public:
    // 2 : le transfert porte l'écart depuis le précédent.
    static constexpr std::uint8_t protocol_version = 2;
    // Plus long message : un transfert.
    static constexpr std::size_t max_message_size = 18;

    enum class Family : std::uint8_t { game_boy = 1, game_boy_advance = 2 };
    enum class Mode : std::uint8_t { game_boy = 0, normal8 = 1, normal32 = 2, multiplayer = 3, other = 4 };
    enum class State { unplugged, waiting, plugged, incompatible };

    /**
     * Une console cadence un transfert : sa donnée, sa durée en cycles (GBA),
     * et les cycles écoulés depuis le départ de son transfert précédent
     * (0 pour le premier d'un branchement).
     */
    struct Transfer {
        std::uint32_t id{};
        Mode mode{};
        std::uint32_t data{};
        std::uint32_t duration{};
        std::uint32_t interval{};
    };
    /** Ce que l'autre console a envoyé pendant le transfert `id`. */
    struct Reply {
        std::uint32_t id{};
        std::uint32_t data{};
    };
    /** Lignes d'état d'une GBA : son mode, et sa ligne SO tirée basse. */
    struct Status {
        Mode mode{};
        bool so_low{};
    };
    using Message = std::variant<Transfer, Reply, Status>;
    using Send = std::function<void(std::span<const std::uint8_t>)>;

    RemoteLink(Family family, Send send);
    RemoteLink(const RemoteLink&) = delete;
    RemoteLink& operator=(const RemoteLink&) = delete;

    /** Un message du partenaire ; faux s'il est mal formé (il est alors ignoré). */
    [[nodiscard]] bool receive(std::span<const std::uint8_t> message);
    /**
     * Branche ou débranche. Brancher annonce cette console au partenaire ; le
     * câble sert une fois l'annonce de l'autre reçue. Débrancher termine une
     * attente sur-le-champ : le transfert finit comme sans câble.
     */
    void set_connected(bool connected);
    [[nodiscard]] State state() const;

    /** Branché, et le partenaire est une console compatible. */
    [[nodiscard]] bool plugged() const noexcept { return plugged_.load(std::memory_order_acquire); }
    /** Des messages attendent : test sans verrou, pour le fil d'émulation. */
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }
    /** Numéro du branchement en cours : il change à chaque branchement. */
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    /** Message suivant, sans attendre. */
    [[nodiscard]] std::optional<Message> take();
    /**
     * Message suivant, en attendant. Rien si le câble est débranché ou
     * rebranché depuis `generation` : une réponse attendue n'y viendra plus.
     */
    [[nodiscard]] std::optional<Message> wait(std::uint32_t generation);

    void send(const Transfer& transfer) const;
    void send(const Reply& reply) const;
    void send(const Status& status) const;

private:
    void send_bytes(std::span<const std::uint8_t> bytes) const;
    void accept_hello(bool compatible);  // sous le verrou

    const Family family_;
    const Send send_;
    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<Message> queue_;
    bool connected_{};
    bool partner_ok_{};
    bool incompatible_{};
    std::optional<bool> early_hello_;  // annonce reçue avant le branchement d'ici
    std::atomic<bool> plugged_{};
    std::atomic<bool> pending_{};
    std::atomic<std::uint32_t> generation_{};
};

/**
 * Port Game Boy relié par un câble distant : un octet par transfert. La
 * console qui cadence envoie son octet au premier bit et attend la réponse ;
 * l'autre la cadence d'un coup, bit par bit, quand elle lit le message, mais
 * pas avant que son horloge ait avancé, depuis l'octet précédent, de l'écart
 * laissé par celle qui cadence : son jeu a eu le temps de préparer le suivant.
 */
class RemoteLinkEndpoint final : public LinkEndpoint {
public:
    explicit RemoteLinkEndpoint(RemoteLink& link) noexcept : link_(link) {}

    [[nodiscard]] bool attach(LinkPort& port) noexcept override {
        if (port_ != nullptr && port_ != &port) return false;
        if (port_ != &port) forget_timing();  // une autre machine : une autre horloge
        port_ = &port;
        return true;
    }
    void detach(LinkPort& port) noexcept override {
        if (port_ != &port) return;
        port_ = nullptr;
        forget_timing();
    }
    [[nodiscard]] bool exchange_bit(LinkPort& source, bool outgoing_high) noexcept override;
    void poll() noexcept override;
    /** Un octet du partenaire attend que cette console atteigne son cycle. */
    [[nodiscard]] bool deferred() const noexcept { return deferred_.has_value(); }

    // Au plus une demi-image d'attente, quel que soit l'écart annoncé.
    static constexpr std::uint64_t max_interval = 35'112;

private:
    std::uint8_t transfer(LinkPort& source, std::uint8_t outgoing) noexcept;
    void serve(const RemoteLink::Transfer& transfer) noexcept;
    void clock_in(const RemoteLink::Transfer& transfer) noexcept;
    void follow_generation() noexcept;
    void forget_timing() noexcept {
        deferred_.reset();
        last_start_.reset();
        last_serve_.reset();
    }

    RemoteLink& link_;
    LinkPort* port_{};
    std::uint32_t last_id_{};
    std::uint8_t reply_{0xff};
    std::uint32_t generation_{};
    std::optional<RemoteLink::Transfer> deferred_;  // lu trop tôt : servi à `due_`
    std::uint64_t due_{};
    std::optional<std::uint64_t> last_start_;  // départ du dernier octet cadencé ici
    std::optional<std::uint64_t> last_serve_;  // dernier octet du partenaire servi
};

} // namespace ravenemu
