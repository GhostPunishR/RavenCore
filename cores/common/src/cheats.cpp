#include "ravenemu/cheats.hpp"
#include "ravenemu/core.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace ravenemu {
namespace {

// Une ligne qu'un format reconnaît sans savoir l'appliquer, ou qu'il ne
// reconnaît pas : la détection du format GBA essaie les deux chiffrements.
class Unsupported final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class Invalid final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition) {
    if (!condition) throw Invalid("ligne invalide");
}

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Au plus huit chiffres déjà vérifiés.
[[nodiscard]] std::uint32_t hex_value(std::string_view digits) noexcept {
    std::uint32_t value = 0;
    for (const char c : digits) value = value << 4U | static_cast<std::uint32_t>(hex_digit(c));
    return value;
}

[[nodiscard]] std::string quoted(std::string_view text) { return "« " + std::string(text) + " »"; }

// ------------------------------------------------------------------- Game Boy

[[nodiscard]] bool separator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '+' || c == ',' || c == ';';
}

/**
 * Game Genie `ABC-DEF-GHI` (Pan Docs) : AB est la nouvelle donnée, FCDE
 * l'adresse XOR F000, GI l'ancienne donnée XOR BA tournée de deux bits à
 * gauche ; H n'est pas utilisé. Sans GHI, l'octet est remplacé sans comparaison.
 */
[[nodiscard]] CheatOperation game_genie(std::string_view token, std::string_view digits) {
    const auto digit = [&](std::size_t index) { return static_cast<std::uint32_t>(hex_digit(digits[index])); };
    CheatOperation operation;
    operation.action = CheatAction::rom_patch;
    operation.value = digit(0) << 4U | digit(1);
    operation.address = (digit(5) << 12U | digit(2) << 8U | digit(3) << 4U | digit(4)) ^ 0xf000U;
    if (operation.address > 0x7fffU)
        throw CheatError("le code Game Genie " + quoted(token) + " ne vise pas la ROM (0000-7FFF)");
    if (digits.size() == 9) {
        const auto stored = digit(6) << 4U | digit(8);
        const auto rotated = (stored >> 2U | stored << 6U) & 0xffU;
        operation.original = static_cast<std::uint8_t>(rotated ^ 0xbaU);
    }
    return operation;
}

/**
 * GameShark `BBVVLLHH` (Pan Docs) : banque, valeur, adresse HHLL. La banque
 * choisit la RAM de cartouche ; 91 à 97 choisissent aussi une banque de WRAM
 * de la GBC. L'appareil récrit ces octets à chaque image.
 */
[[nodiscard]] CheatOperation game_shark(std::string_view token, std::string_view digits) {
    CheatOperation operation;
    operation.action = CheatAction::write;
    operation.bank = static_cast<std::uint8_t>(hex_value(digits.substr(0, 2)));
    operation.value = hex_value(digits.substr(2, 2));
    operation.address = hex_value(digits.substr(6, 2)) << 8U | hex_value(digits.substr(4, 2));
    if (operation.address < 0x8000U)
        throw CheatError("le code GameShark " + quoted(token) + " vise la ROM : c'est le rôle d'un code Game Genie");
    return operation;
}

[[nodiscard]] std::vector<CheatOperation> parse_game_boy(std::string_view text) {
    std::vector<CheatOperation> operations;
    std::size_t position = 0;
    while (position < text.size()) {
        if (separator(text[position])) { ++position; continue; }
        const auto end = std::find_if(text.begin() + static_cast<std::ptrdiff_t>(position), text.end(), separator);
        const auto token = text.substr(position, static_cast<std::size_t>(end - text.begin()) - position);
        position += token.size();
        std::string digits;
        for (const char c : token) if (c != '-') digits += c;
        const bool hex = std::all_of(digits.begin(), digits.end(), [](char c) { return hex_digit(c) >= 0; });
        if (hex && (digits.size() == 6 || digits.size() == 9)) operations.push_back(game_genie(token, digits));
        else if (hex && digits.size() == 8) operations.push_back(game_shark(token, digits));
        else throw CheatError(quoted(token) + " n'est ni un code Game Genie (ABC-DEF-GHI) ni un code GameShark (8 chiffres)");
    }
    return operations;
}

// ------------------------------------------------------------------------ GBA

using Line = std::array<std::uint32_t, 2>;

// Mémoire qu'un code GBA peut lire ou modifier, aux adresses canoniques :
// EWRAM, IWRAM, E/S, palette, VRAM, OAM.
[[nodiscard]] bool in_memory(std::uint32_t first, std::uint32_t last) noexcept {
    constexpr std::array<std::array<std::uint32_t, 2>, 6> regions{{
        {0x0200'0000U, 0x0203'ffffU}, {0x0300'0000U, 0x0300'7fffU}, {0x0400'0000U, 0x0400'03ffU},
        {0x0500'0000U, 0x0500'03ffU}, {0x0600'0000U, 0x0601'7fffU}, {0x0700'0000U, 0x0700'03ffU},
    }};
    return first <= last && std::any_of(regions.begin(), regions.end(), [&](const auto& region) {
        return first >= region[0] && last <= region[1];
    });
}
void require_access(std::uint32_t address, int size) {
    const auto bytes = static_cast<std::uint32_t>(size);
    require(address % bytes == 0 && in_memory(address, address + bytes - 1));
}

[[nodiscard]] constexpr std::uint32_t value_mask(int size) noexcept {
    return size >= 4 ? 0xffff'ffffU : (1U << (8U * static_cast<unsigned>(size))) - 1U;
}

[[nodiscard]] CheatOperation memory_operation(CheatAction action, std::uint32_t address, int size, std::uint32_t value) {
    require_access(address, size);
    require((value & ~value_mask(size)) == 0);
    CheatOperation operation;
    operation.action = action;
    operation.address = address;
    operation.size = static_cast<std::uint8_t>(size);
    operation.value = value;
    return operation;
}

// `count` écritures à partir de `address`, toutes dans la même mémoire.
[[nodiscard]] CheatOperation slide(std::uint32_t address, int size, std::uint32_t value, std::uint32_t count,
                                   std::uint32_t address_step, std::uint32_t value_step) {
    auto operation = memory_operation(CheatAction::write, address, size, value);
    const auto span = static_cast<std::uint64_t>(count == 0 ? 0 : count - 1) * address_step;
    require(span <= 0x0fff'ffffU && address_step % static_cast<std::uint32_t>(size) == 0);
    require_access(address + static_cast<std::uint32_t>(span), size);
    operation.count = count;
    operation.address_step = address_step;
    operation.value_step = value_step;
    return operation;
}

[[nodiscard]] CheatOperation test(CheatTest kind, std::uint32_t address, int size, std::uint32_t value) {
    auto operation = memory_operation(CheatAction::test, address, size, value);
    operation.test = kind;
    return operation;
}

/**
 * Une ligne (ou un groupe de lignes) de l'appareil. Un test peut couvrir les
 * codes qui le suivent, un bloc jusqu'au ELSE ou au ENDIF, ou tout le reste :
 * les sauts se comptent en codes, puis en opérations une fois la liste à plat.
 */
enum class Flow : std::uint8_t { none, next_codes, block, rest, else_branch, end_if };
struct Code {
    std::vector<CheatOperation> operations;
    Flow flow{Flow::none};
    std::uint32_t codes{};
};

[[nodiscard]] Code plain(CheatOperation operation) { return Code{{operation}, Flow::none, 0}; }
[[nodiscard]] Code guarded(CheatOperation operation, Flow flow, std::uint32_t codes = 0) {
    return Code{{operation}, flow, codes};
}
[[nodiscard]] CheatOperation jump() {
    CheatOperation operation;
    operation.action = CheatAction::jump;
    return operation;
}

// Premier code après la fin du bloc qui commence après `from` : son ELSE si
// `at_else`, sinon son ENDIF ; la fin de la liste s'il n'en a pas.
[[nodiscard]] std::size_t block_end(const std::vector<Code>& codes, std::size_t from, bool at_else) {
    int depth = 0;
    for (auto index = from + 1; index < codes.size(); ++index) {
        switch (codes[index].flow) {
        case Flow::block: ++depth; break;
        case Flow::else_branch: if (depth == 0 && at_else) return index + 1; break;
        case Flow::end_if: if (depth-- == 0) return index + 1; break;
        default: break;
        }
    }
    return codes.size();
}

[[nodiscard]] std::vector<CheatOperation> flatten(std::vector<Code> codes) {
    std::vector<std::size_t> start(codes.size() + 1);
    for (std::size_t index = 0; index < codes.size(); ++index)
        start[index + 1] = start[index] + codes[index].operations.size();
    std::vector<CheatOperation> operations;
    operations.reserve(start.back());
    for (std::size_t index = 0; index < codes.size(); ++index) {
        auto& code = codes[index];
        std::size_t target = index + 1;
        switch (code.flow) {
        case Flow::none: case Flow::end_if: break;
        case Flow::next_codes: target = std::min(codes.size(), index + 1 + code.codes); break;
        case Flow::rest: target = codes.size(); break;
        case Flow::block: target = block_end(codes, index, true); break;
        case Flow::else_branch: target = block_end(codes, index, false); break;
        }
        if (!code.operations.empty())
            code.operations.front().skip = static_cast<std::uint32_t>(start[target] - start[index + 1]);
        operations.insert(operations.end(), code.operations.begin(), code.operations.end());
    }
    return operations;
}

constexpr std::uint32_t game_id_marker = 0x001d'c0deU;
constexpr std::uint32_t seed_change = 0xdead'faceU;

void reject_seed_change(const Line& line) {
    if (line[0] == seed_change)
        throw Unsupported("le changement de chiffrement DEADFACE n'est pas pris en charge");
}

/** GameShark v1/v2 déchiffré (GBATEK, « Gameshark RAW Codes »). */
[[nodiscard]] std::vector<Code> gameshark_codes(const std::vector<Line>& lines) {
    std::vector<Code> codes;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto [left, right] = lines[index];
        reject_seed_change(lines[index]);
        if (right == game_id_marker) continue;  // identifiant du jeu : rien à exécuter
        const auto address = left & 0x0fff'ffffU;
        switch (left >> 28U) {
        case 0x0: codes.push_back(plain(memory_operation(CheatAction::write, address, 1, right))); break;
        case 0x1: codes.push_back(plain(memory_operation(CheatAction::write, address, 2, right))); break;
        case 0x2: codes.push_back(plain(memory_operation(CheatAction::write, address, 4, right))); break;
        case 0x3: {
            // La même valeur à cccc - 1 adresses, deux par ligne suivante.
            require((left & 0x0fff'0000U) == 0 && (left & 0xffffU) != 0);
            const auto targets = (left & 0xffffU) - 1U;
            const std::size_t used = (targets + 1U) / 2U;
            require(index + used < lines.size());
            Code code;
            for (std::uint32_t target = 0; target < targets; ++target)
                code.operations.push_back(
                    memory_operation(CheatAction::write, lines[index + 1 + target / 2U][target % 2U], 4, right));
            if (targets % 2U == 1U) require(lines[index + used][1] == 0);
            index += used;
            codes.push_back(std::move(code));
            break;
        }
        case 0x6: throw Unsupported("les patchs de ROM GBA ne sont pas pris en charge");
        case 0x8: {
            // Écritures au bouton du GameShark, absent ici : codes sans effet.
            if (left == 0x80f0'0000U) {
                require((right & 0xffff'0000U) == 0);
            } else {
                const auto size = static_cast<int>((left >> 20U) & 0xfU);
                require(size == 1 || size == 2);
                static_cast<void>(memory_operation(CheatAction::write, (left & 0x0f00'0000U) | (left & 0x000f'ffffU),
                                                   size, right));
            }
            codes.emplace_back();
            break;
        }
        case 0xd: codes.push_back(guarded(test(CheatTest::equal, address, 2, right), Flow::next_codes, 1)); break;
        case 0xe:
            require((left & 0x0f00'0000U) == 0 && (right & 0xf000'0000U) == 0);
            codes.push_back(guarded(test(CheatTest::equal, right, 2, left & 0xffffU), Flow::next_codes,
                                    (left >> 16U) & 0xffU));
            break;
        case 0xf:
            // Adresse d'accroche du gestionnaire de codes : rien à exécuter ici.
            require(address >= 0x0800'0000U && address <= 0x09ff'ffffU && (right & 0xffff'f0f0U) == 0);
            break;
        default: require(false);
        }
    }
    return codes;
}

// Adresse a0aaaaa des codes Action Replay v3 : quatre bits de région, 20 de décalage.
[[nodiscard]] std::uint32_t replay_address(std::uint32_t field) {
    require((field & 0x00c0'0000U) == 0);
    return (field & 0x00f0'0000U) << 4U | (field & 0x000f'ffffU);
}

// Lignes spéciales `00000000 xxxxxxxx` ; rend faux à la fin de la liste.
bool replay_special(const std::vector<Line>& lines, std::size_t& index, std::vector<Code>& codes) {
    const auto right = lines[index][1];
    if (right == 0) return false;
    if (right == 0x6000'0000U) { codes.push_back(guarded(jump(), Flow::else_branch)); return true; }
    if (right == 0x4000'0000U) { codes.push_back(Code{{}, Flow::end_if, 0}); return true; }
    if ((right & 0xffff'00ffU) == 0x0800'0000U) { codes.emplace_back(); return true; }  // ralentissement
    const auto kind = right >> 24U;
    require(index + 1 < lines.size());
    const auto [first, second] = lines[++index];
    switch (kind) {
    case 0x80: case 0x82: case 0x84: {
        // Écritures en série : valeur += ss et adresse += ssss éléments après chacune.
        const int size = 1 << ((kind >> 1U) & 3U);
        codes.push_back(plain(slide(replay_address(right & 0x00ff'ffffU), size, first, (second >> 16U) & 0xffU,
                                    (second & 0xffffU) * static_cast<std::uint32_t>(size), second >> 24U)));
        return true;
    }
    case 0x10: case 0x12: case 0x14:
        // Écritures au bouton de l'Action Replay, absent ici : codes sans effet.
        require(second == 0);
        static_cast<void>(memory_operation(CheatAction::write, replay_address(right & 0x00ff'ffffU),
                                           1 << ((kind >> 1U) & 3U), first));
        codes.emplace_back();
        return true;
    case 0x18: case 0x1a: case 0x1c: case 0x1e: throw Unsupported("les patchs de ROM GBA ne sont pas pris en charge");
    default: require(false); return true;
    }
}

/** Pro Action Replay v3 déchiffré (GBATEK, « Pro Action Replay V3 - RAW Codes »). */
[[nodiscard]] std::vector<Code> action_replay_codes(const std::vector<Line>& lines) {
    std::vector<Code> codes;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto [left, right] = lines[index];
        reject_seed_change(lines[index]);
        if (right == game_id_marker) continue;
        if (left == 0) {
            if (!replay_special(lines, index, codes)) break;  // fin de la liste
            continue;
        }
        // ttaaaaaa : sub (2 bits), typ (3), siz (2), xtr (1), puis l'adresse.
        const auto type = left >> 24U;
        const auto sub = type >> 6U, typ = (type >> 3U) & 7U, siz = (type >> 1U) & 3U;
        const int size = 1 << siz;
        if (typ != 0) {
            // Condition : <cond> (typ), <value> (siz, 3 = toujours fausse), <action> (sub).
            require((type & 1U) == 0);
            constexpr std::array<CheatTest, 7> tests{CheatTest::equal, CheatTest::not_equal, CheatTest::less_signed,
                                                     CheatTest::greater_signed, CheatTest::less, CheatTest::greater,
                                                     CheatTest::any_set};
            CheatOperation operation;
            if (siz == 3) {
                // « Toujours » : l'adresse doit tout de même être autorisée (GBATEK).
                require_access(replay_address(left & 0x00ff'ffffU), 1);
                operation.action = CheatAction::test;
                operation.test = CheatTest::never;
            } else {
                operation = test(tests[typ - 1U], replay_address(left & 0x00ff'ffffU), size, right);
            }
            constexpr std::array<Flow, 4> flows{Flow::next_codes, Flow::next_codes, Flow::block, Flow::rest};
            codes.push_back(guarded(operation, flows[sub], sub + 1U));
            continue;
        }
        switch (type) {
        case 0x00:  // [a..a+xxxxxx] = yy
            codes.push_back(plain(slide(replay_address(left & 0x00ff'ffffU), 1, right & 0xffU, (right >> 8U) + 1U, 1, 0)));
            break;
        case 0x02:  // [a..a+xxxx*2] = yyyy
            codes.push_back(plain(slide(replay_address(left & 0x00ff'ffffU), 2, right & 0xffffU, (right >> 16U) + 1U, 2, 0)));
            break;
        case 0x04: codes.push_back(plain(memory_operation(CheatAction::write, replay_address(left & 0x00ff'ffffU), 4, right))); break;
        case 0x40: case 0x42: case 0x44: {
            // [[a] + décalage] = valeur
            auto operation = memory_operation(CheatAction::write_pointer, replay_address(left & 0x00ff'ffffU), 4, 0);
            operation.size = static_cast<std::uint8_t>(size);
            operation.value = right & value_mask(size);
            operation.offset = size == 1 ? right >> 8U : size == 2 ? (right >> 16U) * 2U : 0U;
            codes.push_back(plain(operation));
            break;
        }
        case 0x80: case 0x82: case 0x84:
            codes.push_back(plain(memory_operation(CheatAction::add, replay_address(left & 0x00ff'ffffU), size, right)));
            break;
        case 0xc4: require((right & 0xffff'0000U) == 0); break;  // adresse d'accroche : rien à exécuter ici
        case 0xc6: case 0xc7:
            codes.push_back(plain(memory_operation(CheatAction::write, 0x0400'0000U | (left & 0x00ff'ffffU),
                                                   type == 0xc6 ? 2 : 4, right)));
            break;
        default: require(false);
        }
    }
    return codes;
}

/** Codebreaker non chiffré, lignes `XAAAAAAA YYYY` (GBATEK). */
[[nodiscard]] std::vector<Code> codebreaker_codes(const std::vector<Line>& lines) {
    std::vector<Code> codes;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto [left, right] = lines[index];
        const auto address = left & 0x0fff'ffffU;
        const auto next_line = [&] {
            require(index + 1 < lines.size());
            return lines[++index];
        };
        switch (left >> 28U) {
        case 0x0: require((left & 0x0fff'0000U) == 0 && right <= 0xfU); break;  // identifiant du jeu
        case 0x1: require(right <= 7U); break;                                    // adresse d'accroche
        case 0x2: codes.push_back(plain(memory_operation(CheatAction::bit_or, address, 2, right))); break;
        case 0x3: codes.push_back(plain(memory_operation(CheatAction::write, address, 1, right))); break;
        case 0x4: {
            // Paramètres iiiicccc ssss : pas de la valeur, nombre, pas de l'adresse.
            const auto [parameters, step] = next_line();
            codes.push_back(plain(slide(address, 2, right, parameters & 0xffffU, step, parameters >> 16U)));
            break;
        }
        case 0x5: {
            // cccc octets, six par ligne suivante, dans l'ordre où ils s'écrivent.
            Code code;
            for (std::uint32_t written = 0; written < right;) {
                const auto [high, low] = next_line();
                const std::array<std::uint32_t, 6> bytes{high >> 24U, (high >> 16U) & 0xffU, (high >> 8U) & 0xffU,
                                                         high & 0xffU, low >> 8U, low & 0xffU};
                for (const auto byte : bytes) {
                    if (written == right) {
                        require(byte == 0);  // remplissage de la dernière ligne
                        continue;
                    }
                    code.operations.push_back(memory_operation(CheatAction::write, address + written++, 1, byte));
                }
            }
            codes.push_back(std::move(code));
            break;
        }
        case 0x6: codes.push_back(plain(memory_operation(CheatAction::bit_and, address, 2, right))); break;
        case 0x7: codes.push_back(guarded(test(CheatTest::equal, address, 2, right), Flow::next_codes, 1)); break;
        case 0x8: codes.push_back(plain(memory_operation(CheatAction::write, address, 2, right))); break;
        case 0x9: throw CheatError("les codes Codebreaker chiffrés (type 9) ne sont pas pris en charge");
        case 0xa: codes.push_back(guarded(test(CheatTest::not_equal, address, 2, right), Flow::next_codes, 1)); break;
        case 0xb: codes.push_back(guarded(test(CheatTest::greater_signed, address, 2, right), Flow::next_codes, 1)); break;
        case 0xc: codes.push_back(guarded(test(CheatTest::less_signed, address, 2, right), Flow::next_codes, 1)); break;
        case 0xd:
            // Touches tenues : leurs bits de KEYINPUT valent 0.
            require(left == 0xd000'0020U);
            codes.push_back(guarded(test(CheatTest::none_set, 0x0400'0130U, 2, right), Flow::next_codes, 1));
            break;
        case 0xe: codes.push_back(plain(memory_operation(CheatAction::add, address, 2, right))); break;
        case 0xf: codes.push_back(guarded(test(CheatTest::any_set, address, 2, right), Flow::next_codes, 1)); break;
        default: break;
        }
    }
    return codes;
}

[[nodiscard]] std::vector<Line> decrypt(std::vector<Line> lines, const detail::CheatSeeds& seeds) {
    for (auto& line : lines) line = detail::decrypt_cheat_line(line, seeds);
    return lines;
}

/**
 * Lignes de 12 chiffres (Codebreaker) ou de 16 (GameShark v1/v2, Action
 * Replay v3, chiffrés comme l'appareil les affiche). Les deux moitiés d'une
 * ligne peuvent être séparées ; tout ce qui n'est pas hexadécimal sépare.
 */
[[nodiscard]] std::vector<CheatOperation> parse_gba(std::string_view text) {
    std::vector<std::string_view> tokens;
    for (std::size_t position = 0; position < text.size();) {
        if (hex_digit(text[position]) < 0) { ++position; continue; }
        auto end = position;
        while (end < text.size() && hex_digit(text[end]) >= 0) ++end;
        tokens.push_back(text.substr(position, end - position));
        position = end;
    }
    std::vector<std::string> lines;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto token = tokens[index];
        if (token.size() == 12 || token.size() == 16) {
            lines.emplace_back(token);
        } else if (token.size() == 8 && index + 1 < tokens.size() &&
                   (tokens[index + 1].size() == 4 || tokens[index + 1].size() == 8)) {
            lines.push_back(std::string(token) + std::string(tokens[++index]));
        } else {
            throw CheatError(quoted(token) + " : une ligne GBA compte 12 chiffres (Codebreaker) "
                             "ou 16 (GameShark, Action Replay)");
        }
    }
    if (lines.empty()) throw CheatError("code vide");
    const auto length = lines.front().size();
    if (std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.size() != length; }))
        throw CheatError("lignes Codebreaker et GameShark/Action Replay mélangées dans un même code");
    std::vector<Line> values;
    for (const auto& line : lines) values.push_back({hex_value(std::string_view(line).substr(0, 8)),
                                                     hex_value(std::string_view(line).substr(8))});
    if (length == 12) {
        try {
            return flatten(codebreaker_codes(values));
        } catch (const Invalid&) {
            throw CheatError("code Codebreaker illisible");
        }
    }
    // Le format se reconnaît au déchiffrement qui donne des lignes valides.
    std::string unsupported;
    for (const auto* seeds : {&detail::gameshark_seeds, &detail::action_replay_seeds}) {
        try {
            const auto clear = decrypt(values, *seeds);
            return flatten(seeds == &detail::gameshark_seeds ? gameshark_codes(clear) : action_replay_codes(clear));
        } catch (const Unsupported& error) {
            if (unsupported.empty()) unsupported = error.what();
        } catch (const Invalid&) {
        }
    }
    if (!unsupported.empty()) throw CheatError(unsupported);
    throw CheatError("code illisible : ni le chiffrement GameShark v1/v2 ni celui de l'Action Replay v3 "
                     "n'en tirent des lignes valides");
}

[[nodiscard]] std::int64_t signed_value(std::uint32_t value, int size) noexcept {
    const auto bits = 8U * static_cast<unsigned>(size);
    const auto sign = std::uint64_t{1} << (bits - 1U);
    const auto masked = static_cast<std::uint64_t>(value & value_mask(size));
    return static_cast<std::int64_t>(masked ^ sign) - static_cast<std::int64_t>(sign);
}

[[nodiscard]] bool passes(const CheatOperation& operation, std::uint32_t memory) noexcept {
    const auto value = operation.value;
    switch (operation.test) {
    case CheatTest::equal: return memory == value;
    case CheatTest::not_equal: return memory != value;
    case CheatTest::less_signed: return signed_value(memory, operation.size) < signed_value(value, operation.size);
    case CheatTest::greater_signed: return signed_value(memory, operation.size) > signed_value(value, operation.size);
    case CheatTest::less: return memory < value;
    case CheatTest::greater: return memory > value;
    case CheatTest::any_set: return (memory & value) != 0;
    case CheatTest::none_set: return (memory & value) == 0;
    case CheatTest::never: break;
    }
    return false;
}

} // namespace

Cheat parse_cheat(Console console, std::string_view code) {
    Cheat cheat;
    cheat.console = console;
    if (console == Console::game_boy_advance) {
        cheat.operations = parse_gba(code);
    } else {
        cheat.operations = parse_game_boy(code);
        if (cheat.operations.empty()) throw CheatError("code vide");
    }
    return cheat;
}

void run_cheat(const Cheat& cheat, CheatMemory& memory) {
    const auto& operations = cheat.operations;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        const int size = operation.size;
        const auto mask = value_mask(size);
        switch (operation.action) {
        case CheatAction::write: {
            auto address = operation.address;
            auto value = operation.value;
            for (std::uint32_t step = 0; step < operation.count; ++step) {
                memory.write(address, size, value & mask);
                address += operation.address_step;
                value += operation.value_step;
            }
            break;
        }
        case CheatAction::add:
            memory.write(operation.address, size, (memory.read(operation.address, size) + operation.value) & mask);
            break;
        case CheatAction::bit_or:
            memory.write(operation.address, size, memory.read(operation.address, size) | operation.value);
            break;
        case CheatAction::bit_and:
            memory.write(operation.address, size, memory.read(operation.address, size) & operation.value);
            break;
        case CheatAction::write_pointer:
            memory.write(memory.read(operation.address, 4) + operation.offset, size, operation.value);
            break;
        case CheatAction::test:
            if (operation.test == CheatTest::never || !passes(operation, memory.read(operation.address, size)))
                index += operation.skip;
            break;
        case CheatAction::jump: index += operation.skip; break;
        case CheatAction::rom_patch: break;
        }
    }
}

namespace detail {

namespace {
constexpr std::uint32_t tea_delta = 0x9e37'79b9U;
} // namespace

std::array<std::uint32_t, 2> encrypt_cheat_line(std::array<std::uint32_t, 2> line, const CheatSeeds& seeds) noexcept {
    auto [left, right] = line;
    std::uint32_t sum = 0;
    for (int round = 0; round < 32; ++round) {
        sum += tea_delta;
        left += ((right << 4U) + seeds[0]) ^ (right + sum) ^ ((right >> 5U) + seeds[1]);
        right += ((left << 4U) + seeds[2]) ^ (left + sum) ^ ((left >> 5U) + seeds[3]);
    }
    return {left, right};
}

std::array<std::uint32_t, 2> decrypt_cheat_line(std::array<std::uint32_t, 2> line, const CheatSeeds& seeds) noexcept {
    auto [left, right] = line;
    std::uint32_t sum = tea_delta * 32U;
    for (int round = 0; round < 32; ++round) {
        right -= ((left << 4U) + seeds[2]) ^ (left + sum) ^ ((left >> 5U) + seeds[3]);
        left -= ((right << 4U) + seeds[0]) ^ (right + sum) ^ ((right >> 5U) + seeds[1]);
        sum -= tea_delta;
    }
    return {left, right};
}

} // namespace detail

} // namespace ravenemu
