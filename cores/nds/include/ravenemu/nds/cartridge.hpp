#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ravenemu::nds {

/** Descripteur d'un exécutable dans l'en-tête NDS (GBATEK, DS Cartridge Header). */
struct Executable {
    std::uint32_t rom_offset{};
    std::uint32_t entry_address{};
    std::uint32_t load_address{};
    std::uint32_t size{};
};

struct CartridgeHeader {
    std::string title;
    std::string game_code;
    Executable arm9;
    Executable arm7;
};

/**
 * Image NDS propriétaire de ses octets, indépendante du buffer de l'appelant.
 * Validation structurelle seulement : ni authentification, ni déchiffrement
 * KEY1 de la zone sécurisée. DSi et images tronquées sont refusées.
 */
class Cartridge {
public:
    explicit Cartridge(std::span<const std::uint8_t> rom);
    [[nodiscard]] const CartridgeHeader& header() const noexcept { return header_; }
    [[nodiscard]] std::span<const std::uint8_t> rom() const noexcept { return rom_ ? std::span<const std::uint8_t>{*rom_} : std::span<const std::uint8_t>{}; }
    [[nodiscard]] std::span<const std::uint8_t> arm9_image() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> arm7_image() const noexcept;

private:
    CartridgeHeader header_;
    std::shared_ptr<const std::vector<std::uint8_t>> rom_; // média immuable partagé, état du bus indépendant
};

} // namespace ravenemu::nds
