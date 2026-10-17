#include "ravenemu/core.hpp"

// The public factory functions live beside their concrete implementations.
// This translation unit keeps what every console shares: the link cable entry.

namespace ravenemu {

// Chaque console fournit son câble (Game Boy, GBA) ; les autres ne se relient pas.
std::unique_ptr<LinkCable> Core::create_link_cable(std::span<Core* const>) {
    throw std::invalid_argument("Cette console ne se relie pas par un câble link");
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
