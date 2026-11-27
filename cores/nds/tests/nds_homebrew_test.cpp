#include "gpu/engine2d.hpp"
#include "machine/machine.hpp"

#include "check.hpp"

#include "../homebrew/charset.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Homebrew de démonstration (cores/nds/homebrew), compilé pour l'ARM9 et
// l'ARM7 puis démarré directement. Le texte des deux écrans est relu dans la
// carte du fond en VRAM ; l'image est vérifiée point par point.
// RAVENEMU_NDS_SCREENSHOT=chemin.ppm enregistre les deux écrans.
namespace {

using namespace ravenemu::nds;
using ravenemu::testing::check;

// Rangée `row` du fond BG0 du moteur 0 (A) ou 1 (B), sans espaces de fin.
std::string row_text(const Machine& machine, unsigned engine, unsigned row) {
    const auto area = engine == 0 ? Vram::bg_a : Vram::bg_b;
    const std::string charset{CHARSET};
    std::string text;
    for (unsigned x = 0; x < 32; ++x) {
        const auto tile = machine.hw.vram.read(area, 31U * 0x800U + (row * 32U + x) * 2U, 2) & 0x3ffU;
        text += tile < charset.size() ? charset[tile] : '?';
    }
    return text.substr(0, text.find_last_not_of(' ') + 1);
}

// Nombre écrit à la colonne 12 d'une rangée de l'écran du haut ; 0 s'il n'y en a pas.
unsigned number_at(const Machine& machine, unsigned row) {
    const auto text = row_text(machine, 0, row);
    if (text.size() <= 12 || text[12] < '0' || text[12] > '9') return 0;
    return static_cast<unsigned>(std::stoul(text.substr(12)));
}

void write_screenshot(const Machine& machine, const char* path) {
    std::ofstream out{path, std::ios::binary};
    out << "P6\n256 384\n255\n";
    for (const auto argb : machine.frame()) {
        const char rgb[3] = {static_cast<char>(argb >> 16U), static_cast<char>(argb >> 8U), static_cast<char>(argb)};
        out.write(rgb, 3);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage : nds_raven_homebrew_tests raven_hello.nds\n", stderr);
        return 2;
    }
    std::ifstream file{argv[1], std::ios::binary};
    std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    Machine machine{rom};
    for (int frame = 0; frame < 70; ++frame) machine.run_frame();

    check(row_text(machine, 0, 1) == " RAVENCORE - COEUR NDS" && row_text(machine, 0, 3) == " BONJOUR DEPUIS LE DS !",
          "écran du haut : titre et message");
    check(row_text(machine, 1, 1) == " ECRAN DU BAS - MOTEUR B", "écran du bas : moteur B");
    const auto frames = number_at(machine, 6);
    check(frames >= 60 && frames <= 70, "images comptées par VBlankIntrWait et l'IRQ de V-Blank");
    check(number_at(machine, 7) == 1, "une seconde comptée par l'IRQ du minuteur 0");
    check(row_text(machine, 0, 8) == " ARM7 IPC   42", "l'ARM7 répond 42 par la FIFO de l'IPC");
    check(row_text(machine, 0, 9) == " PSEUDO     RAVENEMU", "pseudo des réglages du firmware");
    check(number_at(machine, 10) == 1, "premier démarrage compté dans l'EEPROM");
    const auto& save = machine.hw.card.save();
    check(save.type() == SaveType::eeprom_64k, "puce reconnue seule : adresse de 2 octets, EEPROM de 64 Kio");
    check(save.dirty() && save.data()[0] == 'R' && save.data()[1] == 1, "sauvegarde écrite par le SPI de la cartouche");
    check(row_text(machine, 0, 13) == " AUCUNE", "aucune touche appuyée");
    check(row_text(machine, 0, 16) == " LU SUR LA CARTOUCHE PAR DMA",
          "message lu dans la ROM par le bus de la cartouche (B7, DMA, IRQ de fin)");
    machine.hw.keys = 0x3ffU & ~(1U | 8U);  // A et START
    machine.run_frame();
    machine.run_frame();
    check(row_text(machine, 0, 13) == " A START", "touches A et START");

    // Moteur A en haut (POWCNT1 bit 15). Le « R » du titre (tuile en colonne
    // 1, rangée 1) commence par quatre points allumés.
    const auto& image = machine.frame();
    const auto accent = to_argb(0x7e34, 0);
    const auto backdrop = to_argb(0x0821, 0);
    check(image[8 * 256 + 9] == accent && image[8 * 256 + 12] == accent && image[8 * 256 + 13] == backdrop,
          "titre en violet RavenEmu sur l'écran du haut");
    check(image[0] == backdrop && image[Hardware::screen_pixels] == backdrop, "fond des deux écrans");
    check(image[Hardware::screen_pixels + 8 * 256 + 9] == accent, "titre de l'écran du bas");
    check(machine.bios9.unsupported == 0 && machine.bios7.unsupported == 0, "aucune SWI inconnue");
    if (const char* path = std::getenv("RAVENEMU_NDS_SCREENSHOT")) write_screenshot(machine, path);

    // Deuxième démarrage avec le fichier de sauvegarde du premier : son type
    // vient de sa taille (64 Kio).
    const std::vector<std::uint8_t> file_data{save.data().begin(), save.data().end()};
    Machine again{rom};
    check(again.hw.card.save().load(file_data) && again.hw.card.save().type() == SaveType::eeprom_64k,
          "fichier de 64 Kio : EEPROM de 64 Kio");
    for (int frame = 0; frame < 10; ++frame) again.run_frame();
    check(number_at(again, 10) == 2, "deuxième démarrage : le compte repris de la sauvegarde");
    return 0;
}
