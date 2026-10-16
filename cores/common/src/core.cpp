#include "ravenemu/core.hpp"

#include <array>

// The public factory functions live beside their concrete implementations.
// This translation unit keeps what every console shares: the link cable.

namespace ravenemu {

namespace {

/**
 * Câble Game Boy : les deux ports série s'échangent leurs bits par un
 * LocalLinkEndpoint, au rythme de la console qui fournit l'horloge. Les
 * cœurs avancent l'un après l'autre, une image chacun.
 */
class EndpointLinkCable final : public LinkCable {
public:
    EndpointLinkCable(Core& first, Core& second) : players_{&first, &second} {
        if (!first.connect_link_endpoint(&endpoint_) || !second.connect_link_endpoint(&endpoint_)) {
            disconnect();
            throw std::invalid_argument("Ces consoles ne se relient pas par un câble link Game Boy");
        }
    }
    EndpointLinkCable(const EndpointLinkCable&) = delete;
    EndpointLinkCable& operator=(const EndpointLinkCable&) = delete;
    ~EndpointLinkCable() override { disconnect(); }

    [[nodiscard]] std::size_t players() const noexcept override { return players_.size(); }
    void run_frame(std::span<const std::span<std::int32_t>> framebuffers, bool render_video) override {
        if (framebuffers.size() != players_.size()) throw std::invalid_argument("Une image par console reliée");
        for (std::size_t index = 0; index < players_.size(); ++index)
            players_[index]->run_frame(framebuffers[index], render_video);
    }

private:
    void disconnect() noexcept {
        for (auto* player : players_) static_cast<void>(player->connect_link_endpoint(nullptr));
    }
    LocalLinkEndpoint endpoint_;
    std::array<Core*, 2> players_;
};

} // namespace

std::unique_ptr<LinkCable> Core::create_link_cable(std::span<Core* const> players) {
    if (players.size() != 2) throw std::invalid_argument("Le câble link Game Boy relie deux consoles");
    return std::make_unique<EndpointLinkCable>(*players[0], *players[1]);
}

std::unique_ptr<LinkCable> connect_link_cable(std::span<Core* const> players) {
    if (players.size() < 2) throw std::invalid_argument("Un câble link relie au moins deux consoles");
    for (std::size_t index = 0; index < players.size(); ++index) {
        if (players[index] == nullptr) throw std::invalid_argument("Console absente du câble link");
        for (std::size_t other = 0; other < index; ++other)
            if (players[other] == players[index]) throw std::invalid_argument("Une console ne se relie pas à elle-même");
    }
    return players[0]->create_link_cable(players);
}

} // namespace ravenemu
