#include "ravenemu/core.hpp"
#include "check.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using ravenemu::Cheat;
using ravenemu::CheatAction;
using ravenemu::CheatError;
using ravenemu::CheatTest;
using ravenemu::Console;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {

using Line = std::array<std::uint32_t, 2>;

// Mémoire GBA factice, octet par octet, petit-boutiste comme la console.
class FakeMemory final : public ravenemu::CheatMemory {
public:
    std::uint32_t read(std::uint32_t address, int size) override {
        std::uint32_t value = 0;
        for (int index = 0; index < size; ++index)
            value |= static_cast<std::uint32_t>(bytes[address + static_cast<std::uint32_t>(index)])
                     << (8U * static_cast<unsigned>(index));
        return value;
    }
    void write(std::uint32_t address, int size, std::uint32_t value) override {
        for (int index = 0; index < size; ++index)
            bytes[address + static_cast<std::uint32_t>(index)] =
                static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(index)));
    }
    std::map<std::uint32_t, std::uint8_t> bytes;
};

// Lignes en clair, chiffrées comme l'appareil les affiche.
std::string encrypted(const std::vector<Line>& lines, const ravenemu::detail::CheatSeeds& seeds) {
    std::string text;
    for (const auto& line : lines) {
        const auto value = ravenemu::detail::encrypt_cheat_line(line, seeds);
        char buffer[24];
        std::snprintf(buffer, sizeof buffer, "%08X %08X", static_cast<unsigned>(value[0]), static_cast<unsigned>(value[1]));
        if (!text.empty()) text += '+';
        text += buffer;
    }
    return text;
}
std::string action_replay(const std::vector<Line>& lines) {
    return encrypted(lines, ravenemu::detail::action_replay_seeds);
}
std::string gameshark(const std::vector<Line>& lines) { return encrypted(lines, ravenemu::detail::gameshark_seeds); }

Cheat gba(const std::string& code) { return ravenemu::parse_cheat(Console::game_boy_advance, code); }
Cheat game_boy(const std::string& code) { return ravenemu::parse_cheat(Console::game_boy, code); }

void expect_gba_error(const std::string& code, const std::string& fragment, std::string_view message) {
    try {
        static_cast<void>(gba(code));
    } catch (const CheatError& error) {
        check(std::string(error.what()).find(fragment) != std::string::npos, message);
        return;
    }
    check(false, message);
}

void encryption_test() {
    // Vecteur de référence de l'algorithme (clé nulle, bloc nul).
    const auto zero = ravenemu::detail::encrypt_cheat_line({0, 0}, {0, 0, 0, 0});
    check(zero[0] == 0x41ea3a0aU && zero[1] == 0x94baa940U, "chiffrement : vecteur de référence");
    for (const auto* seeds : {&ravenemu::detail::gameshark_seeds, &ravenemu::detail::action_replay_seeds}) {
        const Line clear{0x12345678U, 0x9abcdef0U};
        const auto line = ravenemu::detail::encrypt_cheat_line(clear, *seeds);
        check(line != clear && ravenemu::detail::decrypt_cheat_line(line, *seeds) == clear, "chiffrement : aller-retour");
    }
}

void game_boy_test() {
    // Nouvelle donnée 3E à 4A12 si l'octet vaut 05 : GI = rotation de 05 ^ BA = FE.
    const auto genie = game_boy("3EA-12B-FAE");
    check(genie.console == Console::game_boy && genie.operations.size() == 1, "Game Genie : une opération");
    const auto& patch = genie.operations.front();
    check(patch.action == CheatAction::rom_patch && patch.address == 0x4a12U && patch.value == 0x3eU &&
              patch.original == std::optional<std::uint8_t>{0x05}, "Game Genie : décodage Pan Docs");
    const auto short_genie = game_boy("3ea12b");
    check(short_genie.operations.front().address == 0x4a12U && !short_genie.operations.front().original,
          "Game Genie à six chiffres : sans comparaison");

    // Exemple de Pan Docs : banque 01, valeur 02, adresse CD38.
    const auto shark = game_boy("010238CD");
    const auto& write = shark.operations.front();
    check(write.action == CheatAction::write && write.bank == 0x01 && write.value == 0x02 && write.address == 0xcd38U,
          "GameShark : décodage Pan Docs");
    check(game_boy("3EA-12B-FAE + 010238CD\n91FF00D0").operations.size() == 3, "Game Boy : codes mélangés");

    expect_failure<CheatError>([] { static_cast<void>(game_boy("3EA-127-FAE")); }, "Game Genie hors ROM accepté");
    expect_failure<CheatError>([] { static_cast<void>(game_boy("01021234")); }, "GameShark en ROM accepté");
    expect_failure<CheatError>([] { static_cast<void>(game_boy("01GG38CD")); }, "code non hexadécimal accepté");
    expect_failure<CheatError>([] { static_cast<void>(game_boy("0102")); }, "code trop court accepté");
    expect_failure<CheatError>([] { static_cast<void>(game_boy(" + ")); }, "code vide accepté");
}

void action_replay_test() {
    // Écriture de 32 bits en 0x02000100, séparateurs au choix.
    const auto code = action_replay({{0x04200100U, 0x12345678U}});
    for (const auto& text : {code, code.substr(0, 8) + code.substr(9), code.substr(0, 8) + ":" + code.substr(9)}) {
        const auto cheat = gba(text);
        check(cheat.operations.size() == 1, "Action Replay : une opération");
        const auto& write = cheat.operations.front();
        check(write.action == CheatAction::write && write.size == 4 && write.address == 0x02000100U &&
                  write.value == 0x12345678U, "Action Replay : écriture de 32 bits");
    }
    // Codes d'activation (accroche, identifiant) : rien à exécuter.
    check(gba(action_replay({{0xc4000100U, 0x00000000U}, {0x0badc0deU, 0x001dc0deU}})).operations.empty(),
          "Action Replay : code d'activation sans opération");

    // IF [02000000] = 12 THEN [02000001] = 34 (code suivant seulement).
    const auto guarded = gba(action_replay({{0x08200000U, 0x12U}, {0x00200001U, 0x34U}, {0x00200002U, 0x56U}}));
    for (const std::uint8_t value : {std::uint8_t{0x12}, std::uint8_t{0x00}}) {
        FakeMemory memory;
        memory.bytes[0x02000000U] = value;
        ravenemu::run_cheat(guarded, memory);
        check((memory.bytes[0x02000001U] == 0x34) == (value == 0x12), "Action Replay : condition sur le code suivant");
        check(memory.bytes[0x02000002U] == 0x56, "Action Replay : la condition ne couvre que le code suivant");
    }

    // Bloc IF / ELSE / ENDIF, puis un code hors du bloc.
    const auto block = gba(action_replay({
        {0x88200000U, 0x12U},                      // IF [02000000] = 12 (bloc)
        {0x00200010U, 0xaaU},                      //   [02000010] = AA
        {0x00000000U, 0x60000000U},                // ELSE
        {0x00200011U, 0xbbU},                      //   [02000011] = BB
        {0x00000000U, 0x40000000U},                // ENDIF
        {0x00200012U, 0xccU},                      // [02000012] = CC
    }));
    for (const std::uint8_t value : {std::uint8_t{0x12}, std::uint8_t{0x34}}) {
        FakeMemory memory;
        memory.bytes[0x02000000U] = value;
        ravenemu::run_cheat(block, memory);
        const bool taken = value == 0x12;
        check(memory.bytes[0x02000010U] == (taken ? 0xaa : 0) && memory.bytes[0x02000011U] == (taken ? 0 : 0xbb) &&
                  memory.bytes[0x02000012U] == 0xcc, "Action Replay : bloc IF/ELSE/ENDIF");
    }

    // Série de 16 bits : trois valeurs 0100, 0101, 0102, de quatre octets en quatre octets.
    const auto slide = gba(action_replay({{0x00000000U, 0x82200010U}, {0x00000100U, 0x01030002U}}));
    FakeMemory memory;
    ravenemu::run_cheat(slide, memory);
    check(memory.read(0x02000010U, 2) == 0x0100U && memory.read(0x02000014U, 2) == 0x0101U &&
              memory.read(0x02000018U, 2) == 0x0102U && memory.bytes.size() == 6, "Action Replay : série de 16 bits");

    // Écriture indirecte, addition, registre d'E/S.
    const auto pointer = gba(action_replay({{0x44300000U, 0xcafef00dU}, {0x82300004U, 0x0003U}, {0xc6000200U, 0x1234U}}));
    FakeMemory indirect;
    indirect.write(0x03000000U, 4, 0x02000040U);
    indirect.write(0x03000004U, 2, 0x0010U);
    ravenemu::run_cheat(pointer, indirect);
    check(indirect.read(0x02000040U, 4) == 0xcafef00dU && indirect.read(0x03000004U, 2) == 0x0013U &&
              indirect.read(0x04000200U, 2) == 0x1234U, "Action Replay : pointeur, addition, E/S");

    expect_gba_error(action_replay({{0x00000000U, 0x18000100U}, {0x1234U, 0}}), "patchs de ROM",
                     "Action Replay : patch de ROM accepté");
    expect_gba_error(action_replay({{0xdeadfaceU, 0x00001234U}}), "DEADFACE", "Action Replay : DEADFACE accepté");
}

void gameshark_test() {
    const auto write = gba(gameshark({{0x13000100U, 0xbeefU}}));
    check(write.operations.size() == 1 && write.operations.front().size == 2 &&
              write.operations.front().address == 0x03000100U && write.operations.front().value == 0xbeefU,
          "GameShark : écriture de 16 bits");

    // IF [02000010] = 0005 THEN les deux codes suivants ; puis la même valeur à deux adresses.
    const auto cheat = gba(gameshark({
        {0xe0020005U, 0x02000010U},
        {0x02000020U, 0x11U},
        {0x02000021U, 0x22U},
        {0x02000022U, 0x33U},
        {0x30000003U, 0xdeadbeefU},
        {0x02000030U, 0x02000034U},
    }));
    for (const std::uint32_t value : {0x0005U, 0x0006U}) {
        FakeMemory memory;
        memory.write(0x02000010U, 2, value);
        ravenemu::run_cheat(cheat, memory);
        const bool taken = value == 0x0005U;
        check(memory.bytes[0x02000020U] == (taken ? 0x11 : 0) && memory.bytes[0x02000021U] == (taken ? 0x22 : 0) &&
                  memory.bytes[0x02000022U] == 0x33, "GameShark : condition sur deux codes");
        check(memory.read(0x02000030U, 4) == 0xdeadbeefU && memory.read(0x02000034U, 4) == 0xdeadbeefU,
              "GameShark : même valeur à plusieurs adresses");
    }
    expect_gba_error(gameshark({{0x60000100U, 0x00001234U}}), "patchs de ROM", "GameShark : patch de ROM accepté");
    expect_gba_error(gameshark({{0xdeadfaceU, 0x00001234U}}), "DEADFACE", "GameShark : DEADFACE accepté");
}

void codebreaker_test() {
    // Code d'activation, écritures, série, chaîne d'octets, touches tenues.
    const auto cheat = gba("0000ABCD 000A\n1000048C 0007\n82000100 1234\n32000102 0056\n"
                           "42000200 0010+00010004 0002\n52000300 0007 11223344 5566 77000000 0000\n"
                           "D0000020 0001\n82000104 0099\nE2000100 0001");
    for (const std::uint32_t keys : {0x03feU, 0x03ffU}) {
        FakeMemory memory;
        memory.write(0x04000130U, 2, keys);  // KEYINPUT : bit à 0 = touche tenue
        ravenemu::run_cheat(cheat, memory);
        check(memory.read(0x02000100U, 2) == 0x1235U && memory.bytes[0x02000102U] == 0x56,
              "Codebreaker : écriture puis addition");
        check(memory.read(0x02000200U, 2) == 0x10U && memory.read(0x02000206U, 2) == 0x13U,
              "Codebreaker : série de 16 bits");
        check(memory.bytes[0x02000300U] == 0x11 && memory.bytes[0x02000306U] == 0x77 &&
                  memory.bytes.count(0x02000307U) == 0, "Codebreaker : chaîne d'octets");
        check((memory.read(0x02000104U, 2) == 0x99U) == (keys == 0x03feU), "Codebreaker : condition sur les touches");
    }
    expect_gba_error("9A123456 7890", "chiffrés", "Codebreaker chiffré accepté");
    expect_gba_error("82000101 1234", "Codebreaker", "Codebreaker : écriture de 16 bits non alignée acceptée");
    expect_gba_error("82000100 1234+0420010012345678", "mélangées", "formats mélangés acceptés");
    expect_gba_error("0420010", "12 chiffres", "ligne tronquée acceptée");
    expect_gba_error("", "vide", "code vide accepté");
}

} // namespace

int main() {
    encryption_test();
    game_boy_test();
    action_replay_test();
    gameshark_test();
    codebreaker_test();
    return 0;
}
