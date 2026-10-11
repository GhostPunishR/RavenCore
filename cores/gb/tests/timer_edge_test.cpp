#include "timer/timer.hpp"
#include "check.hpp"

using ravenemu::testing::check;

namespace ravenemu::cgb::testing {

void div_and_tac_falling_edge_test() {
    {
        InterruptController interrupts;
        Timer timer(interrupts, gb::HardwareMode::dmg);
        timer.write_div();
        timer.write_tac(0x05); // activation, bit 3 du compteur système
        timer.tick(8);         // bit sélectionné à 1
        timer.write_div();
        check(timer.read_tima() == 1, "reset DIV sans incrément TIMA sur front descendant");
    }

    {
        InterruptController interrupts;
        Timer timer(interrupts, gb::HardwareMode::dmg);
        timer.write_div();
        timer.write_tac(0x05);
        timer.tick(8);         // bit 3=1, bit 5=0
        timer.write_tac(0x06); // changement vers bit 5
        check(timer.read_tima() == 1, "changement TAC haut-vers-bas sans glitch TIMA");
    }
}

void dmg_cgb_disable_difference_test() {
    InterruptController dmg_interrupts;
    Timer dmg(dmg_interrupts, gb::HardwareMode::dmg);
    dmg.write_div();
    dmg.write_tac(0x05);
    dmg.tick(8);
    dmg.write_tac(0x00);
    check(dmg.read_tima() == 1, "DMG doit incrémenter TIMA quand TAC désactive un signal haut");

    InterruptController cgb_interrupts;
    Timer cgb(cgb_interrupts, gb::HardwareMode::cgb_native);
    cgb.write_div();
    cgb.write_tac(0x05);
    cgb.tick(8);
    cgb.write_tac(0x00);
    check(cgb.read_tima() == 0, "CGB ne doit pas appliquer le glitch de désactivation TAC du DMG");

    cgb.write_tac(0x05);
    check(cgb.read_tima() == 1,
          "CGB doit appliquer le front d'activation TAC sur une entree deja haute");
}

void overflow_reload_window_test() {
    InterruptController interrupts;
    interrupts.flags = 0;
    Timer timer(interrupts);
    timer.write_div();
    timer.set_tma(0x23);
    timer.write_tima(0xff);
    timer.write_tac(0x05);

    timer.tick(16);
    check(timer.read_tima() == 0x00, "TIMA ne reste pas à 00 pendant le délai d'overflow");
    check((interrupts.flags & interrupt_mask(Interrupt::timer)) == 0, "IRQ timer demandée avant le reload");
    timer.tick(3);
    check(timer.read_tima() == 0x00, "reload TIMA effectué avant quatre T-cycles");
    timer.tick(1);
    check(timer.read_tima() == 0x23, "TMA non copié dans TIMA au quatrième T-cycle");
    check((interrupts.flags & interrupt_mask(Interrupt::timer)) != 0, "IRQ timer absente au reload");

    timer.write_tima(0x99);
    check(timer.read_tima() == 0x23, "écriture TIMA acceptée pendant le cycle de reload");
    timer.set_tma(0x77);
    check(timer.read_tima() == 0x77, "écriture TMA non répercutée pendant le cycle de reload");
    timer.tick(1);
    timer.write_tima(0x55);
    check(timer.read_tima() == 0x55, "écriture TIMA encore bloquée après le cycle de reload");
}

void tima_write_cancels_pending_reload_test() {
    InterruptController interrupts;
    interrupts.flags = 0;
    Timer timer(interrupts);
    timer.write_div();
    timer.set_tma(0x23);
    timer.write_tima(0xff);
    timer.write_tac(0x05);
    timer.tick(16);
    timer.write_tima(0x42);
    timer.tick(4);

    check(timer.read_tima() == 0x42, "écriture TIMA n'a pas annulé le reload en attente");
    check((interrupts.flags & interrupt_mask(Interrupt::timer)) == 0,
          "IRQ timer demandée malgré l'annulation du reload TIMA");
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    div_and_tac_falling_edge_test();
    dmg_cgb_disable_difference_test();
    overflow_reload_window_test();
    tima_write_cancels_pending_reload_test();
    return 0;
}
