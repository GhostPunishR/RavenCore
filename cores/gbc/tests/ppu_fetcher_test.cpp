// Tests du PPU GB/GBC : Fetcher du fond, window, mode 3 et rendu (ppu/background_fetcher.hpp, ppu/pixel_transfer.hpp, ppu/pixel_renderer.hpp).

#include "ppu_test_support.hpp"

namespace ravenemu::cgb::testing {

void fifo_baseline_and_scx_test() {
    for (int scx = 0; scx < 8; ++scx) {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
        ppu.write_lcdc(0x11);
        ppu.set_scx(scx);
        ppu.write_lcdc(0x91);
        const int length = enter_and_measure_mode3(ppu);
        check(length == 172 + scx, "durée mode 3 incorrecte pour la pénalité SCX fine");
    }
}

void window_restart_and_line_counter_test() {
    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
        ppu.write_lcdc(0x11);
        ppu.set_wy(0);
        ppu.set_wx(7);
        ppu.write_lcdc(0xb1); // LCD + BG + window
        check(enter_and_measure_mode3(ppu) == 178, "restart du fetcher window différent de six dots");
        check(ppu.window_line() == 1, "compteur interne window non incrémenté après affichage");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
        ppu.write_lcdc(0x11);
        ppu.set_wy(0);
        ppu.set_wx(167); // invisible
        ppu.write_lcdc(0xb1);
        check(enter_and_measure_mode3(ppu) == 172, "window invisible a allongé le mode 3");
        check(ppu.window_line() == 0, "compteur window incrémenté sans rendu");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
        ppu.write_lcdc(0x11);
        ppu.set_scx(3);
        ppu.set_wy(0);
        ppu.set_wx(0);
        ppu.write_lcdc(0xb1);
        const int wx_zero_length = enter_and_measure_mode3(ppu);
        check(wx_zero_length == 180,
              "cas WX=0/SCX fin n'applique pas le décalage et le raccourcissement documentés");
    }
}

void window_horizontal_scroll_independence_test() {
    for (const auto hardware : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_compatibility,
                               gbc::HardwareMode::cgb_native}) {
        for (const int wx : {1, 6, 7, 15, 87}) {
            for (const int scx : {0, 1, 2, 3, 4, 5, 6, 7, 8, 31, 255}) {
                InterruptController interrupts;
                Ppu ppu(interrupts, hardware);
                ppu.write_lcdc(0x11);
                ppu.set_bgp(0xe4);
                if (hardware == gbc::HardwareMode::cgb_compatibility) {
                    ppu.initialize_hle_compatibility_palettes();
                } else if (hardware == gbc::HardwareMode::cgb_native) {
                    ppu.write_bcps(0); ppu.write_bcpd(0);
                    ppu.write_bcps(1); ppu.write_bcpd(0);
                    ppu.write_bcps(2); ppu.write_bcpd(0x1f);
                    ppu.write_bcps(3); ppu.write_bcpd(0);
                }
                // BG uni, window avec une colonne colorée par tuile.
                for (int row = 0; row < 8; ++row) {
                    ppu.vram[static_cast<std::size_t>(16 + row * 2)] = 0x80;
                }
                for (int tile = 0; tile < 32 * 32; ++tile) {
                    ppu.vram[static_cast<std::size_t>(0x1c00 + tile)] = 1;
                }
                ppu.set_scx(scx);
                ppu.set_wy(112); // panneau en bas de l'écran
                ppu.set_wx(wx);
                ppu.write_lcdc(0xf1);
                while (ppu.mode() != Ppu::mode_vblank) ppu.tick(1);
                const auto background = ppu.completed_frame[0];
                for (int y = 112; y < Ppu::height; ++y) {
                    for (int x = 0; x < Ppu::width; ++x) {
                        const bool stripe = x >= wx - 7 && (x - (wx - 7)) % 8 == 0;
                        check((ppu.completed_frame[static_cast<std::size_t>(y * Ppu::width + x)] != background) == stripe,
                              "SCX déplace le panneau window ou altère son bord gauche");
                    }
                }
            }
        }
    }
}

void mid_scanline_wx_glitch_test() {
    for (const auto hardware_mode : {gbc::HardwareMode::dmg,
                                     gbc::HardwareMode::cgb_compatibility,
                                     gbc::HardwareMode::cgb_native}) {
        InterruptController interrupts;
        Ppu ppu(interrupts, hardware_mode);
        ppu.write_lcdc(0x11);
        ppu.set_bgp(0xe4);
        if (hardware_mode == gbc::HardwareMode::cgb_compatibility) {
            ppu.initialize_hle_compatibility_palettes();
        } else if (hardware_mode == gbc::HardwareMode::cgb_native) {
            // Palette BG 0 : couleur 0 noire, couleur 1 rouge.
            ppu.write_bcps(0); ppu.write_bcpd(0x00);
            ppu.write_bcps(1); ppu.write_bcpd(0x00);
            ppu.write_bcps(2); ppu.write_bcpd(0x1f);
            ppu.write_bcps(3); ppu.write_bcpd(0x00);
        }
        for (int row = 0; row < 8; ++row) {
            ppu.vram[static_cast<std::size_t>(row * 2)] = 0xff; // tuile 0, couleur 1
        }
        ppu.set_wy(0);
        ppu.set_wx(7);
        ppu.write_lcdc(0xb1);
        while (ppu.mode() != Ppu::mode_transfer || ppu.transfer_x() < 8) ppu.tick(1);
        ppu.set_wx(31); // nouvelle coïncidence à X=24, après le démarrage window

        const auto armed_state = snapshot(ppu);
        InterruptController restored_interrupts;
        Ppu restored(restored_interrupts, hardware_mode);
        detail::BinaryReader reader(armed_state);
        restored.load(reader);
        check(reader.exhausted(), "état PPU avec glitch WX armé laisse des données");

        while (ppu.mode() != Ppu::mode_vblank) ppu.tick(1);
        while (restored.mode() != Ppu::mode_vblank) restored.tick(1);
        check_snapshot_equal(snapshot(ppu), snapshot(restored),
                             "glitch WX divergent après restauration en mode 3");

        const auto regular_pixel = ppu.completed_frame[0];
        int injected_pixels{};
        for (int x = 0; x < Ppu::width; ++x) {
            if (ppu.completed_frame[static_cast<std::size_t>(x)] != regular_pixel) ++injected_pixels;
        }
        check(injected_pixels == 1,
              "changement WX après démarrage window n'a pas injecté exactement un pixel FIFO");
    }
}

void mid_scanline_scx_tile_fetch_test() {
    InterruptController reference_interrupts;
    InterruptController changed_interrupts;
    Ppu reference(reference_interrupts, gbc::HardwareMode::dmg);
    Ppu changed(changed_interrupts, gbc::HardwareMode::dmg);
    const auto prepare = [](Ppu& ppu) {
        ppu.write_lcdc(0x11);
        ppu.set_bgp(0xe4);
        for (int row = 0; row < 8; ++row) {
            ppu.vram[static_cast<std::size_t>(row * 2)] = 0xff;      // tuile 0, couleur 1
            ppu.vram[static_cast<std::size_t>(16 + row * 2 + 1)] = 0xff; // tuile 1, couleur 2
        }
        for (int tile = 0; tile < 32; ++tile) {
            ppu.vram[static_cast<std::size_t>(0x1800 + tile)] =
                static_cast<std::uint8_t>(tile & 1);
        }
        ppu.write_lcdc(0x91);
    };
    prepare(reference);
    prepare(changed);
    while (changed.mode() != Ppu::mode_transfer || changed.transfer_x() < 24) {
        reference.tick(1);
        changed.tick(1);
    }
    changed.set_scx(8); // conserve le décalage fin, décale les prochains Get Tile
    while (changed.mode() != Ppu::mode_vblank) {
        reference.tick(1);
        changed.tick(1);
    }

    for (int x = 0; x < 24; ++x) {
        check(reference.completed_frame[static_cast<std::size_t>(x)] ==
                  changed.completed_frame[static_cast<std::size_t>(x)],
              "écriture SCX mode 3 a modifié des pixels déjà sortis de la FIFO");
    }
    bool future_tile_changed{};
    for (int x = 24; x < Ppu::width; ++x) {
        future_tile_changed = future_tile_changed ||
            reference.completed_frame[static_cast<std::size_t>(x)] !=
            changed.completed_frame[static_cast<std::size_t>(x)];
    }
    check(future_tile_changed,
          "fetcher BG n'a pas relu les bits de tuile SCX après une écriture mode 3");
}

/**
 * Une écriture de palette en mode 3 atteint les pixels sortis pendant les
 * trois points qui précèdent la fin de son M-cycle : un pixel n'est coloré que
 * trois points après sa sortie de la FIFO (Mealybug Tearoom, m3_bgp_change).
 */
void render_register_write_phase_test() {
    for (const auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_compatibility}) {
        auto image = std::make_shared<const std::vector<std::uint8_t>>(ravenemu::testing::minimal_game_boy_rom());
        Machine machine(image, [] { return std::int64_t{0}; }, mode);
        machine.bus.write(0xff47, 0x00); // couleur 0 : blanc
        constexpr int line = 10;
        while (!(machine.ppu.ly() == line && machine.ppu.mode() == Ppu::mode_transfer &&
                 machine.ppu.transfer_x() >= 40)) {
            machine.bus.tick_mcycle();
        }
        const int x = machine.ppu.transfer_x();
        machine.bus.write_mcycle(0xff47, 0xff); // couleur 0 : noir
        check(machine.ppu.transfer_x() == x + 4, "le mode 3 ne sort pas un pixel par point");
        machine.bus.write(0xff47, 0x00);
        while (machine.ppu.ly() != Ppu::height) machine.bus.tick_mcycle();
        const auto pixel = [&](int column) {
            return machine.ppu.completed_frame[static_cast<std::size_t>(line * Ppu::width + column)];
        };
        check(pixel(x) == pixel(x - 1), "le pixel sorti avant l'écriture a changé de couleur");
        for (int column = x + 1; column < x + 4; ++column) {
            check(pixel(column) != pixel(x), "écriture de palette pas encore visible trois points avant la fin du M-cycle");
        }
    }
}

/**
 * Le fetcher ne pousse une tuile que dans une FIFO BG vide (Pan Docs, Pixel
 * FIFO) : il lit l'index de la tuile 7 (pixels 56 à 63) après la sortie du
 * pixel 50, et non pendant celle de la tuile 5. Mealybug Tearoom
 * (m3_lcdc_bg_map_change, m3_scy_change) le mesure.
 */
void fetcher_single_tile_lookahead_test() {
    for (const auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_native}) {
        const auto poke_tile_7 = [](Ppu& ppu) { ppu.vram[0x1800 + 32 + 7] = 1; };
        const auto untouched = fetcher_probe(mode, 8, 0, 7, [](Ppu&) {});
        const auto at_48 = fetcher_probe(mode, 8, 48, 7, poke_tile_7);
        const auto at_52 = fetcher_probe(mode, 8, 52, 7, poke_tile_7);
        check(at_48 != untouched, "index de la tuile 7 lu avant la sortie du pixel 48");
        check(at_52 == untouched, "index de la tuile 7 lu après la sortie du pixel 52");
    }
}

/**
 * Ligne de tuile et SCY (Mealybug Tearoom, m3_scy_change) : sur DMG, chaque
 * lecture de donnée recalcule la ligne avec le SCY du moment ; sur CGB, elle
 * reste celle de la lecture de l'index. Tuile 6 : index lu entre les pixels
 * 42 et 43, octet bas entre 44 et 45, octet haut entre 46 et 47.
 */
void fetcher_scy_tile_row_test() {
    const auto scy_1 = [](Ppu& ppu) { ppu.set_scy(1); };
    const auto dmg = [&](int x) { return fetcher_probe(gbc::HardwareMode::dmg, 8, x, 6, scy_1); };
    check(dmg(44) == 3, "DMG : octets de la tuile lus sans le SCY écrit après son index");
    check(dmg(46) == 2, "DMG : octet haut lu sans le SCY écrit après l'octet bas");
    check(dmg(48) == 0, "DMG : SCY écrit après la tuile a changé sa ligne");
    const auto cgb = [&](int x) { return fetcher_probe(gbc::HardwareMode::cgb_native, 8, x, 6, scy_1); };
    const auto cgb_row_0 = fetcher_probe(gbc::HardwareMode::cgb_native, 8, 0, 6, [](Ppu&) {});
    const auto cgb_row_1 = cgb(0);
    check(cgb_row_0 != cgb_row_1, "CGB : sonde de couleur sans contraste");
    check(cgb(42) == cgb_row_1 && cgb(44) == cgb_row_0, "CGB : ligne de tuile non fixée à la lecture de l'index");
}

/**
 * Écritures de SCY et de LCDC.6 par le CPU en mode 3 : comme les palettes,
 * elles sont validées dès le premier point du M-cycle (MemoryBus::
 * render_register), ce que Mealybug Tearoom mesure (m3_scy_change,
 * m3_lcdc_win_map_change). La tuile 0 a sa ligne 0 en couleur 0, les autres
 * en couleur 3 ; la tuile 1 est en couleur 3. Ligne 8, la sortie se fait à un
 * pixel par point : `start` est le pixel où commence le M-cycle d'écriture.
 */
void fetcher_register_write_phase_test() {
    const auto run = [](gbc::HardwareMode mode, int wx, int start, int address, int value) {
        auto image = std::make_shared<const std::vector<std::uint8_t>>(ravenemu::testing::minimal_game_boy_rom());
        Machine machine(image, [] { return std::int64_t{0}; }, mode);
        auto& vram = machine.ppu.vram;
        for (int offset = 0; offset < 32; ++offset) vram[static_cast<std::size_t>(offset)] = offset < 2 ? 0 : 0xff;
        std::fill(vram.begin() + 0x1800, vram.begin() + 0x1c00, std::uint8_t{0});
        std::fill(vram.begin() + 0x1c00, vram.begin() + 0x2000, std::uint8_t{1});
        machine.bus.write(0xff47, 0xe4);
        machine.bus.write(0xff4a, 0);
        machine.bus.write(0xff4b, wx);
        machine.bus.write(0xff40, wx <= 166 ? 0xb1 : 0x91);
        while (!(machine.ppu.ly() == 8 && machine.ppu.mode() == Ppu::mode_transfer &&
                 machine.ppu.transfer_x() >= start - 3)) {
            machine.bus.tick_mcycle();
        }
        check(machine.ppu.transfer_x() == start, "phase du M-cycle d'écriture inattendue");
        machine.bus.write_mcycle(address, value);
        while (machine.ppu.ly() != Ppu::height) machine.bus.tick_mcycle();
        std::array<std::int32_t, 3> tiles{};
        for (std::size_t index = 0; index < tiles.size(); ++index) {
            tiles[index] = machine.ppu.completed_frame[static_cast<std::size_t>(8 * Ppu::width + (4 + 2 * index) * 8 + 3)];
        }
        return tiles; // tuiles 4, 6 et 8
    };
    // DMG : octet bas de la tuile 6 lu avant la validation (point 45), octet
    // haut après : couleur 2. Validée en fin de M-cycle, l'écriture la manque.
    check(run(gbc::HardwareMode::dmg, 255, 44, 0xff42, 1)[1] == 2,
          "DMG : SCY écrit pas encore visible au premier point du M-cycle");
    // CGB : l'index de la tuile 6, qui fixe sa ligne, est lu après le point 41.
    const auto cgb = run(gbc::HardwareMode::cgb_compatibility, 255, 40, 0xff42, 1);
    check(cgb[0] != cgb[2] && cgb[1] == cgb[2], "CGB : SCY écrit pas encore visible au premier point du M-cycle");
    for (const auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_compatibility}) {
        const auto window = run(mode, 8, 42, 0xff40, 0xf1);
        check(window[0] != window[2] && window[1] == window[2],
              "carte de la window (LCDC.6) pas encore changée au premier point du M-cycle");
    }
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    fifo_baseline_and_scx_test();
    window_restart_and_line_counter_test();
    window_horizontal_scroll_independence_test();
    mid_scanline_wx_glitch_test();
    mid_scanline_scx_tile_fetch_test();
    render_register_write_phase_test();
    fetcher_single_tile_lookahead_test();
    fetcher_scy_tile_row_test();
    fetcher_register_write_phase_test();
    return 0;
}
