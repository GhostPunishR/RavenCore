// Tests du PPU GB/GBC : Bus vidéo : accès du CPU à la VRAM, à l'OAM et à la RAM couleur (ppu/video_bus.hpp, ppu/color_ram.hpp).

#include "ppu_test_support.hpp"

namespace ravenemu::cgb::testing {

void video_bus_lock_boundary_test() {
    for (const auto hardware_mode : {gbc::HardwareMode::dmg,
                                     gbc::HardwareMode::cgb_compatibility,
                                     gbc::HardwareMode::cgb_native}) {
        InterruptController interrupts;
        Ppu ppu(interrupts, hardware_mode);
        ppu.vram[0] = 0x12;
        ppu.write_oam_direct(0, 0x34);

        check(ppu.mode() == Ppu::mode_oam, "précondition mode 2 absente");
        check(ppu.read_vram(0) == 0x12, "VRAM bloquée à tort pendant le mode 2");
        ppu.write_vram(0, 0x56);
        check(ppu.read_oam(0) == 0xff, "lecture OAM autorisée pendant le mode 2");
        ppu.write_oam(0, 0x78);
        check(ppu.oam[0] == 0x34, "écriture OAM acceptée pendant le mode 2");

        while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
        check(ppu.read_vram(0) == 0xff && ppu.read_oam(0) == 0xff,
              "mémoire vidéo lisible par le CPU pendant le mode 3");
        ppu.write_vram(0, 0x9a);
        ppu.write_oam(0, 0xbc);
        check(ppu.vram[0] == 0x56 && ppu.oam[0] == 0x34,
              "écriture vidéo mode 3 non rejetée");

        while (ppu.mode() == Ppu::mode_transfer) ppu.tick(1);
        check(ppu.mode() == Ppu::mode_hblank && ppu.read_vram(0) == 0x56 &&
              ppu.read_oam(0) == 0x34,
              "portes VRAM/OAM non rouvertes à l'entrée de HBlank");
        ppu.write_vram(0, 0xde);
        ppu.write_oam(0, 0xf0);
        check(ppu.vram[0] == 0xde && ppu.oam[0] == 0xf0,
              "écriture vidéo HBlank rejetée");
    }
}

void dmg_lcd_startup_bus_gate_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gbc::HardwareMode::dmg);
    ppu.write_lcdc(0x11);
    ppu.write_vram(0, 0x10);
    ppu.write_oam(0, 0x20);
    ppu.write_lcdc(0x91);

    ppu.tick(75);
    check(ppu.mode() == Ppu::mode_hblank && ppu.read_vram(0) == 0x10 &&
          ppu.read_oam(0) == 0x20,
          "portes vidéo fermées avant le transfert partiel de la ligne LCD initiale");
    ppu.tick(1);
    check(ppu.line_dot() == 76 && ppu.mode() == Ppu::mode_hblank,
          "mode 3 DMG publié sans son retard d'activation LCD");
    ppu.write_vram(0, 0x11);
    ppu.write_oam(0, 0x21);
    check(ppu.read_vram(0) == 0x11 && ppu.read_oam(0) == 0x21,
          "ouverture bus de la première ligne DMG non conservée");

    ppu.tick(4);
    check(ppu.mode() == Ppu::mode_transfer && ppu.read_vram(0) == 0xff &&
          ppu.read_oam(0) == 0xff,
          "portes vidéo DMG non fermées avec la publication du mode 3");
    while (ppu.transfer_x() < Ppu::width) ppu.tick(1);
    check(ppu.mode() == Ppu::mode_transfer && ppu.read_vram(0) == 0xff,
          "fin interne du transfert DMG a rouvert le bus avant le front publié");
    ppu.tick(4);
    check(ppu.mode() == Ppu::mode_hblank && ppu.read_vram(0) == 0x11 &&
          ppu.read_oam(0) == 0x21,
          "portes vidéo DMG non rouvertes après le retard de fin de mode 3");

    while (ppu.ly() == 0) ppu.tick(1);
    check(ppu.mode() == Ppu::mode_hblank && ppu.read_oam(0) == 0xff,
          "scan OAM interne DMG n'a pas fermé sa porte de lecture");
    ppu.write_oam(0, 0x22);
    check(ppu.oam[0] == 0x22,
          "porte d'écriture OAM DMG n'a pas conservé son front distinct au démarrage de ligne");
    ppu.tick(4);
    ppu.write_oam(0, 0x23);
    check(ppu.mode() == Ppu::mode_oam && ppu.oam[0] == 0x22,
          "écriture OAM acceptée après publication du mode 2 DMG");
}

void cgb_lcd_startup_bus_gate_test() {
    for (const auto hardware_mode : {gbc::HardwareMode::cgb_compatibility,
                                     gbc::HardwareMode::cgb_native}) {
        InterruptController interrupts;
        Ppu ppu(interrupts, hardware_mode);
        ppu.write_lcdc(0x11);
        ppu.write_vram(0, 0x31);
        ppu.write_oam(0, 0x41);
        ppu.write_lcdc(0x91);
        ppu.tick(75);
        check(ppu.read_vram(0) == 0x31 && ppu.read_oam(0) == 0x41,
              "matériel CGB ferme le bus avant le mode 3 initial");
        ppu.tick(1);
        check(ppu.mode() == Ppu::mode_transfer && ppu.read_vram(0) == 0xff &&
              ppu.read_oam(0) == 0xff,
              "matériel CGB ne ferme pas le bus au front interne du mode 3 initial");
        ppu.write_vram(0, 0x32);
        ppu.write_oam(0, 0x42);
        check(ppu.vram[0] == 0x31 && ppu.oam[0] == 0x41,
              "écriture vidéo CGB acceptée au début du mode 3 initial");
    }
}

void cgb_cram_lock_and_autoincrement_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gbc::HardwareMode::cgb_native);
    ppu.bg_cram[63] = 0x12;
    ppu.obj_cram[5] = 0x34;
    ppu.write_bcps(0xbf); // auto-incrément, adresse 63
    ppu.write_ocps(0x85); // auto-incrément, adresse 5
    while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);

    ppu.write_bcpd(0x56);
    ppu.write_ocpd(0x78);
    check(ppu.bg_cram[63] == 0x12 && ppu.obj_cram[5] == 0x34,
          "écriture CRAM mode 3 non rejetée");
    check(ppu.read_bcps() == 0xc0 && ppu.read_ocps() == 0xc6,
          "index CRAM non auto-incrémenté après une écriture bloquée");
    check(ppu.read_bcpd() == 0xff && ppu.read_ocpd() == 0xff,
          "lecture CRAM autorisée pendant le mode 3");

    ppu.write_bcps(0x89);
    check(ppu.read_bcps() == 0xc9,
          "registre d'index CRAM bloqué à tort pendant le mode 3");
    while (ppu.mode() == Ppu::mode_transfer) ppu.tick(1);
    ppu.write_bcps(0x3f);
    ppu.write_ocps(0x05);
    check(ppu.read_bcpd() == 0x12 && ppu.read_ocpd() == 0x34,
          "CRAM non rouverte en HBlank ou donnée bloquée modifiée");
}

void lcd_disable_releases_video_bus_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gbc::HardwareMode::cgb_native);
    ppu.vram[0] = 0x11;
    ppu.write_oam_direct(0, 0x22);
    ppu.bg_cram[0] = 0x33;
    while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
    ppu.write_lcdc(0x11);

    check(ppu.ly() == 0 && ppu.mode() == Ppu::mode_hblank &&
          ppu.read_vram(0) == 0x11 && ppu.read_oam(0) == 0x22,
          "LCDC.7 n'a pas libéré immédiatement VRAM/OAM");
    ppu.write_vram(0, 0x44);
    ppu.write_oam(0, 0x55);
    ppu.write_bcps(0);
    ppu.write_bcpd(0x66);
    check(ppu.vram[0] == 0x44 && ppu.oam[0] == 0x55 && ppu.bg_cram[0] == 0x66,
          "mémoire vidéo encore verrouillée LCD éteint");
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    video_bus_lock_boundary_test();
    dmg_lcd_startup_bus_gate_test();
    cgb_lcd_startup_bus_gate_test();
    cgb_cram_lock_and_autoincrement_test();
    lcd_disable_releases_video_bus_test();
    return 0;
}
