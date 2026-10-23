#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ravenemu {

enum class Console : std::uint8_t;

/** Ce qu'une opération de code de triche fait à chaque image. */
enum class CheatAction : std::uint8_t {
    write,          ///< `count` écritures : adresse += address_step, valeur += value_step après chacune
    add,            ///< [adresse] += valeur
    bit_or,         ///< [adresse] |= valeur
    bit_and,        ///< [adresse] &= valeur
    write_pointer,  ///< [[adresse] + offset] = valeur, le pointeur lu sur 32 bits
    test,           ///< si le test échoue, les `skip` opérations suivantes sont sautées
    jump,           ///< les `skip` opérations suivantes sont sautées
    rom_patch,      ///< Game Boy : octet de ROM remplacé à chaque lecture (Game Genie)
};

/** Comparaison d'une opération `test` entre la mémoire et `value`. */
enum class CheatTest : std::uint8_t {
    equal,
    not_equal,
    less_signed,
    greater_signed,
    less,
    greater,
    any_set,   ///< (mémoire & valeur) != 0
    none_set,  ///< (mémoire & valeur) == 0
    never,
};

struct CheatOperation {
    CheatAction action{};
    CheatTest test{};
    std::uint8_t size{1};  ///< octets lus ou écrits : 1, 2 ou 4
    /**
     * Game Boy, code GameShark : banque de RAM de cartouche (A000-BFFF), ou
     * banque de WRAM de la GBC quand elle vaut 0x91 à 0x97 (D000-DFFF).
     */
    std::uint8_t bank{};
    std::uint32_t address{};
    std::uint32_t value{};
    std::uint32_t count{1};
    std::uint32_t address_step{};
    std::uint32_t value_step{};
    std::uint32_t offset{};
    std::uint32_t skip{};
    std::optional<std::uint8_t> original;  ///< Game Genie : octet attendu en ROM
};

/**
 * Un code de triche décodé par parse_cheat : une ou plusieurs lignes, comme
 * une entrée d'un menu de triche, traduites en opérations. Les codes d'activation
 * (identifiant du jeu, adresse d'accroche) n'en produisent aucune.
 */
struct Cheat {
    Console console{};
    std::vector<CheatOperation> operations;
};

/** Mémoire d'une console vue par run_cheat. */
class CheatMemory {
public:
    virtual ~CheatMemory() = default;
    [[nodiscard]] virtual std::uint32_t read(std::uint32_t address, int size) = 0;
    virtual void write(std::uint32_t address, int size, std::uint32_t value) = 0;
};

/** Exécute une fois les opérations GBA d'un code, comme le fait l'appareil à chaque image. */
void run_cheat(const Cheat& cheat, CheatMemory& memory);

namespace detail {

/** Graines de chiffrement des codes GameShark v1/v2 et Action Replay v3 (GBATEK). */
using CheatSeeds = std::array<std::uint32_t, 4>;
inline constexpr CheatSeeds gameshark_seeds{0x09f4fbbdU, 0x9681884aU, 0x352027e9U, 0xf3dee5a7U};
inline constexpr CheatSeeds action_replay_seeds{0x7aa9648fU, 0x7fae6994U, 0xc0efaad5U, 0x42712c57U};

/** Chiffrement de ces appareils (32 tours, GBATEK « Gameshark Encryption »), et son inverse. */
[[nodiscard]] std::array<std::uint32_t, 2> encrypt_cheat_line(std::array<std::uint32_t, 2> line, const CheatSeeds& seeds) noexcept;
[[nodiscard]] std::array<std::uint32_t, 2> decrypt_cheat_line(std::array<std::uint32_t, 2> line, const CheatSeeds& seeds) noexcept;

} // namespace detail

} // namespace ravenemu
