#include "ppu/ppu.hpp"
#include "check.hpp"
#include <ravenemu/binary_io.hpp>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

std::vector<std::uint8_t> snapshot(const Ppu& ppu);
void check_snapshot_equal(const std::vector<std::uint8_t>& expected,
                          const std::vector<std::uint8_t>& actual,
                          std::string_view message);

int enter_and_measure_mode3(Ppu& ppu) {
    while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
    const int start = ppu.line_dot();
    while (ppu.mode() == Ppu::mode_transfer) ppu.tick(1);
    return ppu.line_dot() - start;
}

void fifo_baseline_and_scx_test() {
    for (int scx = 0; scx < 8; ++scx) {
        InterruptController interrupts;
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
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
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
        ppu.write_lcdc(0x11);
        ppu.set_wy(0);
        ppu.set_wx(7);
        ppu.write_lcdc(0xb1); // LCD + BG + window
        check(enter_and_measure_mode3(ppu) == 178, "restart du fetcher window différent de six dots");
        check(ppu.window_line() == 1, "compteur interne window non incrémenté après affichage");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
        ppu.write_lcdc(0x11);
        ppu.set_wy(0);
        ppu.set_wx(167); // invisible
        ppu.write_lcdc(0xb1);
        check(enter_and_measure_mode3(ppu) == 172, "window invisible a allongé le mode 3");
        check(ppu.window_line() == 0, "compteur window incrémenté sans rendu");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
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
    for (const auto hardware : {gb::HardwareMode::dmg, gb::HardwareMode::cgb_compatibility,
                               gb::HardwareMode::cgb_native}) {
        for (const int wx : {1, 6, 7, 15, 87}) {
            for (const int scx : {0, 1, 2, 3, 4, 5, 6, 7, 8, 31, 255}) {
                InterruptController interrupts;
                Ppu ppu(interrupts, hardware);
                ppu.write_lcdc(0x11);
                ppu.set_bgp(0xe4);
                if (hardware == gb::HardwareMode::cgb_compatibility) {
                    ppu.initialize_hle_compatibility_palettes();
                } else if (hardware == gb::HardwareMode::cgb_native) {
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
    for (const auto hardware_mode : {gb::HardwareMode::dmg,
                                     gb::HardwareMode::cgb_compatibility,
                                     gb::HardwareMode::cgb_native}) {
        InterruptController interrupts;
        Ppu ppu(interrupts, hardware_mode);
        ppu.write_lcdc(0x11);
        ppu.set_bgp(0xe4);
        if (hardware_mode == gb::HardwareMode::cgb_compatibility) {
            ppu.initialize_hle_compatibility_palettes();
        } else if (hardware_mode == gb::HardwareMode::cgb_native) {
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
    Ppu reference(reference_interrupts, gb::HardwareMode::dmg);
    Ppu changed(changed_interrupts, gb::HardwareMode::dmg);
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

void sprite_penalty_test() {
    const auto measure = [](gb::HardwareMode mode, std::initializer_list<int> raw_x,
                            bool objects_enabled = true) {
        InterruptController interrupts;
        Ppu ppu(interrupts, mode);
        int sprite{};
        for (const int x : raw_x) {
            ppu.write_oam_direct(sprite * 4, 16);
            ppu.write_oam_direct(sprite * 4 + 1, x);
            ++sprite;
        }
        ppu.write_lcdc(objects_enabled ? 0x93 : 0x91);
        return enter_and_measure_mode3(ppu);
    };

    check(measure(gb::HardwareMode::dmg, {8}) == 179,
          "premier OBJ au bord gauche ne recouvre pas le fetch BG initial");
    check(measure(gb::HardwareMode::dmg, {0}) == 179,
          "premier OBJ OAM X=0 ne recouvre pas quatre dots du fetch BG initial");
    check(measure(gb::HardwareMode::dmg, {0, 0, 0}) == 191,
          "OBJ OAM X=0 superposés ne coûtent pas sept puis six dots chacun");
    check(measure(gb::HardwareMode::dmg, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == 233,
          "dix OBJ OAM X=0 n'appliquent pas le timing matériel cumulé");
    check(measure(gb::HardwareMode::dmg, {8, 8}) == 185,
          "deux OBJ sur le même X ne coûtent pas sept puis six dots");
    check(measure(gb::HardwareMode::dmg, {8}, false) == 172,
          "DMG fetch un OBJ alors que LCDC.1 est coupé avant sa rencontre");
    check(measure(gb::HardwareMode::cgb_native, {8}, false) == 179,
          "CGB supprime à tort le timing OBJ lorsque LCDC.1 est coupé");
    check(measure(gb::HardwareMode::cgb_compatibility, {8}, false) == 179,
          "compatibilité CGB supprime à tort le timing OBJ lorsque LCDC.1 est coupé");
}

void object_fifo_latches_fetched_data_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gb::HardwareMode::dmg);
    ppu.write_lcdc(0x11);
    ppu.set_bgp(0xe4);
    ppu.set_obp0(0xe4);
    ppu.write_oam_direct(0, 17); // première ligne normale après l'activation LCD
    ppu.write_oam_direct(1, 8);
    ppu.write_oam_direct(2, 1);
    ppu.write_oam_direct(3, 0);
    ppu.vram[16] = 0xff;      // tuile 1, couleur 1
    ppu.vram[32 + 1] = 0xff;  // tuile 2, couleur 2
    ppu.write_lcdc(0x93);

    while (ppu.ly() != 1 || ppu.mode() != Ppu::mode_transfer || ppu.transfer_x() < 1) ppu.tick(1);
    // Le premier pixel prouve que le fetch est terminé. Les sept pixels
    // restants doivent venir du FIFO, sans relire OAM ni VRAM.
    ppu.write_oam_direct(2, 2);
    ppu.vram[16] = 0x00;
    ppu.vram[16 + 1] = 0xff;
    while (ppu.mode() != Ppu::mode_vblank) ppu.tick(1);

    for (int x = 0; x < 8; ++x) {
        check(ppu.completed_frame[static_cast<std::size_t>(Ppu::width + x)] == 1,
              "FIFO OBJ a relu OAM/VRAM après la fin du fetch de tuile");
    }
}

void object_fetch_read_phase_test() {
    const auto prepare = [](Ppu& ppu) {
        ppu.set_bgp(0xe4);
        ppu.set_obp0(0xe4);
        ppu.write_oam_direct(0, 16);
        ppu.write_oam_direct(1, 40);
        ppu.write_oam_direct(2, 1);
        ppu.write_oam_direct(3, 0);
        ppu.vram[16] = 0xff;      // tuile 1, couleur 1
        ppu.vram[32 + 1] = 0xff;  // tuile 2, couleur 2
        ppu.write_lcdc(0x93);
        while (ppu.mode() != Ppu::mode_transfer || ppu.transfer_x() < 32) ppu.tick(1);
    };
    const auto pixel_after_oam_change = [&](int change_after_dots) {
        InterruptController interrupts;
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
        prepare(ppu);
        ppu.tick(change_after_dots);
        ppu.write_oam_direct(2, 2);
        while (ppu.mode() != Ppu::mode_vblank) ppu.tick(1);
        return ppu.completed_frame[32];
    };

    check(pixel_after_oam_change(2) == 2,
          "fetch OBJ n'a pas observé Tile ID écrit avant la phase OAM");
    check(pixel_after_oam_change(3) == 1,
          "fetch OBJ a relu Tile ID après la phase OAM");

    InterruptController interrupts;
    Ppu split_bytes(interrupts, gb::HardwareMode::dmg);
    prepare(split_bytes);
    split_bytes.tick(5); // l'octet bas est lu, l'octet haut ne l'est pas encore
    split_bytes.vram[16] = 0x00;
    split_bytes.vram[16 + 1] = 0xff;
    while (split_bytes.mode() != Ppu::mode_vblank) split_bytes.tick(1);
    check(split_bytes.completed_frame[32] == 3,
          "octets bas/haut OBJ non échantillonnés lors de leurs phases distinctes");
}

void object_scan_ten_sprite_limit_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gb::HardwareMode::dmg);
    ppu.set_bgp(0xe4);
    ppu.set_obp0(0xe4);
    for (int sprite = 0; sprite < 11; ++sprite) {
        ppu.write_oam_direct(sprite * 4, 16);
        ppu.write_oam_direct(sprite * 4 + 1, 8);
        ppu.write_oam_direct(sprite * 4 + 2, sprite == 10 ? 1 : 0);
        ppu.write_oam_direct(sprite * 4 + 3, 0);
    }
    ppu.vram[16] = 0xff;
    ppu.write_lcdc(0x93);
    check(enter_and_measure_mode3(ppu) == 233,
          "scan OAM n'a pas limité le fetch aux dix premiers OBJ de la ligne");
    while (ppu.mode() != Ppu::mode_vblank) ppu.tick(1);
    check(ppu.completed_frame[0] == 0,
          "onzième OBJ de la ligne rendu malgré la limite matérielle de dix");
}

void cgb_object_bank_palette_and_flip_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gb::HardwareMode::cgb_native);
    ppu.write_lcdc(0x11);
    // Palette OBJ 3, couleur 1 : vert pur.
    ppu.write_ocps((3 * 4 + 1) * 2); ppu.write_ocpd(0xe0);
    ppu.write_ocps((3 * 4 + 1) * 2 + 1); ppu.write_ocpd(0x03);
    ppu.write_oam_direct(0, 17);
    ppu.write_oam_direct(1, 8);
    ppu.write_oam_direct(2, 1);
    ppu.write_oam_direct(3, 0x20 | 0x08 | 3); // flip X, banque 1, palette 3
    ppu.vram[0x2000 + 16] = 0x01;             // bit 0 devient le pixel gauche
    ppu.write_lcdc(0x93);
    while (ppu.mode() != Ppu::mode_vblank) ppu.tick(1);

    check(ppu.completed_frame[Ppu::width] == static_cast<std::int32_t>(0xff00ff00U),
          "fetch OBJ CGB n'applique pas banque VRAM, palette et flip horizontal");
    check(ppu.completed_frame[Ppu::width + 1] != static_cast<std::int32_t>(0xff00ff00U),
          "flip horizontal CGB a produit plus d'un pixel opaque");
}

void object_fetch_cancel_test() {
    const auto prepare = [](Ppu& ppu, int fetch_dots) {
        ppu.set_bgp(0xe4);
        ppu.set_obp0(0xe4);
        ppu.write_oam_direct(0, 16);
        ppu.write_oam_direct(1, 40); // pixel écran 32
        ppu.write_oam_direct(2, 1);
        ppu.write_oam_direct(3, 0);
        ppu.vram[16] = 0xff;
        ppu.write_lcdc(0x93);
        while (ppu.mode() != Ppu::mode_transfer || ppu.transfer_x() < 32) ppu.tick(1);
        ppu.tick(fetch_dots);
    };

    int latest_dmg_mode3_end{};
    for (int cancel_after = 1; cancel_after <= 6; ++cancel_after) {
        InterruptController dmg_interrupts;
        Ppu dmg(dmg_interrupts, gb::HardwareMode::dmg);
        prepare(dmg, cancel_after);
        dmg.write_lcdc(0x91);
        dmg.tick(1); // le prochain dot annule le fetch DMG
        dmg.write_lcdc(0x93);
        while (dmg.mode() == Ppu::mode_transfer) dmg.tick(1);
        latest_dmg_mode3_end = std::max(latest_dmg_mode3_end, dmg.line_dot());
        while (dmg.mode() != Ppu::mode_vblank) dmg.tick(1);
        for (int x = 33; x < 40; ++x) {
            check(dmg.completed_frame[static_cast<std::size_t>(x)] == 0,
                  "annulation DMG tardive a tout de même rempli le FIFO OBJ");
        }
    }

    for (const auto mode : {gb::HardwareMode::cgb_native, gb::HardwareMode::cgb_compatibility}) {
        InterruptController cgb_interrupts;
        Ppu cgb(cgb_interrupts, mode);
        if (mode == gb::HardwareMode::cgb_compatibility) {
            cgb.initialize_hle_compatibility_palettes();
        } else {
            cgb.write_ocps(2); cgb.write_ocpd(0x1f);
            cgb.write_ocps(3); cgb.write_ocpd(0x00);
        }
        prepare(cgb, 2);
        cgb.write_lcdc(0x91);
        cgb.tick(1);
        cgb.write_lcdc(0x93);
        while (cgb.mode() == Ppu::mode_transfer) cgb.tick(1);
        check(cgb.line_dot() > latest_dmg_mode3_end,
              "matériel CGB a annulé le fetch OBJ comme un DMG");
        while (cgb.mode() != Ppu::mode_vblank) cgb.tick(1);
        if (mode == gb::HardwareMode::cgb_native) {
            check(cgb.completed_frame[32] == static_cast<std::int32_t>(0xffff0000U),
                  "fetch OBJ CGB poursuivi n'a pas rempli le FIFO après réactivation");
        } else {
            check(cgb.completed_frame[32] != cgb.completed_frame[31],
                  "fetch OBJ en compatibilité CGB perdu lors du masquage LCDC.1");
        }
    }
}

int measure_mode3_after_mode2(Ppu& ppu, int remaining_mode2_dots) {
    ppu.tick(remaining_mode2_dots);
    while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
    check(ppu.mode() == Ppu::mode_transfer, "PPU absent du mode 3 après le scan OAM");
    const int start = ppu.line_dot();
    while (ppu.mode() == Ppu::mode_transfer) ppu.tick(1);
    return ppu.line_dot() - start;
}

void progressive_oam_scan_test() {
    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
        ppu.write_oam_direct(0, 0);  // hors ligne avant son créneau de scan
        ppu.write_oam_direct(1, 8);
        ppu.write_lcdc(0x93);
        ppu.tick(1);
        ppu.write_oam_direct(0, 16); // DMA simulé avant le deuxième dot
        check(measure_mode3_after_mode2(ppu, 79) == 179,
              "modification OAM avant le créneau mode 2 non observée");
    }

    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
        ppu.write_oam_direct(0, 0);
        ppu.write_oam_direct(1, 8);
        ppu.write_lcdc(0x93);
        ppu.tick(2);                 // l'entrée 0 est déjà rejetée
        ppu.write_oam_direct(0, 16); // DMA simulé après son créneau
        check(measure_mode3_after_mode2(ppu, 78) == 172,
              "modification OAM tardive a rétroactivement changé le scan mode 2");
    }
}

void mid_oam_scan_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gb::HardwareMode::dmg);
    source.write_oam_direct(0, 16);
    source.write_oam_direct(1, 8);
    source.write_oam_direct(20 * 4, 16);
    source.write_oam_direct(20 * 4 + 1, 16);
    source.write_lcdc(0x93);
    source.tick(21); // dix entrées inspectées, entrée 20 encore à venir

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
    detail::BinaryReader reader(saved);
    restored.load(reader);
    check(reader.exhausted(), "état PPU mode 2 laisse des données non consommées");

    source.tick(59);
    restored.tick(59);
    check_snapshot_equal(snapshot(source), snapshot(restored),
                         "restauration en milieu de scan OAM non déterministe");
}

void stat_line_edge_test() {
    InterruptController interrupts;
    interrupts.flags = 0;
    Ppu ppu(interrupts, gb::HardwareMode::cgb_native);
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
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
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
        Ppu ppu(interrupts, gb::HardwareMode::cgb_compatibility);
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
        Ppu ppu(interrupts, gb::HardwareMode::dmg);
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
        Ppu ppu(interrupts, gb::HardwareMode::cgb_native);
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

void video_bus_lock_boundary_test() {
    for (const auto hardware_mode : {gb::HardwareMode::dmg,
                                     gb::HardwareMode::cgb_compatibility,
                                     gb::HardwareMode::cgb_native}) {
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
    Ppu ppu(interrupts, gb::HardwareMode::dmg);
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
    for (const auto hardware_mode : {gb::HardwareMode::cgb_compatibility,
                                     gb::HardwareMode::cgb_native}) {
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
    Ppu ppu(interrupts, gb::HardwareMode::cgb_native);
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
    Ppu ppu(interrupts, gb::HardwareMode::cgb_native);
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

void stat_source_blocking_test() {
    for (const auto hardware_mode : {gb::HardwareMode::dmg,
                                     gb::HardwareMode::cgb_compatibility,
                                     gb::HardwareMode::cgb_native}) {
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
    Ppu ppu(interrupts, gb::HardwareMode::dmg);
    ppu.write_lyc(0);
    ppu.write_stat(0x48); // LYC et mode 0
    interrupts.flags = 0;
    while (ppu.mode() != Ppu::mode_hblank) ppu.tick(1);
    check((interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
          "source mode 0 non bloquée par une coïncidence LYC maintenue");
}

void startup_bus_gate_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gb::HardwareMode::dmg);
    source.write_lcdc(0x11);
    source.write_vram(0, 0x71);
    source.write_lcdc(0x91);
    source.tick(76);
    check(source.mode() == Ppu::mode_hblank && source.read_vram(0) == 0x71,
          "point de sauvegarde absent du front bus LCD initial");

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
    detail::BinaryReader reader(saved);
    restored.load(reader);
    check(reader.exhausted() && restored.read_vram(0) == 0x71,
          "porte vidéo DMG mal restaurée au milieu du retard STAT");
    source.tick(4);
    restored.tick(4);
    check(source.read_vram(0) == 0xff && restored.read_vram(0) == 0xff,
          "porte vidéo restaurée diverge au front mode 3");
    check_snapshot_equal(snapshot(source), snapshot(restored),
                         "timing du verrou vidéo divergent après restauration");
}

void lcd_off_lyc_latch_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gb::HardwareMode::dmg);
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
    Ppu ppu(interrupts, gb::HardwareMode::dmg);
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
    Ppu dmg(dmg_interrupts, gb::HardwareMode::dmg);
    dmg.write_lcdc(0x11);
    dmg.write_lcdc(0x91); // mode 2, aucune source STAT activée
    dmg_interrupts.flags = 0;
    dmg.write_stat(0x00);
    check((dmg_interrupts.flags & interrupt_mask(Interrupt::stat)) != 0,
          "glitch IRQ d'écriture STAT absent sur DMG en mode 2");

    InterruptController cgb_interrupts;
    Ppu cgb(cgb_interrupts, gb::HardwareMode::cgb_native);
    cgb.write_lcdc(0x11);
    cgb.write_lcdc(0x91);
    cgb_interrupts.flags = 0;
    cgb.write_stat(0x00);
    check((cgb_interrupts.flags & interrupt_mask(Interrupt::stat)) == 0,
          "glitch d'écriture STAT DMG appliqué à tort au CGB");
}

std::int32_t overlapping_sprite_pixel(
    bool dmg_priority,
    bool first_transparent = false,
    gb::HardwareMode mode = gb::HardwareMode::cgb_native
) {
    InterruptController interrupts;
    Ppu ppu(interrupts, mode);
    ppu.write_lcdc(0x11);

    // OBJ palette 0: couleur 1 rouge, couleur 2 verte.
    ppu.write_ocps(2); ppu.write_ocpd(0x1f);
    ppu.write_ocps(3); ppu.write_ocpd(0x00);
    ppu.write_ocps(4); ppu.write_ocpd(0xe0);
    ppu.write_ocps(5); ppu.write_ocpd(0x03);
    ppu.set_obp0(0xe4); // mapping DMG identité pour le mode compatible

    // OAM 0 commence en X=2 et gagne en mode CGB; OAM 1 commence en X=0
    // et gagne par coordonnée X en mode DMG. Ils se recouvrent au pixel 2.
    // La première ligne après LCDC.7 saute le scan OAM : placer les objets
    // sur LY=1 isole ici la priorité OPRI du timing d'activation LCD.
    ppu.write_oam_direct(0, 17); ppu.write_oam_direct(1, 10);
    ppu.write_oam_direct(2, 0); ppu.write_oam_direct(3, 0);
    ppu.write_oam_direct(4, 17); ppu.write_oam_direct(5, 8);
    ppu.write_oam_direct(6, 1); ppu.write_oam_direct(7, 0);
    if (!first_transparent) ppu.vram[0] = 0x80; // tile 0, pixel 0 = couleur 1
    ppu.vram[16 + 1] = 0x20;                   // tile 1, pixel 2 = couleur 2

    ppu.write_opri(dmg_priority ? 1 : 0);
    ppu.write_lcdc(0x93);
    ppu.tick(456 * 144);
    return ppu.completed_frame[Ppu::width + 2];
}

void opri_render_priority_test() {
    check(overlapping_sprite_pixel(false) == static_cast<std::int32_t>(0xffff0000U),
          "OPRI=0 n'accorde pas la priorité à l'index OAM le plus bas");
    check(overlapping_sprite_pixel(true) == static_cast<std::int32_t>(0xff00ff00U),
          "OPRI=1 n'accorde pas la priorité à la plus petite coordonnée X");
    check(overlapping_sprite_pixel(false, true) == static_cast<std::int32_t>(0xff00ff00U),
          "OBJ transparent prioritaire masque à tort un OBJ opaque inférieur");
    check(overlapping_sprite_pixel(false, false, gb::HardwareMode::cgb_compatibility) ==
              static_cast<std::int32_t>(0xff00ff00U),
          "mode compatible CGB n'applique pas la priorité X programmée par la boot ROM");
}

std::vector<std::uint8_t> snapshot(const Ppu& ppu) {
    detail::BinaryWriter writer;
    ppu.save(writer);
    return std::move(writer).take();
}

void check_snapshot_equal(const std::vector<std::uint8_t>& expected,
                          const std::vector<std::uint8_t>& actual,
                          std::string_view message) {
    check(expected.size() == actual.size(), "taille d'état PPU différente après restauration");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] != actual[index]) {
            throw std::runtime_error(std::string{message} + " (octet " + std::to_string(index) + ")");
        }
    }
}

void mid_transfer_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gb::HardwareMode::cgb_native);
    source.write_lcdc(0x11);
    source.set_scx(5);
    source.set_wy(0);
    source.set_wx(23);
    source.write_oam_direct(0, 16);
    source.write_oam_direct(1, 32);
    source.write_lcdc(0xb3);
    source.tick(111);
    check(source.mode() == Ppu::mode_transfer,
          "point de sauvegarde PPU absent du milieu du mode 3");

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gb::HardwareMode::cgb_native);
    detail::BinaryReader reader(saved);
    restored.load(reader);
    check(reader.exhausted(), "état PPU laisse des données non consommées");
    check_snapshot_equal(saved, snapshot(restored),
                         "état PPU différent immédiatement après restauration");

    source.tick(1000);
    restored.tick(1000);
    check_snapshot_equal(snapshot(source), snapshot(restored),
                         "restauration en milieu de fetch/FIFO PPU non déterministe");
}

void mid_object_fetch_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gb::HardwareMode::cgb_native);
    source.write_oam_direct(0, 16);
    source.write_oam_direct(1, 40);
    source.write_oam_direct(2, 1);
    source.write_oam_direct(3, 3);
    source.vram[16] = 0xaa;
    source.vram[16 + 1] = 0x55;
    source.write_lcdc(0x93);
    while (source.mode() != Ppu::mode_transfer || source.transfer_x() < 32) source.tick(1);
    source.tick(3);
    check(source.transfer_x() == 32 && source.mode() == Ppu::mode_transfer,
          "point de sauvegarde absent du milieu du fetch OBJ");

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gb::HardwareMode::cgb_native);
    detail::BinaryReader reader(saved);
    restored.load(reader);
    check(reader.exhausted(), "état PPU en fetch OBJ laisse des données non consommées");
    check_snapshot_equal(saved, snapshot(restored),
                         "fetch OBJ différent immédiatement après restauration");

    source.tick(1000);
    restored.tick(1000);
    check_snapshot_equal(snapshot(source), snapshot(restored),
                         "restauration en plein fetch OBJ non déterministe");
}

void overlapping_object_fetch_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gb::HardwareMode::dmg);
    source.write_oam_direct(0, 16);
    source.write_oam_direct(1, 69); // X écran 61, trois dots de recouvrement
    source.write_oam_direct(2, 1);
    source.vram[16] = 0xff;
    source.write_lcdc(0x93);
    while (source.mode() != Ppu::mode_transfer || source.transfer_x() < 58) source.tick(1);
    source.tick(1);
    check(source.transfer_x() == 59 && source.mode() == Ppu::mode_transfer,
          "fetch OBJ n'a pas avancé la sortie BG pendant son premier dot recouvert");

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
    detail::BinaryReader reader(saved);
    restored.load(reader);
    check(reader.exhausted(), "état PPU au milieu du recouvrement OBJ laisse des données");
    check_snapshot_equal(saved, snapshot(restored),
                         "recouvrement OBJ/BG différent immédiatement après restauration");

    source.tick(1000);
    restored.tick(1000);
    check_snapshot_equal(snapshot(source), snapshot(restored),
                         "recouvrement OBJ/BG divergent après restauration");
}

void mid_object_fifo_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gb::HardwareMode::dmg);
    source.write_lcdc(0x11);
    source.set_bgp(0xe4);
    source.set_obp0(0xe4);
    source.write_oam_direct(0, 17);
    source.write_oam_direct(1, 8);
    source.write_oam_direct(2, 1);
    source.vram[16] = 0xff;
    source.write_lcdc(0x93);
    while (source.ly() != 1 || source.mode() != Ppu::mode_transfer || source.transfer_x() < 1) {
        source.tick(1);
    }

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
    detail::BinaryReader reader(saved);
    restored.load(reader);
    check(reader.exhausted(), "état PPU avec FIFO OBJ rempli laisse des données");
    source.tick(1000);
    restored.tick(1000);
    check_snapshot_equal(snapshot(source), snapshot(restored),
                         "FIFO OBJ partiellement consommé non restauré fidèlement");
}

void object_fifo_state_validation_test() {
    InterruptController interrupts;
    Ppu source(interrupts, gb::HardwareMode::dmg);
    auto corrupted = snapshot(source);
    constexpr std::size_t object_fifo_offset = 4 + 72 * 4 + 16 * 3;
    check(corrupted.size() > object_fifo_offset, "état PPU trop court pour contenir le FIFO OBJ");
    corrupted[object_fifo_offset] = 4; // couleur OBJ hors de l'intervalle matériel 0..3
    expect_failure<SaveStateError>(
        [&] {
            InterruptController restored_interrupts;
            Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
            detail::BinaryReader reader(corrupted);
            restored.load(reader);
        },
        "couleur invalide du FIFO OBJ acceptée par le chargeur d'état");

    auto invalid_overlap = snapshot(source);
    constexpr std::size_t overlap_offset = 4 + 56 * 4;
    check(invalid_overlap.size() > overlap_offset + 3,
          "état PPU trop court pour la phase de recouvrement OBJ");
    invalid_overlap[overlap_offset] = 4;
    expect_failure<SaveStateError>(
        [&] {
            InterruptController restored_interrupts;
            Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
            detail::BinaryReader reader(invalid_overlap);
            restored.load(reader);
        },
        "phase de recouvrement OBJ invalide acceptée par le chargeur d'état");
}

void ppu_timing_state_validation_test() {
    InterruptController interrupts;
    Ppu source(interrupts, gb::HardwareMode::dmg);

    auto invalid_mode = snapshot(source);
    constexpr std::size_t mode_offset = 4 + 11 * 4;
    check(invalid_mode.size() > mode_offset + 3, "état PPU trop court pour son mode interne");
    invalid_mode[mode_offset] = 4;
    invalid_mode[mode_offset + 1] = 0;
    invalid_mode[mode_offset + 2] = 0;
    invalid_mode[mode_offset + 3] = 0;
    expect_failure<SaveStateError>(
        [&] {
            InterruptController restored_interrupts;
            Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
            detail::BinaryReader reader(invalid_mode);
            restored.load(reader);
        },
        "mode PPU invalide accepté par le chargeur d'état");

    auto invalid_delay = snapshot(source);
    constexpr std::size_t reported_delay_offset = 4 + 14 * 4;
    check(invalid_delay.size() > reported_delay_offset + 3,
          "état PPU trop court pour son retard de mode publié");
    invalid_delay[reported_delay_offset] = 5;
    invalid_delay[reported_delay_offset + 1] = 0;
    invalid_delay[reported_delay_offset + 2] = 0;
    invalid_delay[reported_delay_offset + 3] = 0;
    expect_failure<SaveStateError>(
        [&] {
            InterruptController restored_interrupts;
            Ppu restored(restored_interrupts, gb::HardwareMode::dmg);
            detail::BinaryReader reader(invalid_delay);
            restored.load(reader);
        },
        "retard de publication PPU invalide accepté au lieu d'être refusé");
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    fifo_baseline_and_scx_test();
    window_restart_and_line_counter_test();
    window_horizontal_scroll_independence_test();
    mid_scanline_wx_glitch_test();
    mid_scanline_scx_tile_fetch_test();
    sprite_penalty_test();
    object_fifo_latches_fetched_data_test();
    object_fetch_read_phase_test();
    object_scan_ten_sprite_limit_test();
    cgb_object_bank_palette_and_flip_test();
    object_fetch_cancel_test();
    progressive_oam_scan_test();
    mid_oam_scan_save_state_test();
    stat_line_edge_test();
    mode2_stat_lead_test();
    video_bus_lock_boundary_test();
    dmg_lcd_startup_bus_gate_test();
    cgb_lcd_startup_bus_gate_test();
    cgb_cram_lock_and_autoincrement_test();
    lcd_disable_releases_video_bus_test();
    stat_source_blocking_test();
    startup_bus_gate_save_state_test();
    lcd_off_lyc_latch_test();
    ly_153_quirk_test();
    dmg_stat_write_glitch_test();
    opri_render_priority_test();
    mid_transfer_save_state_test();
    mid_object_fetch_save_state_test();
    overlapping_object_fetch_save_state_test();
    mid_object_fifo_save_state_test();
    object_fifo_state_validation_test();
    ppu_timing_state_validation_test();
    return 0;
}
