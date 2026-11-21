// Tests du PPU GB/GBC : Lignes, modes, STAT et LYC (ppu/line_timing.hpp).

#include "ppu_test_support.hpp"

namespace ravenemu::cgb::testing {

/**
 * La source mode 2 de la ligne 0 : point 4 sur DMG (Mooneye,
 * intr_1_2_timing-GS), point 0 sur CGB (Mealybug Tearoom, qui compense quatre
 * points de retard sur les autres lignes).
 */
void line0_mode2_stat_test() {
    for (const auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_native,
                            gbc::HardwareMode::cgb_compatibility}) {
        InterruptController interrupts;
        Ppu ppu(interrupts, mode);
        ppu.write_stat(0x20);
        ppu.tick(154 * 456 - 1);
        check(ppu.line_dot() == 455, "fin de la ligne 153 manquée");
        interrupts.flags = 0;
        ppu.tick(1);
        const bool at_dot_0 = (interrupts.flags & interrupt_mask(Interrupt::stat)) != 0;
        ppu.tick(4);
        const bool by_dot_4 = (interrupts.flags & interrupt_mask(Interrupt::stat)) != 0;
        if (mode == gbc::HardwareMode::dmg) {
            check(!at_dot_0 && by_dot_4, "DMG : IRQ mode 2 de la ligne 0 hors du point 4");
        } else {
            check(at_dot_0, "CGB : IRQ mode 2 de la ligne 0 retardée");
        }
    }
}

void stat_line_edge_test() {
    InterruptController interrupts;
    interrupts.flags = 0;
    Ppu ppu(interrupts, gbc::HardwareMode::cgb_native);
    ppu.write_stat(0x20); // IRQ mode 2
    ppu.write_lcdc(0x91);
    check((interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
          "front montant de la ligne STAT mode 2 absent");
    interrupts.flags = 0;
    ppu.write_stat(0x60); // ajoute LYC alors que la ligne STAT est déjà haute
    check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
          "IRQ STAT redemandée sans nouveau front de ligne");
}

void mode2_stat_lead_test() {
    {
        InterruptController interrupts;
        interrupts.flags = 0;
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
        ppu.tick(252); // HBlank de la ligne 0.
        ppu.write_stat(0x20);
        interrupts.flags = 0;
        ppu.tick(199);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
              "IRQ STAT mode 2 levee avant le dot 452");
        ppu.tick(1);
        check(ppu.mode() == Ppu::mode_hblank && ppu.line_dot() == 452 &&
              (interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
              "signal STAT mode 2 absent quatre dots avant la ligne suivante");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::cgb_compatibility);
        ppu.write_stat(0x20);
        ppu.tick(143 * 456 + 451);
        interrupts.flags = 0;
        ppu.tick(1);
        check(ppu.ly() == 143 && ppu.line_dot() == 452 &&
              (interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
              "CGB n'anticipe pas le front STAT mode 2 avant VBlank");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
        ppu.write_stat(0x20);
        ppu.tick(143 * 456 + 451);
        interrupts.flags = 0;
        ppu.tick(1);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
              "DMG anticipe à tort la source STAT mode 2 sur la ligne 143");
        ppu.tick(4);
        check(ppu.ly() == 144 && ppu.mode() == Ppu::mode_vblank &&
              (interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
              "impulsion STAT mode 2 DMG absente à l'entrée de VBlank");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::cgb_native);
        ppu.write_stat(0x20);
        ppu.tick(143 * 456 + 451);
        interrupts.flags = 0;
        ppu.tick(1);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
              "CGB natif n'anticipe pas la source STAT mode 2 sur la ligne 143");
        interrupts.flags = 0;
        ppu.tick(4);
        check(ppu.ly() == 144 && ppu.mode() == Ppu::mode_vblank &&
              (interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
              "CGB redemande une IRQ STAT sans nouveau front à l'entrée de VBlank");
    }
}

void stat_source_blocking_test() {
    for (const auto hardware_mode : {gbc::HardwareMode::dmg,
                                     gbc::HardwareMode::cgb_compatibility,
                                     gbc::HardwareMode::cgb_native}) {
        InterruptController interrupts;
        Ppu ppu(interrupts, hardware_mode);
        while (ppu.ly() != 143 || ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
        ppu.write_stat(0x18); // sources mode 0 et mode 1 consécutives
        interrupts.flags = 0;
        while (ppu.mode() != Ppu::mode_hblank) ppu.tick(1);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
              "front STAT mode 0 absent avant VBlank");
        interrupts.flags = 0;
        while (ppu.mode() != Ppu::mode_vblank) ppu.tick(1);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
              "source mode 1 non bloquée par la ligne STAT mode 0 déjà haute");
    }

    InterruptController interrupts;
    Ppu ppu(interrupts, gbc::HardwareMode::dmg);
    ppu.write_lyc(0);
    ppu.write_stat(0x48); // LYC et mode 0
    interrupts.flags = 0;
    while (ppu.mode() != Ppu::mode_hblank) ppu.tick(1);
    check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
          "source mode 0 non bloquée par une coïncidence LYC maintenue");
}

void lcd_off_lyc_latch_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gbc::HardwareMode::dmg);
    ppu.write_lcdc(0x11);
    ppu.write_lcdc(0x91);
    ppu.tick(456 * 144);
    ppu.write_lyc(144);
    check((ppu.read_stat() & 4) != 0, "précondition LY=LYC absente");
    ppu.write_lcdc(0x00);
    ppu.write_lyc(1);
    check((ppu.read_stat() & 4) != 0,
          "comparateur LY=LYC modifié alors que son horloge LCD est coupée");
    ppu.write_lcdc(0x80);
    check((ppu.read_stat() & 4) == 0,
          "comparateur LY=LYC non réévalué à l'activation LCD");
}

void ly_153_quirk_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gbc::HardwareMode::dmg);
    ppu.write_lcdc(0x11);
    ppu.write_lcdc(0x91);
    ppu.tick(452 + 456 * 152);
    check(ppu.ly() == 153 && ppu.line_dot() == 0,
          "PPU n'entre pas sur la ligne interne 153 au bon dot");
    ppu.write_lyc(0);
    ppu.write_stat(0x40);
    interrupts.flags = 0;
    ppu.tick(3);
    check(ppu.ly() == 153, "LY passe à zéro avant le dot 4 de la ligne 153");
    ppu.tick(1);
    check(ppu.ly() == 0, "LY ne présente pas zéro au dot 4 de la ligne 153");
    check((ppu.read_stat() & 4) != 0 &&
          (interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
          "coïncidence LYC/IRQ STAT absente lors du changement LY 153→0");
}

void dmg_stat_write_glitch_test() {
    InterruptController dmg_interrupts;
    Ppu dmg(dmg_interrupts, gbc::HardwareMode::dmg);
    dmg.write_lcdc(0x11);
    dmg.write_lcdc(0x91); // mode 2, aucune source STAT activée
    dmg_interrupts.flags = 0;
    dmg.write_stat(0x00);
    check((dmg_interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
          "glitch IRQ d'écriture STAT absent sur DMG en mode 2");

    InterruptController cgb_interrupts;
    Ppu cgb(cgb_interrupts, gbc::HardwareMode::cgb_native);
    cgb.write_lcdc(0x11);
    cgb.write_lcdc(0x91);
    cgb_interrupts.flags = 0;
    cgb.write_stat(0x00);
    check((cgb_interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
          "glitch d'écriture STAT DMG appliqué à tort au CGB");
}

/**
 * Fausse interruption du DMG à l'écriture de STAT (GBMicrotest,
 * stat_write_glitch) : elle suit l'état du PPU au début du M-cycle
 * d'écriture, et part en H-Blank ou en V-Blank, jamais en mode 2 ni en mode 3.
 * LYC hors de portée, pour isoler la source de mode.
 */
void dmg_stat_write_glitch_phase_test() {
    const auto glitch_at = [](int line, int start_dot) {
        auto image = std::make_shared<const std::vector<std::uint8_t>>(ravenemu::testing::minimal_game_boy_rom());
        Machine machine(image, [] { return std::int64_t{0}; }, gbc::HardwareMode::dmg);
        machine.bus.write(0xff45, 0x90);
        machine.bus.write(0xff41, 0x00);
        while (!(machine.ppu.ly() == line && machine.ppu.line_dot() == start_dot)) machine.bus.tick_mcycle();
        machine.interrupts.flags = 0;
        machine.bus.write_mcycle(0xff41, 0x00);
        return (machine.interrupts.flags & interrupt_mask(Interrupt::stat)) != 0;
    };
    check(!glitch_at(10, 248), "DMG : STAT écrit sur le point qui ouvre le H-Blank a levé l'interruption");
    check(glitch_at(10, 256), "DMG : STAT écrit en H-Blank sans interruption");
    check(glitch_at(10, 452), "DMG : STAT écrit au dernier M-cycle du H-Blank sans interruption");
    check(!glitch_at(11, 4), "DMG : STAT écrit en mode 2 a levé l'interruption");
    check(!glitch_at(11, 100), "DMG : STAT écrit en mode 3 a levé l'interruption");
    check(glitch_at(150, 100), "DMG : STAT écrit en V-Blank sans interruption");
}

/**
 * Le mode 0 partiel qui ouvre la première ligne après l'allumage de l'écran
 * ne lève pas l'interruption H-Blank : elle attend la fin du mode 3 de cette
 * ligne (GBMicrotest, int_hblank_nops et int_hblank_incs).
 */
void startup_line_hblank_interrupt_test() {
    for (const auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_native}) {
        InterruptController interrupts;
        Ppu ppu(interrupts, mode);
        ppu.write_lcdc(0x11);
        ppu.write_stat(0x08);
        ppu.write_lyc(0x90);
        interrupts.flags = 0;
        ppu.write_lcdc(0x91);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
              "interruption H-Blank levée dès l'allumage de l'écran");
        while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
              "interruption H-Blank levée pendant le mode 0 de démarrage");
        while (ppu.mode() == Ppu::mode_transfer) ppu.tick(1);
        check((interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
              "interruption H-Blank absente à la fin du mode 3 de la première ligne");
    }
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    stat_line_edge_test();
    line0_mode2_stat_test();
    mode2_stat_lead_test();
    stat_source_blocking_test();
    lcd_off_lyc_latch_test();
    ly_153_quirk_test();
    dmg_stat_write_glitch_test();
    dmg_stat_write_glitch_phase_test();
    startup_line_hblank_interrupt_test();
    return 0;
}
