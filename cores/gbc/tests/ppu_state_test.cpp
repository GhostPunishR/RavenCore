// Tests du PPU GB/GBC : États instantanés du PPU (ppu/ppu_state.hpp, ppu/frame_codec.hpp).

#include "ppu_test_support.hpp"

namespace ravenemu::cgb::testing {

void mid_oam_scan_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gbc::HardwareMode::dmg);
    source.write_oam_direct(0, 16);
    source.write_oam_direct(1, 8);
    source.write_oam_direct(20 * 4, 16);
    source.write_oam_direct(20 * 4 + 1, 16);
    source.write_lcdc(0x93);
    source.tick(21); // dix entrées inspectées, entrée 20 encore à venir

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
    detail::BinaryReader reader(saved);
    restored.load(reader);
    check(reader.exhausted(), "état PPU mode 2 laisse des données non consommées");

    source.tick(59);
    restored.tick(59);
    check_snapshot_equal(snapshot(source), snapshot(restored),
                         "restauration en milieu de scan OAM non déterministe");
}

void startup_bus_gate_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gbc::HardwareMode::dmg);
    source.write_lcdc(0x11);
    source.write_vram(0, 0x71);
    source.write_lcdc(0x91);
    source.tick(76);
    check(source.mode() == Ppu::mode_hblank && source.read_vram(0) == 0x71,
          "point de sauvegarde absent du front bus LCD initial");

    const auto saved = snapshot(source);
    InterruptController restored_interrupts;
    Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
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

void mid_transfer_save_state_test() {
    InterruptController source_interrupts;
    Ppu source(source_interrupts, gbc::HardwareMode::cgb_native);
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
    Ppu restored(restored_interrupts, gbc::HardwareMode::cgb_native);
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
    Ppu source(source_interrupts, gbc::HardwareMode::cgb_native);
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
    Ppu restored(restored_interrupts, gbc::HardwareMode::cgb_native);
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
    Ppu source(source_interrupts, gbc::HardwareMode::dmg);
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
    Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
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
    Ppu source(source_interrupts, gbc::HardwareMode::dmg);
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
    Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
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
    Ppu source(interrupts, gbc::HardwareMode::dmg);
    auto corrupted = snapshot(source);
    constexpr std::size_t object_fifo_offset = 4 + 72 * 4 + 16 * 3;
    check(corrupted.size() > object_fifo_offset, "état PPU trop court pour contenir le FIFO OBJ");
    corrupted[object_fifo_offset] = 4; // couleur OBJ hors de l'intervalle matériel 0..3
    expect_failure<SaveStateError>(
        [&] {
            InterruptController restored_interrupts;
            Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
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
            Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
            detail::BinaryReader reader(invalid_overlap);
            restored.load(reader);
        },
        "phase de recouvrement OBJ invalide acceptée par le chargeur d'état");
}

void ppu_timing_state_validation_test() {
    InterruptController interrupts;
    Ppu source(interrupts, gbc::HardwareMode::dmg);

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
            Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
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
            Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
            detail::BinaryReader reader(invalid_delay);
            restored.load(reader);
        },
        "retard de publication PPU invalide accepté au lieu d'être refusé");
}

// Le mode 3 passe en H-Blank dès son 160e pixel : un état modifié qui le
// garderait au bout de la ligne écrirait au-delà de la ligne et de l'image.
void transfer_at_the_line_end_is_rejected() {
    InterruptController interrupts;
    Ppu source(interrupts, gbc::HardwareMode::cgb_native);
    source.write_lcdc(0x11);
    source.write_lcdc(0x93);  // sans fenêtre : aucun glitch WX armé
    source.tick(111);
    check(source.mode() == Ppu::mode_transfer, "PPU hors du mode 3");
    const auto load = [](const std::vector<std::uint8_t>& bytes) {
        InterruptController restored_interrupts;
        Ppu restored(restored_interrupts, gbc::HardwareMode::cgb_native);
        detail::BinaryReader reader(bytes);
        restored.load(reader);
    };
    auto state = snapshot(source);
    load(state);
    constexpr std::size_t transfer_x_offset = 4 + 29 * 4;
    check(state.size() > transfer_x_offset + 3, "état PPU trop court pour la position du mode 3");
    // Entier 32 bits gros-boutiste : 160.
    state[transfer_x_offset] = 0;
    state[transfer_x_offset + 1] = 0;
    state[transfer_x_offset + 2] = 0;
    state[transfer_x_offset + 3] = 160;
    expect_failure<SaveStateError>([&] { load(state); }, "mode 3 au bout de la ligne accepté");
}

void corrupt_background_fifo_is_rejected() {
    constexpr std::size_t fifo_offset = 4 + 72 * 4; // PPU layout 72.
    for (auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_native}) {
        auto rom = ravenemu::testing::minimal_game_boy_rom();
        if (mode == gbc::HardwareMode::cgb_native) rom[0x143] = 0x80;
        auto core = gbc::make_core(); core->load_rom(rom, {});
        auto image = std::make_shared<const std::vector<std::uint8_t>>(rom);
        Machine reference(image, [] { return std::int64_t{0}; }, mode);
        const auto initial_ppu = snapshot(reference.ppu);
        auto state = core->save_state();
        const auto found = std::search(state.begin(), state.end(), initial_ppu.begin(), initial_ppu.end());
        check(found != state.end(), "PPU fixture not found in full state");
        const auto base = static_cast<std::size_t>(found - state.begin());
        // Restore a live transfer rather than an empty FIFO at startup.
        reference.ppu.tick(100);
        const auto transfer = snapshot(reference.ppu);
        std::copy(transfer.begin(), transfer.end(), state.begin() + static_cast<std::ptrdiff_t>(base));
        core->load_state(state);
        const auto unchanged = core->save_state();
        for (std::size_t slot = 0; slot < 16; ++slot) {
            for (const auto& [field, invalid] : std::array<std::pair<std::size_t, std::uint8_t>, 4>{{
                     {0, 4}, {0, 255}, {1, 8}, {1, 255}}}) {
                auto bad = state;
                bad[base + fifo_offset + slot * 3 + field] = invalid;
                expect_failure<SaveStateError>([&] { core->load_state(bad); }, "unsafe BG FIFO accepted");
                check(core->save_state() == unchanged, "rejected PPU state changed current machine");
            }
        }
        // Maximum legal palette/color values remain loadable and renderable.
        for (std::size_t slot = 0; slot < 16; ++slot) {
            state[base + fifo_offset + slot * 3] = 3;
            state[base + fifo_offset + slot * 3 + 1] = 7;
        }
        core->load_state(state);
        std::vector<std::int32_t> frame(Ppu::frame_pixels); core->run_frame(frame, true);
    }
}

/**
 * Format 11 : chaque image PPU porte un octet d'encodage puis 16 bits par
 * pixel quand tous ses pixels se ré-encodent à l'identique. Le format 10
 * (i32 sans octet d'encodage) reste lisible, et une image non compactable
 * repasse en i32 sans perte.
 */
void frame_encoding_save_state_test() {
    constexpr auto pixels = static_cast<std::size_t>(Ppu::frame_pixels);
    InterruptController interrupts;
    Ppu source(interrupts, gbc::HardwareMode::cgb_native);
    for (std::size_t index = 0; index < pixels; ++index) {
        // Teintes DMG, puis couleurs CGB développées depuis 15 bits.
        const auto bgr = static_cast<std::uint32_t>(index * 37U) & 0x7fffU;
        const auto expand = [](std::uint32_t c5) { return (c5 << 3U) | (c5 >> 2U); };
        const auto argb = 0xff000000U | (expand(bgr & 31U) << 16U) |
            (expand((bgr >> 5U) & 31U) << 8U) | expand((bgr >> 10U) & 31U);
        source.completed_frame[index] = index % 3 == 0 ? static_cast<std::int32_t>(index % 4)
                                                       : static_cast<std::int32_t>(argb);
    }
    const std::size_t tail = source.vram.size() + source.oam.size() +
        source.bg_cram.size() + source.obj_cram.size();

    detail::BinaryWriter legacy_writer;
    source.save(legacy_writer, false);
    const auto legacy = std::move(legacy_writer).take();
    const auto compact = snapshot(source);
    check(compact.size() + 2 * pixels * 2 == legacy.size() + 2,
          "images PPU non compactées en 16 bits par pixel");

    const auto restore = [](std::span<const std::uint8_t> bytes, bool tagged) {
        InterruptController restored_interrupts;
        auto restored = std::make_unique<Ppu>(restored_interrupts, gbc::HardwareMode::cgb_native);
        detail::BinaryReader reader(bytes);
        restored->load(reader, tagged);
        check(reader.exhausted(), "état PPU laisse des données non consommées");
        return snapshot(*restored);
    };
    check_snapshot_equal(compact, restore(compact, true), "images PPU compactes altérées");
    check_snapshot_equal(compact, restore(legacy, false), "état PPU au format 10 mal relu");

    // Les 32768 couleurs CGB se compactent toutes sans perte.
    Ppu odd(interrupts, gbc::HardwareMode::cgb_native);
    for (std::size_t first = 0; first < 0x8000; first += pixels) {
        for (std::size_t index = 0; index < pixels; ++index) {
            const auto bgr = static_cast<std::uint32_t>((first + index) & 0x7fffU);
            const auto expand = [](std::uint32_t c5) { return (c5 << 3U) | (c5 >> 2U); };
            odd.completed_frame[index] = static_cast<std::int32_t>(0xff000000U | (expand(bgr & 31U) << 16U) |
                (expand((bgr >> 5U) & 31U) << 8U) | expand((bgr >> 10U) & 31U));
        }
        const auto colors = snapshot(odd);
        check(colors.size() == compact.size(), "couleur CGB non compactée");
        check_snapshot_equal(colors, restore(colors, true), "couleur CGB altérée par la compaction");
    }
    // Un pixel hors des deux familles garde son image en i32, sans perte.
    for (const std::uint32_t near_miss : {0x12345678U, 0x00000004U, 0xfeffffffU, 0xfff80000U,
                                          0xff000001U, 0xff00f800U, 0x7fffffffU}) {
        odd.completed_frame = source.completed_frame;
        odd.completed_frame[pixels / 2] = static_cast<std::int32_t>(near_miss);
        const auto mixed = snapshot(odd);
        check(mixed.size() == compact.size() + pixels * 2, "image non compactable mal encodée");
        check_snapshot_equal(mixed, restore(mixed, true), "image PPU en i32 altérée");
    }

    // Octets d'encodage : image de travail, puis image terminée.
    const auto working_tag = compact.size() - tail - 2 * (1 + pixels * 2);
    const auto completed_tag = working_tag + 1 + pixels * 2;
    check(compact[working_tag] == 1 && compact[completed_tag] == 1, "octet d'encodage PPU introuvable");
    for (const auto tag : {working_tag, completed_tag}) {
        auto unknown = compact;
        unknown[tag] = 2;
        expect_failure<SaveStateError>([&] { (void)restore(unknown, true); },
                                       "encodage d'image PPU inconnu accepté");
        auto bad_pixel = compact;
        bad_pixel[tag + 1] = 0x80;
        bad_pixel[tag + 2] = 0x04;
        expect_failure<SaveStateError>([&] { (void)restore(bad_pixel, true); },
                                       "pixel PPU compact invalide accepté");
    }
}

/**
 * Les états complets aux formats 10 (images PPU en i32, APU en disposition 1)
 * et 11 (APU en disposition 1) se rechargent, puis se réécrivent au format 12
 * sans rien perdre d'autre que ces différences.
 */
void legacy_core_state_loads_test() {
    for (auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_native}) {
        auto rom = ravenemu::testing::minimal_game_boy_rom();
        if (mode == gbc::HardwareMode::cgb_native) rom[0x143] = 0x80;
        auto core = gbc::make_core(); core->load_rom(rom, {});
        auto image = std::make_shared<const std::vector<std::uint8_t>>(rom);
        Machine reference(image, [] { return std::int64_t{0}; }, mode);
        const auto ppu_current = snapshot(reference.ppu);
        detail::BinaryWriter ppu_v10_writer;
        reference.ppu.save(ppu_v10_writer, false);
        const auto ppu_v10 = std::move(ppu_v10_writer).take();
        detail::BinaryWriter apu_writer;
        reference.apu.save(apu_writer);
        const auto apu_current = std::move(apu_writer).take();
        detail::BinaryWriter apu_v1_writer;
        reference.apu.save(apu_v1_writer, 1);
        const auto apu_v1 = std::move(apu_v1_writer).take();

        const auto state = core->save_state();
        check(state[4] == 0 && state[5] == 12, "version d'état GB inattendue");
        const auto replace = [](std::vector<std::uint8_t> bytes, const std::vector<std::uint8_t>& from,
                                const std::vector<std::uint8_t>& to) {
            const auto found = std::search(bytes.begin(), bytes.end(), from.begin(), from.end());
            check(found != bytes.end(), "fixture introuvable dans l'état complet");
            const auto offset = found - bytes.begin();
            bytes.erase(bytes.begin() + offset, bytes.begin() + offset + static_cast<std::ptrdiff_t>(from.size()));
            bytes.insert(bytes.begin() + offset, to.begin(), to.end());
            return bytes;
        };
        auto v11 = replace(state, apu_current, apu_v1);
        v11[5] = 11;
        auto v10 = replace(v11, ppu_current, ppu_v10);
        v10[5] = 10;
        std::vector<std::int32_t> frame(Ppu::frame_pixels);
        for (const auto* legacy : {&v10, &v11}) {
            core->run_frame(frame, true);
            core->load_state(*legacy);
            check(core->save_state() == state, "état GB d'un format antérieur mal relu");
        }
        auto future = state;
        future[5] = 13;
        expect_failure<SaveStateError>([&] { core->load_state(future); }, "version d'état future acceptée");
    }
}

/**
 * Le scan OAM retient jusqu'à dix sprites dont l'index sert ensuite, en
 * mode 3, à lire `oam[index * 4 + 2]`. Restaurés sans contrôle, ces index
 * permettaient à un état forgé de lire hors du tableau et de faire planter
 * l'hôte au rendu suivant.
 */
void retained_sprite_state_validation_test() {
    InterruptController interrupts;
    Ppu source(interrupts, gbc::HardwareMode::dmg);
    source.oam[0] = 17; // visible sur la ligne 1
    source.oam[1] = 8;
    source.write_lcdc(0x11);
    source.write_lcdc(0x93); // LCD + OBJ + BG
    while (!(source.ly() == 1 && source.line_dot() == 80)) source.tick(1);
    check(source.selected_object_count() == 1, "sprite de test non retenu par le scan OAM");
    const auto valid = snapshot(source);

    {
        InterruptController restored_interrupts;
        Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
        detail::BinaryReader in(valid);
        restored.load(in);
        for (int dot = 0; dot < 456; ++dot) restored.tick(1);
    }

    // Disposition PPU 72 : champs, FIFO BG (16 x 3), FIFO OBJ (16 x 5),
    // dix booléens de fetch, dix tuiles considérées, puis index, Y et X.
    constexpr std::size_t indices = 4 + 72 * 4 + 16 * 3 + 16 * 5 + 10 + 10 * 4;
    constexpr std::size_t y_values = indices + 10 * 4;
    constexpr std::size_t x_values = y_values + 10 * 4;
    for (const auto& [offset, value] : std::array<std::pair<std::size_t, std::int32_t>, 7>{{
             {indices, 40}, {indices, -1}, {indices, 0x1000'0000}, {indices + 9 * 4, 40},
             {y_values, 0x100}, {x_values, 0x100}, {x_values, -1}}}) {
        auto corrupt = valid;
        const auto raw = static_cast<std::uint32_t>(value);
        for (std::size_t i = 0; i < 4; ++i) {
            corrupt[offset + i] = static_cast<std::uint8_t>(raw >> (24U - 8U * i));
        }
        expect_failure<SaveStateError>(
            [&] {
                InterruptController restored_interrupts;
                Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
                detail::BinaryReader in(corrupt);
                restored.load(in);
            },
            "sprite retenu hors limites accepté dans un état PPU");
    }
}

/**
 * Registres 8 bits et compteurs du fetcher : aucun n'indexe un tableau, mais
 * des valeurs extrêmes faisaient déborder l'arithmétique signée. Chaque
 * valeur ci-dessous était acceptée avant validation (LCDC garde son bit 7).
 */
void register_state_validation_test() {
    InterruptController interrupts;
    Ppu source(interrupts, gbc::HardwareMode::dmg);
    source.write_lcdc(0x11);
    source.write_lcdc(0x91);
    while (source.mode() != Ppu::mode_transfer) source.tick(1);
    source.tick(20);
    const auto valid = snapshot(source);
    {
        InterruptController restored_interrupts;
        Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
        detail::BinaryReader in(valid);
        restored.load(in);
    }

    // Index des champs i32 de la disposition PPU 72 (après le compteur).
    constexpr std::size_t lcdc = 0, wx = 10, window_line = 21, scx_latched = 35;
    constexpr std::size_t startup_dots = 36, fetcher_tile = 46, fetcher_address = 49;
    for (const auto& [field, value] : std::array<std::pair<std::size_t, std::int32_t>, 7>{{
             {lcdc, 0x191}, {wx, -1}, {window_line, -1}, {scx_latched, 0x100},
             {startup_dots, 0x10000}, {fetcher_tile, 0x100}, {fetcher_address, -1}}}) {
        auto corrupt = valid;
        const auto offset = 4 + field * 4;
        const auto raw = static_cast<std::uint32_t>(value);
        for (std::size_t i = 0; i < 4; ++i) {
            corrupt[offset + i] = static_cast<std::uint8_t>(raw >> (24U - 8U * i));
        }
        expect_failure<SaveStateError>(
            [&] {
                InterruptController restored_interrupts;
                Ppu restored(restored_interrupts, gbc::HardwareMode::dmg);
                detail::BinaryReader in(corrupt);
                restored.load(in);
            },
            "registre ou compteur PPU hors limites accepté");
    }
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    corrupt_background_fifo_is_rejected();
    mid_oam_scan_save_state_test();
    startup_bus_gate_save_state_test();
    mid_transfer_save_state_test();
    mid_object_fetch_save_state_test();
    overlapping_object_fetch_save_state_test();
    mid_object_fifo_save_state_test();
    object_fifo_state_validation_test();
    ppu_timing_state_validation_test();
    transfer_at_the_line_end_is_rejected();
    retained_sprite_state_validation_test();
    register_state_validation_test();
    frame_encoding_save_state_test();
    legacy_core_state_loads_test();
    return 0;
}
