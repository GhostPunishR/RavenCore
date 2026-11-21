// Tests du PPU GB/GBC : Objets : scan OAM, fetcher, priorités et corruption de l'OAM (ppu/object_fetcher.hpp, ppu/oam_port.hpp).

#include "ppu_test_support.hpp"

namespace ravenemu::cgb::testing {

void sprite_penalty_test() {
    const auto measure = [](gbc::HardwareMode mode, std::initializer_list<int> raw_x,
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

    check(measure(gbc::HardwareMode::dmg, {8}) == 179,
          "premier OBJ au bord gauche ne recouvre pas le fetch BG initial");
    check(measure(gbc::HardwareMode::dmg, {0}) == 179,
          "premier OBJ OAM X=0 ne recouvre pas quatre dots du fetch BG initial");
    check(measure(gbc::HardwareMode::dmg, {0, 0, 0}) == 191,
          "OBJ OAM X=0 superposés ne coûtent pas sept puis six dots chacun");
    check(measure(gbc::HardwareMode::dmg, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == 233,
          "dix OBJ OAM X=0 n'appliquent pas le timing matériel cumulé");
    check(measure(gbc::HardwareMode::dmg, {8, 8}) == 185,
          "deux OBJ sur le même X ne coûtent pas sept puis six dots");
    check(measure(gbc::HardwareMode::dmg, {8}, false) == 172,
          "DMG fetch un OBJ alors que LCDC.1 est coupé avant sa rencontre");
    check(measure(gbc::HardwareMode::cgb_native, {8}, false) == 179,
          "CGB supprime à tort le timing OBJ lorsque LCDC.1 est coupé");
    check(measure(gbc::HardwareMode::cgb_compatibility, {8}, false) == 179,
          "compatibilité CGB supprime à tort le timing OBJ lorsque LCDC.1 est coupé");
}

void object_fifo_latches_fetched_data_test() {
    InterruptController interrupts;
    Ppu ppu(interrupts, gbc::HardwareMode::dmg);
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
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
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
    Ppu split_bytes(interrupts, gbc::HardwareMode::dmg);
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
    Ppu ppu(interrupts, gbc::HardwareMode::dmg);
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
    Ppu ppu(interrupts, gbc::HardwareMode::cgb_native);
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
        Ppu dmg(dmg_interrupts, gbc::HardwareMode::dmg);
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

    for (const auto mode : {gbc::HardwareMode::cgb_native, gbc::HardwareMode::cgb_compatibility}) {
        InterruptController cgb_interrupts;
        Ppu cgb(cgb_interrupts, mode);
        if (mode == gbc::HardwareMode::cgb_compatibility) {
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
        if (mode == gbc::HardwareMode::cgb_native) {
            check(cgb.completed_frame[32] == static_cast<std::int32_t>(0xffff0000U),
                  "fetch OBJ CGB poursuivi n'a pas rempli le FIFO après réactivation");
        } else {
            check(cgb.completed_frame[32] != cgb.completed_frame[31],
                  "fetch OBJ en compatibilité CGB perdu lors du masquage LCDC.1");
        }
    }
}

void progressive_oam_scan_test() {
    {
        InterruptController interrupts;
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
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
        Ppu ppu(interrupts, gbc::HardwareMode::dmg);
        ppu.write_oam_direct(0, 0);
        ppu.write_oam_direct(1, 8);
        ppu.write_lcdc(0x93);
        ppu.tick(2);                 // l'entrée 0 est déjà rejetée
        ppu.write_oam_direct(0, 16); // DMA simulé après son créneau
        check(measure_mode3_after_mode2(ppu, 78) == 172,
              "modification OAM tardive a rétroactivement changé le scan mode 2");
    }
}

std::int32_t overlapping_sprite_pixel(
    bool dmg_priority,
    bool first_transparent = false,
    gbc::HardwareMode mode = gbc::HardwareMode::cgb_native
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
    check(overlapping_sprite_pixel(false, false, gbc::HardwareMode::cgb_compatibility) ==
              static_cast<std::int32_t>(0xff00ff00U),
          "mode compatible CGB n'applique pas la priorité X programmée par la boot ROM");
}

namespace {

struct OamCorruptionFixture {
    explicit OamCorruptionFixture(gbc::HardwareMode hardware) : ppu(interrupts, hardware) {
        ppu.write_lcdc(0x91);
        // Ligne 2 : hors des lignes de démarrage du LCD, en plein mode 2.
        while (ppu.ly() != 2 || ppu.mode() != Ppu::mode_oam) ppu.tick(1);
        while (ppu.line_dot() != 0) ppu.tick(1);
        for (int index = 0; index < 0xa0; ++index) ppu.write_oam_direct(index, (index * 37 + 11) & 0xff);
    }
    void corrupt_at_row(int row, Ppu::OamCorruption kind) {
        while (ppu.line_dot() < row * 4) ppu.tick(1);
        ppu.corrupt_oam(kind);
    }
    InterruptController interrupts;
    Ppu ppu;
};

using OamImage = std::array<int, 0xa0>;

OamImage oam_image(const Ppu& ppu) {
    OamImage image{};
    for (int index = 0; index < 0xa0; ++index) image[static_cast<std::size_t>(index)] = ppu.oam[static_cast<std::size_t>(index)];
    return image;
}

// Formules de Pan Docs (« OAM Corruption Bug »), réécrites ici octet par octet
// sur une ligne de 8 octets : mots 0..3 = octets (0,1) (2,3) (4,5) (6,7).
int& byte_at(OamImage& image, int row, int offset) { return image[static_cast<std::size_t>(row * 8 + offset)]; }

void expect_simple_corruption(OamImage& image, int row, bool write) {
    for (int offset = 0; offset < 2; ++offset) {
        const int a = byte_at(image, row, offset);
        const int b = byte_at(image, row - 1, offset);
        const int c = byte_at(image, row - 1, offset + 4);
        byte_at(image, row, offset) = write ? ((a ^ c) & (b ^ c)) ^ c : (b | (a & c));
    }
    for (int offset = 2; offset < 8; ++offset) byte_at(image, row, offset) = byte_at(image, row - 1, offset);
}

} // namespace

/**
 * Bug de corruption de l'OAM du DMG : la ligne line_dot / 4 lue par le PPU en
 * mode 2 reçoit la corruption d'écriture, de lecture ou de lecture avec
 * incrément décrite par Pan Docs. La ligne 0, le mode 3 et le CGB y échappent.
 */
void oam_corruption_patterns_test() {
    {
        OamCorruptionFixture fixture(gbc::HardwareMode::dmg);
        auto expected = oam_image(fixture.ppu);
        fixture.corrupt_at_row(5, Ppu::OamCorruption::write);
        expect_simple_corruption(expected, 5, true);
        check(oam_image(fixture.ppu) == expected, "corruption d'écriture OAM différente de Pan Docs");
    }
    {
        OamCorruptionFixture fixture(gbc::HardwareMode::dmg);
        auto expected = oam_image(fixture.ppu);
        fixture.corrupt_at_row(7, Ppu::OamCorruption::read);
        expect_simple_corruption(expected, 7, false);
        check(oam_image(fixture.ppu) == expected, "corruption de lecture OAM différente de Pan Docs");
    }
    {
        OamCorruptionFixture fixture(gbc::HardwareMode::dmg);
        auto expected = oam_image(fixture.ppu);
        fixture.corrupt_at_row(9, Ppu::OamCorruption::read_with_increment);
        for (int offset = 0; offset < 2; ++offset) {
            const int a = byte_at(expected, 7, offset);
            const int b = byte_at(expected, 8, offset);
            const int c = byte_at(expected, 9, offset);
            const int d = byte_at(expected, 8, offset + 4);
            byte_at(expected, 8, offset) = (b & (a | c | d)) | (a & c & d);
        }
        for (int offset = 0; offset < 8; ++offset) {
            byte_at(expected, 9, offset) = byte_at(expected, 8, offset);
            byte_at(expected, 7, offset) = byte_at(expected, 8, offset);
        }
        expect_simple_corruption(expected, 9, false);
        check(oam_image(fixture.ppu) == expected, "corruption lecture + incrément OAM différente de Pan Docs");
    }
    {
        // Parmi les quatre premières lignes, seule la corruption de lecture reste.
        OamCorruptionFixture fixture(gbc::HardwareMode::dmg);
        auto expected = oam_image(fixture.ppu);
        fixture.corrupt_at_row(2, Ppu::OamCorruption::read_with_increment);
        expect_simple_corruption(expected, 2, false);
        check(oam_image(fixture.ppu) == expected, "lecture + incrément sur une des quatre premières lignes");
    }
    {
        OamCorruptionFixture fixture(gbc::HardwareMode::dmg);
        const auto expected = oam_image(fixture.ppu);
        fixture.corrupt_at_row(0, Ppu::OamCorruption::write);
        check(oam_image(fixture.ppu) == expected, "la ligne des objets 0 et 1 a été corrompue");
        while (fixture.ppu.mode() != Ppu::mode_transfer) fixture.ppu.tick(1);
        fixture.ppu.corrupt_oam(Ppu::OamCorruption::write);
        check(oam_image(fixture.ppu) == expected, "OAM corrompue hors du mode 2");
    }
    {
        OamCorruptionFixture fixture(gbc::HardwareMode::cgb_native);
        const auto expected = oam_image(fixture.ppu);
        fixture.corrupt_at_row(5, Ppu::OamCorruption::write);
        check(oam_image(fixture.ppu) == expected, "le CGB a reproduit le bug de corruption OAM du DMG");
    }
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    sprite_penalty_test();
    object_fifo_latches_fetched_data_test();
    object_fetch_read_phase_test();
    object_scan_ten_sprite_limit_test();
    cgb_object_bank_palette_and_flip_test();
    object_fetch_cancel_test();
    progressive_oam_scan_test();
    oam_corruption_patterns_test();
    opri_render_priority_test();
    return 0;
}
