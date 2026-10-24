#include "ravenemu/core.hpp"
#include "ravenemu/gba/core.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
using ravenemu::testing::minimal_game_boy_rom;
using ravenemu::testing::minimal_gba_rom;

namespace {
// Le format 16 finit par l'instruction qui suit un Halt du BIOS en cours,
// puis SIOMULTI0-3 : un mot chacun. Le format 17 y ajoute le type de BIOS et
// son empreinte.
constexpr std::ptrdiff_t format16_bytes = 4 + 4 * 4;
constexpr std::ptrdiff_t format17_bytes = 1 + 32;

void gba_smoke_test() {
    auto core = ravenemu::make_gba_core(ravenemu::GbaSaveType::sram);
    const auto rom = minimal_gba_rom();
    core->load_rom(rom, {});
    check(core->console() == ravenemu::Console::game_boy_advance, "console GBA incorrecte");
    check(
        core->framebuffer_format() == ravenemu::FramebufferFormat::argb_8888,
        "format vidéo GBA incorrect"
    );
    check(core->supports_video_frame_skipping(), "saut de rendu GBA non déclaré");
    check(core->gba_save_type() == ravenemu::GbaSaveType::sram, "type SRAM GBA incorrect");
    check(core->has_battery_ram(), "sauvegarde GBA absente");
    std::vector<std::int32_t> frame(core->video_spec().pixel_count());
    core->run_frame(frame, true);
    const auto battery = core->snapshot_battery_ram();
    check(battery && battery->data.size() == 32U * 1024U, "taille SRAM GBA incorrecte");
    check(battery->data[0] == 0x5a, "écriture CPU vers SRAM GBA perdue");
    check(core->battery_ram_dirty(), "écriture SRAM GBA non signalée");
    core->acknowledge_battery_ram_saved(battery->generation);
    check(!core->battery_ram_dirty(), "acquittement SRAM GBA ignoré");

    core->set_measuring_time(true);
    check(core->measuring_time(), "mesure GBA non activée");
    core->run_frame(frame, true);
    const auto debug = core->debug_snapshot();
    check(debug.has_value(), "photographie de diagnostic GBA absente");
    check(debug->instructions_per_frame > 0, "compteur d'instructions GBA vide");

    core->reset();
    check(core->measuring_time(), "mesure GBA perdue après reset");

    const auto state = core->save_state();
    check(!state.empty(), "état GBA vide");
    std::fill(frame.begin(), frame.end(), 0x12345678);
    core->run_frame(frame, false);
    check(
        std::all_of(frame.begin(), frame.end(), [](std::int32_t pixel) {
            return pixel == 0x12345678;
        }),
        "le saut de rendu GBA a modifié le framebuffer"
    );
    core->load_state(state);
    check(core->battery_ram_dirty(), "RAM restaurée GBA non marquée à persister");
    core->acknowledge_battery_ram_saved(battery->generation);
    check(core->battery_ram_dirty(), "ancien acquittement accepté après restauration GBA");
    const auto persisted = core->snapshot_battery_ram();
    core->acknowledge_battery_ram_saved(persisted->generation);
    check(!core->battery_ram_dirty(), "nouvel acquittement GBA refusé");
    core->load_state(state);
    core->acknowledge_battery_ram_saved(persisted->generation);
    check(core->battery_ram_dirty(), "génération réutilisée après seconde restauration GBA");
    check(core->measuring_time(), "mesure GBA perdue après restauration d'état");
    core->run_frame(frame, true);
    const auto restored_battery = core->snapshot_battery_ram();
    check(restored_battery && restored_battery->data[0] == 0x5a, "SRAM GBA non restaurée");

    const auto before_failure = core->save_state();
    auto old_version = before_failure;
    old_version[4] = 0; old_version[5] = 9;
    expect_failure<ravenemu::SaveStateError>(
        [&] { core->load_state(old_version); }, "ancien format GBA accepté");
    check(core->save_state() == before_failure, "ancien format GBA a modifié la machine");
    auto truncated = before_failure;
    truncated.resize(truncated.size() / 2U);
    expect_failure<ravenemu::SaveStateError>(
        [&] { core->load_state(truncated); },
        "un état GBA tronqué a été accepté"
    );
    check(
        core->save_state() == before_failure,
        "la restauration GBA en échec a modifié la machine"
    );

    auto other_rom = rom;
    other_rom[0x100] = 1;
    auto other_core = ravenemu::make_gba_core(ravenemu::GbaSaveType::sram);
    other_core->load_rom(other_rom, {});
    expect_failure<ravenemu::SaveStateError>(
        [&] { other_core->load_state(before_failure); },
        "un état GBA d'une autre ROM a été accepté"
    );

    std::vector<std::int32_t> short_frame(core->video_spec().pixel_count() - 1U);
    expect_failure<std::invalid_argument>(
        [&] { core->run_frame(short_frame, true); },
        "un framebuffer GBA trop petit a été accepté"
    );
}

void gba_save_detection_test() {
    auto rom = minimal_gba_rom();
    constexpr std::string_view marker = "FLASH1M_V";
    std::copy(marker.begin(), marker.end(), rom.begin() + 0x100);
    auto detected = ravenemu::make_gba_core(std::nullopt);
    detected->load_rom(rom, {});
    check(
        detected->gba_save_type() == ravenemu::GbaSaveType::flash_128k,
        "détection Flash 128 Kio GBA incorrecte"
    );

    auto forced = ravenemu::make_gba_core(ravenemu::GbaSaveType::eeprom_8k);
    forced->load_rom(rom, {});
    check(
        forced->gba_save_type() == ravenemu::GbaSaveType::eeprom_8k,
        "type de sauvegarde GBA imposé ignoré"
    );
    forced->set_gba_forced_save_type(ravenemu::GbaSaveType::sram);
    check(
        forced->gba_save_type() == ravenemu::GbaSaveType::eeprom_8k,
        "le type GBA actif a changé avant le reset"
    );
    forced->reset();
    check(
        forced->gba_save_type() == ravenemu::GbaSaveType::sram,
        "le type GBA imposé n'a pas été appliqué au reset"
    );
}


void audio_state_continuation_test() {
    auto rom = minimal_gba_rom();
    // Configure PSG once, then loop without further IO writes. Sound is
    // switched on first: while it is off, PSG registers ignore writes.
    const std::array<std::uint32_t, 13> code{
        0xe59f0020, 0xe3a01080, 0xe5801084, 0xe59f1018, 0xe5801080,
        0xe59f1014, 0xe5801060, 0xe3a01902, 0xe5801064, 0xeafffffe,
        0x04000000, 0x00001177, 0xf0800000};
    for (std::size_t i = 0; i < code.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) rom[i * 4 + b] = static_cast<std::uint8_t>(code[i] >> (8U * b));
    auto core = ravenemu::make_gba_core(std::nullopt); core->load_rom(rom, {});
    std::vector<std::int32_t> frame(240 * 160);
    std::array<std::int16_t, 4096> first{}, restored{};
    core->run_frame(frame, true); core->read_audio(first);
    const auto state = core->save_state();
    core->run_frame(frame, true); const auto n = core->read_audio(first);
    check(std::any_of(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(n), [](auto v) { return v != 0; }), "audio fixture silent");
    core->load_state(state); core->run_frame(frame, true);
    const auto m = core->read_audio(restored);
    check(n == m && std::equal(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(n), restored.begin()), "audio continuation diverged");
    const auto unchanged = core->save_state();
    auto old = state; old[5] = 10;
    expect_failure<ravenemu::SaveStateError>([&] { core->load_state(old); }, "legacy audio-less state accepted");
    auto truncated = state; truncated.pop_back();
    expect_failure<ravenemu::SaveStateError>([&] { core->load_state(truncated); }, "truncated audio state accepted");
    check(core->save_state() == unchanged, "failed load changed GBA machine");
}

void prefetch_state_continuation_test() {
    // Boucle en ROM, tampon de préchargement actif (WAITCNT = 0x4317) : un
    // état pris en pleine lecture d'avance doit reprendre au même cycle.
    auto rom = minimal_gba_rom();
    const std::array<std::uint32_t, 9> code{
        0xe59f0014, 0xe59f1014, 0xe5801204, 0xe3a03403,  // r0 = E/S, WAITCNT, r3 = IWRAM
        0xe2822001, 0xe5832000, 0xeafffffc,              // boucle : ++r2, rangé en IWRAM
        0x04000000, 0x00004317};
    for (std::size_t i = 0; i < code.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) rom[i * 4 + b] = static_cast<std::uint8_t>(code[i] >> (8U * b));
    auto core = ravenemu::make_gba_core(std::nullopt);
    core->load_rom(rom, {});
    std::vector<std::int32_t> frame(240 * 160);
    core->run_frame(frame, false);
    const auto state = core->save_state();
    core->run_frame(frame, false);
    const auto continued = core->save_state();
    core->load_state(state);
    core->run_frame(frame, false);
    check(core->save_state() == continued, "préchargement GBA : reprise d'état décalée");
}

// ROM dont le code ARM commence à 0x08000000, et lectures de l'IWRAM par
// l'espace mémoire des succès (0x0000..0x7FFF).
std::vector<std::uint8_t> rom_with(std::span<const std::uint32_t> code) {
    auto rom = minimal_gba_rom();
    for (std::size_t i = 0; i < code.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) rom[i * 4 + b] = static_cast<std::uint8_t>(code[i] >> (8U * b));
    return rom;
}
std::uint32_t iwram(const ravenemu::Core& core, std::uint32_t address, std::size_t bytes) {
    std::array<std::uint8_t, 4> data{};
    check(core.read_achievement_memory(address, std::span(data).first(bytes)) == bytes, "IWRAM GBA illisible");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) value |= std::uint32_t{data[i]} << (8U * i);
    return value;
}

/**
 * Codes de triche : appliqués une fois par image, sans décaler la console
 * d'un cycle ni toucher au préchargement, et conservés quand la machine
 * change (état, réinitialisation).
 */
void cheats_test() {
    const auto parse = [](const char* code) { return ravenemu::parse_cheat(ravenemu::Console::game_boy_advance, code); };
    // Boucle en ROM, préchargement actif : ++r2 rangé en IWRAM.
    const std::array<std::uint32_t, 9> code{
        0xe59f0014, 0xe59f1014, 0xe5801204, 0xe3a03403,
        0xe2822001, 0xe5832000, 0xeafffffc,
        0x04000000, 0x00004317};
    const auto rom = rom_with(code);
    std::vector<std::int32_t> frame(240 * 160);
    auto plain = ravenemu::make_gba_core(std::nullopt);
    auto core = ravenemu::make_gba_core(std::nullopt);
    plain->load_rom(rom, {});
    core->load_rom(rom, {});
    // Des tests seuls (EWRAM, KEYINPUT, IWRAM) : rien ne doit changer, pas même un cycle.
    const std::array<ravenemu::Cheat, 1> tests{parse("72000100 0000\nD0000020 0001\nF3000000 0001")};
    core->set_cheats(tests);
    for (int index = 0; index < 3; ++index) {
        plain->run_frame(frame, false);
        core->run_frame(frame, false);
    }
    check(core->save_state() == plain->save_state(), "codes de triche : la console a été décalée");

    // 16 bits en EWRAM, 8 bits en IWRAM.
    const std::array<ravenemu::Cheat, 1> writes{parse("82000100 BEEF+33000100 0042")};
    core->set_cheats(writes);
    const auto state = core->save_state();
    core->run_frame(frame, false);
    check(iwram(*core, 0x8100, 2) == 0xbeefU && iwram(*core, 0x100, 1) == 0x42U, "codes de triche non appliqués");
    core->load_state(state);
    check(iwram(*core, 0x100, 1) == 0, "état restauré avec l'écriture du code");
    core->run_frame(frame, false);
    check(iwram(*core, 0x100, 1) == 0x42U, "codes de triche perdus après un état");
    core->set_cheats({});
    core->reset();
    core->run_frame(frame, false);
    check(iwram(*core, 0x100, 1) == 0, "codes de triche retirés toujours appliqués");

    const std::array<ravenemu::Cheat, 1> game_boy{ravenemu::parse_cheat(ravenemu::Console::game_boy, "01AA10C0")};
    expect_failure<std::invalid_argument>([&] { core->set_cheats(game_boy); }, "code Game Boy accepté par la GBA");
}

/**
 * Vrai BIOS : ses SWI et son démarrage remplacent ceux du BIOS réécrit. Le
 * BIOS de test, fait maison, marque 0x03000000 au démarrage et 0x03000001 à
 * chaque SWI, puis rend la main ; le jeu appelle GetBiosChecksum et marque
 * 0x03000002.
 */
std::vector<std::uint8_t> test_bios(std::uint8_t boot_marker) {
    std::vector<std::uint8_t> bios(0x4000);
    const auto store = [&](std::size_t address, std::uint32_t word) {
        for (unsigned byte = 0; byte < 4; ++byte) bios[address + byte] = static_cast<std::uint8_t>(word >> (8U * byte));
    };
    for (std::size_t vector = 0; vector < 0x20; vector += 4) store(vector, 0xeafffffeU);  // b .
    store(0x00, 0xea00003eU);  // reset : b 0x100
    store(0x08, 0xea00007cU);  // SWI : b 0x200
    // mov r0, #0x03000000 ; mov r1, #marque ; strb r1, [r0] ; mov pc, #0x08000000
    store(0x100, 0xe3a00403U);
    store(0x104, 0xe3a01000U | boot_marker);
    store(0x108, 0xe5c01000U);
    store(0x10c, 0xe3a0f302U);
    // mov r0, #0x03000000 ; mov r1, #0x5C ; strb r1, [r0, #1] ; movs pc, lr
    store(0x200, 0xe3a00403U);
    store(0x204, 0xe3a0105cU);
    store(0x208, 0xe5c01001U);
    store(0x20c, 0xe1b0f00eU);
    return bios;
}

void real_bios_test() {
    // swi 0x0D0000 (GetBiosChecksum) ; mov r0, #0x03000000 ; mov r1, #0xCA ; strb r1, [r0, #2] ; b .
    const std::array<std::uint32_t, 5> code{0xef0d0000U, 0xe3a00403U, 0xe3a010caU, 0xe5c01002U, 0xeafffffeU};
    const auto rom = rom_with(code);
    const auto bios = test_bios(0xb0);
    std::vector<std::int32_t> frame(240 * 160);
    struct Case {
        bool real;
        bool skip_intro;
        std::uint32_t markers;  // octets 0x03000000-02, petit-boutiste
    };
    for (const auto [real, skip_intro, markers] : {Case{false, false, 0xca0000U}, Case{true, false, 0xca5cb0U},
                                                   Case{true, true, 0xca5c00U}}) {
        auto core = ravenemu::make_gba_core(std::nullopt);
        if (real) core->set_gba_bios(bios, skip_intro);
        core->load_rom(rom, {});
        core->run_frame(frame, false);
        check(iwram(*core, 0, 3) == markers, "vrai BIOS GBA : démarrage ou SWI inattendu");
    }

    // Un état ne se recharge qu'avec le BIOS qui l'a créé.
    auto real = ravenemu::make_gba_core(std::nullopt);
    real->set_gba_bios(bios, false);
    real->load_rom(rom, {});
    real->run_frame(frame, false);
    const auto state = real->save_state();
    real->run_frame(frame, false);
    const auto continued = real->save_state();
    real->load_state(state);
    real->run_frame(frame, false);
    check(real->save_state() == continued, "vrai BIOS GBA : reprise d'état décalée");
    auto hle = ravenemu::make_gba_core(std::nullopt);
    hle->load_rom(rom, {});
    expect_failure<ravenemu::SaveStateError>([&] { hle->load_state(state); }, "état du vrai BIOS accepté sans lui");
    expect_failure<ravenemu::SaveStateError>([&] { real->load_state(hle->save_state()); },
                                             "état du BIOS réécrit accepté avec un vrai BIOS");
    auto other = ravenemu::make_gba_core(std::nullopt);
    other->set_gba_bios(test_bios(0xb1), false);
    other->load_rom(rom, {});
    expect_failure<ravenemu::SaveStateError>([&] { other->load_state(state); }, "état d'un autre BIOS accepté");

    // Retour au BIOS réécrit au prochain chargement : d'ici là, un état garde
    // le BIOS en place. Une taille fausse est refusée.
    real->set_gba_bios({}, false);
    real->load_state(state);
    real->load_rom(rom, {});
    real->run_frame(frame, false);
    check(iwram(*real, 0, 3) == 0xca0000U, "BIOS réécrit non rétabli");
    expect_failure<std::invalid_argument>([&] { real->set_gba_bios(std::span(bios).first(0x3fff), false); },
                                          "BIOS GBA de mauvaise taille accepté");
}

void irq_state_continuation_test() {
    // IRQ de VCount à la ligne 0 : elle se lève à la fin de chaque trame, et
    // l'état est pris avant qu'elle ne soit synchronisée. Le jeu compte dans
    // r5 entre deux IRQ ; reprise d'état décalée d'un cycle, compte décalé.
    // Assemblé avec llvm-mc.
    std::vector<std::uint32_t> code{
        0xe3a00301, 0xe59f1088, 0xe59f2088, 0xe5821000,  // r0 = E/S ; [0x03007FFC] = gestionnaire
        0xe3a01020, 0xe1c010b4,                          // DISPSTAT : IRQ de VCount à la ligne 0
        0xe2803c02, 0xe3a01004, 0xe1c310b0,              // IE = VCount
        0xe3a01001, 0xe1c310b8, 0xe321f01f};             // IME = 1, IRQ autorisées
    code.insert(code.end(), 24, 0xe2855001);             // boucle : 24 fois ++r5,
    code.insert(code.end(), {0xeaffffe6, 0x080000c0, 0x03007ffc});  // puis b boucle
    // Gestionnaire en 0x080000C0, après l'en-tête : acquitte IF, puis bx lr.
    const std::array<std::uint32_t, 5> handler{0xe3a00301, 0xe2800c02, 0xe3a01004, 0xe1c010b2, 0xe12fff1e};
    auto rom = rom_with(code);
    for (std::size_t i = 0; i < handler.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) rom[0xc0 + i * 4 + b] = static_cast<std::uint8_t>(handler[i] >> (8U * b));
    auto core = ravenemu::make_gba_core(std::nullopt);
    core->load_rom(rom, {});
    std::vector<std::int32_t> frame(240 * 160);
    core->run_frame(frame, false);
    auto settling = 0;
    for (int index = 0; index < 4; ++index) {
        const auto state = core->save_state();
        // L'âge de la ligne d'IRQ (gros-boutiste, -1 au repos) précède deux
        // mots pour le BIOS et le bloc du format 16 ; la ligne n'est
        // synchronisée qu'à 4 cycles.
        std::uint32_t age = 0;
        const auto at = state.end() - format17_bytes - format16_bytes - 12;
        for (auto byte = at; byte != at + 4; ++byte) age = age << 8U | *byte;
        if (age < 4) ++settling;
        core->run_frame(frame, false);
        const auto continued = core->save_state();
        core->load_state(state);
        core->run_frame(frame, false);
        check(core->save_state() == continued, "IRQ GBA : reprise d'état décalée");
    }
    check(settling > 0, "aucun état pris pendant la synchronisation d'une IRQ");
}

void bios_busy_state_continuation_test() {
    // Boucle de CpuFastSet de 16 Kio en EWRAM, et un timer à IRQ toutes les
    // 1024 cycles (llvm-mc) : la plupart des trames s'arrêtent pendant le temps
    // d'une copie, que les IRQ interrompent. Un état pris là doit reprendre
    // au même cycle (format 15).
    const std::array<std::uint32_t, 21> code{
        0xe3a00301, 0xe59f103c, 0xe59f203c, 0xe5821000,  // r0 = E/S ; [0x03007FFC] = gestionnaire
        0xe59f1038, 0xe5801100, 0xe2803c02, 0xe3a01008,  // TM0 : recharge 0xFC00, IRQ
        0xe1c310b0, 0xe3a01001, 0xe1c310b8, 0xe321f01f,  // IE = timer 0, IME = 1, IRQ autorisées
        0xe3a00402, 0xe2801801, 0xe3a02a01, 0xef0c0000,  // boucle : CpuFastSet 0x02000000 -> 0x02010000,
        0xe2855001, 0xeafffff9,                          // ++r5
        0x080000c0, 0x03007ffc, 0x00c0fc00};
    // Gestionnaire en 0x080000C0 : acquitte IF, ++r4, bx lr.
    const std::array<std::uint32_t, 6> handler{0xe3a00301, 0xe2800c02, 0xe3a01008, 0xe1c010b2, 0xe2844001, 0xe12fff1e};
    auto rom = rom_with(code);
    for (std::size_t i = 0; i < handler.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) rom[0xc0 + i * 4 + b] = static_cast<std::uint8_t>(handler[i] >> (8U * b));
    auto core = ravenemu::make_gba_core(std::nullopt);
    core->load_rom(rom, {});
    std::vector<std::int32_t> frame(240 * 160);
    core->run_frame(frame, false);
    auto busy = 0;
    for (int index = 0; index < 4; ++index) {
        const auto state = core->save_state();
        // Le temps restant d'une fonction du BIOS (gros-boutiste) précède
        // l'instruction qui la suit et le bloc du format 16.
        std::uint32_t remaining = 0;
        const auto at = state.end() - format17_bytes - format16_bytes - 8;
        for (auto byte = at; byte != at + 4; ++byte) remaining = remaining << 8U | *byte;
        if (remaining > 0) ++busy;
        core->run_frame(frame, false);
        const auto continued = core->save_state();
        core->load_state(state);
        core->run_frame(frame, false);
        check(core->save_state() == continued, "fonction du BIOS GBA : reprise d'état décalée");
    }
    check(busy > 0, "aucun état pris pendant une fonction du BIOS");
}

void bios_halt_state_continuation_test() {
    // Boucle de Halt du BIOS réveillée par un timer toutes les 1024 cycles
    // (llvm-mc) : presque tout le temps se passe dans le Halt. Le jeu ajoute
    // dans r7 l'instant de chaque reprise, lu sur le timer 1. Un état pris
    // pendant le Halt doit garder la sortie du BIOS qui suit le réveil
    // (format 16) : sinon, les reprises viennent plus tôt et r7 diffère.
    const std::array<std::uint32_t, 23> code{
        0xe3a00301, 0xe59f1044, 0xe59f2044, 0xe5821000,  // r0 = E/S ; [0x03007FFC] = gestionnaire
        0xe3a01502, 0xe5801104, 0xe59f1038, 0xe5801100,  // TM1 libre ; TM0 : recharge 0xFC00, IRQ
        0xe2803c02, 0xe3a01008, 0xe1c310b0, 0xe3a01001,  // IE = timer 0,
        0xe1c310b8, 0xe321f01f, 0xe2806c01,              // IME = 1, IRQ autorisées ; r6 = timers
        0xef020000, 0xe1d610b4, 0xe0877001, 0xe2855001,  // boucle : Halt du BIOS, r7 += TM1, ++r5
        0xeafffffa, 0x080000c0, 0x03007ffc, 0x00c0fc00};
    // Gestionnaire en 0x080000C0 : acquitte IF, ++r4, bx lr.
    const std::array<std::uint32_t, 6> handler{0xe3a00301, 0xe2800c02, 0xe3a01008, 0xe1c010b2, 0xe2844001, 0xe12fff1e};
    auto rom = rom_with(code);
    for (std::size_t i = 0; i < handler.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) rom[0xc0 + i * 4 + b] = static_cast<std::uint8_t>(handler[i] >> (8U * b));
    auto core = ravenemu::make_gba_core(std::nullopt);
    core->load_rom(rom, {});
    std::vector<std::int32_t> frame(240 * 160);
    core->run_frame(frame, false);
    auto halted = 0;
    for (int index = 0; index < 4; ++index) {
        const auto state = core->save_state();
        // L'instruction qui suit le Halt en cours (gros-boutiste) précède
        // SIOMULTI0-3 et le bloc du format 17.
        std::uint32_t resume = 0;
        const auto at = state.end() - format17_bytes - 16 - 4;
        for (auto byte = at; byte != at + 4; ++byte) resume = resume << 8U | *byte;
        if (resume == 0x0800'0040U) ++halted;
        core->run_frame(frame, false);
        const auto continued = core->save_state();
        core->load_state(state);
        core->run_frame(frame, false);
        check(core->save_state() == continued, "Halt du BIOS GBA : reprise d'état décalée");
    }
    check(halted > 0, "aucun état pris pendant un Halt du BIOS");
}

void multiplayer_data_state_test() {
    // Seule, la console fait un transfert multijoueur, puis recopie sans fin
    // SIOMULTI0 et SIOMULTI1 en IWRAM (llvm-mc). Un état les garde : rechargé
    // ailleurs, la copie continue de les lire.
    const std::array<std::uint32_t, 22> code{
        0xe3a00301, 0xe2801c01, 0xe3a02000, 0xe1c123b4,  // RCNT = 0
        0xe59f2034, 0xe1c122b8, 0xe59f2030, 0xe1c122ba,  // multijoueur ; SIOMLT_SEND = 0x1234
        0xe59f202c, 0xe1c122b8, 0xe1d122b8, 0xe3120080,  // départ, attente de la fin
        0x1afffffc, 0xe3a06403, 0xe1d122b0, 0xe1c620b0,  // boucle : [0x03000000] = SIOMULTI0,
        0xe1d122b2, 0xe1c620b2, 0xeafffffa, 0x00002003,  //          [0x03000002] = SIOMULTI1
        0x00001234, 0x00002083};
    const auto rom = rom_with(code);
    auto core = ravenemu::make_gba_core(std::nullopt);
    core->load_rom(rom, {});
    std::vector<std::int32_t> frame(240 * 160);
    core->run_frame(frame, false);
    check(iwram(*core, 0, 2) == 0x1234 && iwram(*core, 2, 2) == 0xffff, "SIOMULTI : transfert seul non reçu");
    const auto state = core->save_state();
    auto restored = ravenemu::make_gba_core(std::nullopt);
    restored->load_rom(rom, {});
    restored->run_frame(frame, false);
    restored->load_state(state);
    restored->run_frame(frame, false);
    check(iwram(*restored, 0, 2) == 0x1234 && iwram(*restored, 2, 2) == 0xffff, "SIOMULTI perdus par l'état");
}

void multiplayer_link_test() {
    // Assemblé avec llvm-mc. Chaque console passe en multijoueur à 115200
    // bauds, lit SI pour savoir si elle est parente, range 0xAA00 (parent) ou
    // 0xBB00 (enfant) dans SIOMLT_SEND. Le parent attend SD puis lance le
    // transfert ; l'enfant attend qu'il commence puis finisse. Chacune range
    // ensuite SIOMULTI0, SIOMULTI1, SIOCNT et SIOMULTI2 en IWRAM, puis recopie
    // sans cesse SIOCNT en IWRAM + 8.
    const std::array<std::uint32_t, 40> code{
        0xe59f2094, 0xe3a01000, 0xe1c211b4, 0xe3a01a02, 0xe3811003, 0xe1c210b8, 0xe1d230b8, 0xe3130004,
        0x03a04caa, 0x13a04cbb, 0xe1c240ba, 0x1a000008, 0xe1d230b8, 0xe3130008, 0x0afffffc, 0xe3833080,
        0xe1c230b8, 0xe1d230b8, 0xe3130080, 0x1afffffc, 0xea000005, 0xe1d230b8, 0xe3130080, 0x0afffffc,
        0xe1d230b8, 0xe3130080, 0x1afffffc, 0xe3a05403, 0xe1d260b0, 0xe1c560b0, 0xe1d260b2, 0xe1c560b2,
        0xe1d260b8, 0xe1c560b4, 0xe1d260b4, 0xe1c560b6, 0xe1d260b8, 0xe1c560b8, 0xeafffffc, 0x04000120};
    const auto rom = rom_with(code);
    auto parent = ravenemu::make_gba_core(std::nullopt);
    auto child = ravenemu::make_gba_core(std::nullopt);
    parent->load_rom(rom, {});
    child->load_rom(rom, {});
    const std::array<ravenemu::Core*, 2> players{parent.get(), child.get()};
    auto cable = ravenemu::connect_link_cable(players);
    check(cable->players() == 2, "câble GBA : nombre de joueurs");
    std::vector<std::int32_t> first(240 * 160), second(240 * 160);
    const std::array<std::span<std::int32_t>, 2> frames{first, second};
    for (int frame = 0; frame < 3; ++frame) cable->run_frame(frames, false);
    for (const auto* core : players) {
        check(iwram(*core, 0, 2) == 0xaa00 && iwram(*core, 2, 2) == 0xbb00, "câble GBA : données multijoueur non échangées");
        check(iwram(*core, 6, 2) == 0xffff, "câble GBA : troisième console absente non lue à 0xFFFF");
    }
    const auto parent_control = iwram(*parent, 4, 2);
    const auto child_control = iwram(*child, 4, 2);
    check((parent_control & 0x3c) == 0x08, "câble GBA : parent mal signalé (SI 0, SD 1, numéro 0)");
    check((child_control & 0x3c) == 0x1c, "câble GBA : enfant mal signalé (SI 1, SD 1, numéro 1)");
    check((parent_control & 0x80) == 0 && (child_control & 0x80) == 0, "câble GBA : transfert jamais terminé");

    // Débranchée, la console parente relit SIOCNT comme sans câble : SI tiré
    // à 1, toujours prête, numéro 0.
    cable.reset();
    parent->run_frame(first, false);
    check((iwram(*parent, 8, 2) & 0x3c) == 0x0c, "câble GBA débranché : SIOCNT garde l'état relié");

    // Seule, la même ROM se croit enfant (SI tiré à 1) et attend un parent.
    auto alone = ravenemu::make_gba_core(std::nullopt);
    alone->load_rom(rom, {});
    for (int frame = 0; frame < 3; ++frame) alone->run_frame(first, false);
    check(iwram(*alone, 0, 2) == 0, "GBA sans câble : transfert multijoueur inattendu");
}

void normal_link_test() {
    // Le joueur 0 (SI à 0 en multijoueur) devient maître en mode normal 32
    // bits à 2 MHz : il laisse l'esclave se préparer (VCOUNT 10), puis attend
    // que celui-ci, à horloge externe, tienne SO bas. Chacun range le mot reçu
    // en IWRAM.
    const std::array<std::uint32_t, 40> code{
        0xe59f208c, 0xe3a01000, 0xe1c211b4, 0xe3a01a02, 0xe1c210b8, 0xe1d230b8, 0xe3130004, 0x059f4074,
        0x159f4074, 0xe5824000, 0x1a00000f, 0xe3a01a01, 0xe381100b, 0xe1c210b8, 0xe3a00301, 0xe1d030b6,
        0xe353000a, 0x3afffffc, 0xe1d230b8, 0xe3130004, 0x1afffffc, 0xe3833080, 0xe1c230b8, 0xe1d230b8,
        0xe3130080, 0x1afffffc, 0xea000005, 0xe3a01a01, 0xe3811088, 0xe1c210b8, 0xe1d230b8, 0xe3130080,
        0x1afffffc, 0xe3a05403, 0xe5926000, 0xe5856000, 0xeafffffe, 0x04000120, 0x11223344, 0x55667788};
    const auto rom = rom_with(code);
    auto master = ravenemu::make_gba_core(std::nullopt);
    auto slave = ravenemu::make_gba_core(std::nullopt);
    master->load_rom(rom, {});
    slave->load_rom(rom, {});
    const std::array<ravenemu::Core*, 2> players{master.get(), slave.get()};
    auto cable = ravenemu::connect_link_cable(players);
    std::vector<std::int32_t> first(240 * 160), second(240 * 160);
    const std::array<std::span<std::int32_t>, 2> frames{first, second};
    for (int frame = 0; frame < 3; ++frame) cable->run_frame(frames, true);
    check(iwram(*master, 0, 4) == 0x55667788U, "câble GBA : le maître n'a pas reçu le mot de l'esclave");
    check(iwram(*slave, 0, 4) == 0x11223344U, "câble GBA : l'esclave n'a pas reçu le mot du maître");

    // Le câble est déterministe : la même partie donne le même état.
    auto again_master = ravenemu::make_gba_core(std::nullopt);
    auto again_slave = ravenemu::make_gba_core(std::nullopt);
    again_master->load_rom(rom, {});
    again_slave->load_rom(rom, {});
    const std::array<ravenemu::Core*, 2> again{again_master.get(), again_slave.get()};
    auto second_cable = ravenemu::connect_link_cable(again);
    for (int frame = 0; frame < 3; ++frame) second_cable->run_frame(frames, true);
    check(again_master->save_state() == master->save_state() && again_slave->save_state() == slave->save_state(),
          "câble GBA : deux parties identiques divergent");
}

void link_cable_validation_test() {
    auto gba = ravenemu::make_gba_core(std::nullopt);
    auto other = ravenemu::make_gba_core(std::nullopt);
    const std::array<ravenemu::Core*, 1> single{gba.get()};
    expect_failure<std::invalid_argument>([&] { (void)ravenemu::connect_link_cable(single); }, "câble à une console accepté");
    const std::array<ravenemu::Core*, 2> twice{gba.get(), gba.get()};
    expect_failure<std::invalid_argument>([&] { (void)ravenemu::connect_link_cable(twice); }, "console reliée à elle-même");
    const std::array<ravenemu::Core*, 2> pair{gba.get(), other.get()};
    auto cable = ravenemu::connect_link_cable(pair);
    std::vector<std::int32_t> first(240 * 160), second(240 * 160);
    const std::array<std::span<std::int32_t>, 2> frames{first, second};
    expect_failure<std::logic_error>([&] { cable->run_frame(frames, true); }, "câble GBA lancé sans ROM");
    const std::array<std::span<std::int32_t>, 1> one_frame{first};
    expect_failure<std::invalid_argument>([&] { cable->run_frame(one_frame, true); }, "câble GBA : image manquante");
}

void invalid_rom_test() {
    auto rom = minimal_gba_rom();
    rom[0xb2] = 0;
    auto gba = ravenemu::make_gba_core(std::nullopt);
    expect_failure<ravenemu::RomLoadError>(
        [&] { gba->load_rom(rom, {}); },
        "une ROM GBA sans marqueur a été acceptée"
    );
}

void public_header_factory_test() {
    // L'en-tête public `ravenemu/gba/core.hpp` ne compilait plus : sa fabrique
    // appelait `make_gba_core` sans l'argument désormais obligatoire.
    auto core = ravenemu::gba::make_core();
    core->load_rom(minimal_gba_rom(), {});
    check(core->console() == ravenemu::Console::game_boy_advance, "fabrique publique GBA incorrecte");
}

void odd_audio_capacity_keeps_stereo_frames_test() {
    auto core = ravenemu::make_gba_core(std::nullopt);
    core->load_rom(minimal_gba_rom(), {});
    std::vector<std::int32_t> frame(core->video_spec().pixel_count());
    core->run_frame(frame, true);
    // Une capacité impaire ne doit jamais couper une trame stéréo : sinon
    // gauche et droite restent inversées pour toutes les lectures suivantes.
    std::array<std::int16_t, 101> odd{};
    const auto first = core->read_audio(odd);
    check(first > 0, "aucun échantillon GBA produit");
    check(first % 2 == 0, "lecture audio GBA impaire : trame stéréo coupée");
    std::array<std::int16_t, 1> single{};
    check(core->read_audio(single) == 0, "un échantillon isolé GBA a été rendu");
}

void keypad_interrupt_state_validation_test() {
    auto core = ravenemu::make_gba_core(std::nullopt);
    core->load_rom(minimal_gba_rom(), {});
    const auto valid = core->save_state();
    // En-tête 39 + registres 64 + CPSR 4 + halt 1 + marqueur 4 + banques 112,
    // puis EWRAM, IWRAM, IO, palette, VRAM, OAM, SRAM de repli et touches.
    constexpr std::size_t keypad = 224 + 0x40000 + 0x8000 + 0x400 + 0x400 + 0x18000 + 0x400 + 0x10000;
    constexpr std::size_t ppu_field_count = keypad + 4;
    constexpr std::size_t interrupt_enable = ppu_field_count + 4 + 9 * 4 + 240 * 160 * 4;
    constexpr std::size_t interrupt_flags = interrupt_enable + 4;
    check(valid[ppu_field_count + 3] == 9, "disposition de l'état GBA inattendue");

    for (const auto& [offset, value] : std::array<std::pair<std::size_t, std::int32_t>, 4>{{
             {keypad, 0x400}, {keypad, -1}, {interrupt_enable, 0x10000}, {interrupt_flags, -1}}}) {
        auto corrupt = valid;
        const auto raw = static_cast<std::uint32_t>(value);
        for (std::size_t i = 0; i < 4; ++i) {
            corrupt[offset + i] = static_cast<std::uint8_t>(raw >> (24U - 8U * i));
        }
        expect_failure<ravenemu::SaveStateError>([&] { core->load_state(corrupt); },
                                                 "touches ou IE/IF GBA hors limites acceptés");
    }
    core->load_state(valid);
}

/** ROM au code de jeu donné ; avec `motor_on`, elle allume la broche 3 du GPIO. */
std::vector<std::uint8_t> gpio_rom(const char (&code)[5], bool motor_on) {
    auto rom = minimal_gba_rom();
    // LDR r0, =0x080000C4 ; MOV r1, #8 ; STRH r1, [r0, #2] ; STRH r1, [r0] ; B .
    const std::array<std::uint32_t, 6> program{0xe59f000cU, 0xe3a01008U, 0xe1c010b2U,
                                               0xe1c010b0U, 0xeafffffeU, 0x080000c4U};
    for (std::size_t index = 0; index < program.size(); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[index * 4U + byte] = static_cast<std::uint8_t>(program[index] >> (byte * 8U));
    if (!motor_on) { rom[0] = 0xfe; rom[1] = 0xff; rom[2] = 0xff; rom[3] = 0xea; } // B .
    std::copy_n(code, 4, rom.begin() + 0xac);
    return rom;
}

/**
 * Drill Dozer (V49) et WarioWare: Twisted! (RZW) portent un moteur de vibration
 * sur la broche 3 du GPIO (GBATEK). Le port existe alors sans horloge ; un état
 * enregistré avant cette prise en charge n'a pas de bloc GPIO et doit pourtant
 * se recharger, moteur au repos.
 */
void rumble_cartridge_test() {
    std::vector<std::int32_t> frame(240U * 160U);
    const auto running = [&](const std::vector<std::uint8_t>& rom, std::optional<bool> rtc = std::nullopt) {
        auto core = ravenemu::make_gba_core(std::nullopt);
        core->set_gba_forced_rtc(rtc);
        core->load_rom(rom, {});
        core->run_frame(frame, false);
        return core;
    };
    const auto motor_rom = gpio_rom("V49E", true);
    check(running(motor_rom)->rumble_active(), "Drill Dozer : moteur jamais allumé");
    check(running(gpio_rom("RZWE", true))->rumble_active(), "WarioWare: Twisted! : moteur jamais allumé");
    check(running(motor_rom, false)->rumble_active(), "horloge forcée absente : moteur perdu");
    check(!running(gpio_rom("AAAE", true))->rumble_active(), "moteur allumé sur une cartouche qui n'en a pas");
    check(!running(gpio_rom("V49E", false))->rumble_active(), "moteur allumé sans ordre du jeu");
    const auto motor = running(motor_rom);
    check(!motor->gba_rtc_active(), "vibreur pris pour une horloge");
    auto restored = ravenemu::make_gba_core(std::nullopt);
    restored->load_rom(motor_rom, {});
    restored->load_state(motor->save_state());
    check(restored->rumble_active(), "état du moteur perdu par la sauvegarde d'état");

    // Ancien état : celui de la même ROM, sans le drapeau ni les 23 mots du GPIO.
    // On le reconstitue d'après un état sans GPIO, identique hors en-tête et port.
    const auto with_gpio = motor->save_state();
    const auto without_gpio = running(gpio_rom("AAAE", true))->save_state();
    constexpr std::size_t gpio_bytes = 23U * 4U;
    check(with_gpio.size() == without_gpio.size() + gpio_bytes, "taille du bloc GPIO inattendue");
    std::size_t flag = 0;
    while (flag < without_gpio.size() &&
           !(without_gpio[flag] == 0 && with_gpio[flag] == 1 &&
             std::equal(without_gpio.begin() + static_cast<std::ptrdiff_t>(flag + 1), without_gpio.end(),
                        with_gpio.begin() + static_cast<std::ptrdiff_t>(flag + 1 + gpio_bytes)))) ++flag;
    check(flag < without_gpio.size(), "drapeau GPIO introuvable dans l'état");
    std::vector<std::uint8_t> legacy(with_gpio.begin(), with_gpio.begin() + static_cast<std::ptrdiff_t>(flag));
    legacy.push_back(0);
    legacy.insert(legacy.end(), with_gpio.begin() + static_cast<std::ptrdiff_t>(flag + 1 + gpio_bytes), with_gpio.end());
    restored->load_state(legacy);
    check(!restored->rumble_active(), "ancien état : moteur resté allumé");
    // Une horloge, elle, a un état à restaurer : son absence reste une erreur.
    auto clock = running(motor_rom, true);
    expect_failure<std::exception>([&] { clock->load_state(legacy); }, "ancien état accepté par une horloge");
}

/**
 * Le bus SRAM d'une cartouche à EEPROM est vide : il se lit 0xFF et une
 * écriture n'y reste pas. Les jeux Classic NES Series écrivent puis relisent
 * 0x0E000000 et affichent « GAME PAK ERROR » si l'octet est resté, comme sur une
 * copie équipée de SRAM. Sans type de sauvegarde détecté, la SRAM de repli reste.
 */
void eeprom_cartridge_has_no_sram_test() {
    auto rom = minimal_gba_rom();
    // LDR r0, =0x0E000000 ; MOV r1, #0x5A ; STRB r1, [r0] ; LDRB r2, [r0] ;
    // LDR r3, =0x03000000 ; STRB r2, [r3] ; B .
    const std::array<std::uint32_t, 9> program{0xe59f0014U, 0xe3a0105aU, 0xe5c01000U, 0xe5d02000U, 0xe59f3008U,
                                               0xe5c32000U, 0xeafffffeU, 0x0e000000U, 0x03000000U};
    for (std::size_t index = 0; index < program.size(); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            rom[index * 4U + byte] = static_cast<std::uint8_t>(program[index] >> (byte * 8U));
    std::vector<std::int32_t> frame(240U * 160U);
    const auto read_back = [&](std::optional<ravenemu::GbaSaveType> save_type) {
        auto core = ravenemu::make_gba_core(save_type);
        core->load_rom(rom, {});
        core->run_frame(frame, false);
        std::array<std::uint8_t, 1> out{};
        check(core->read_achievement_memory(0, out) == 1, "IWRAM illisible");
        return out[0];
    };
    check(read_back(ravenemu::GbaSaveType::eeprom_8k) == 0xff, "EEPROM 8 Kio : écriture restée sur le bus SRAM");
    check(read_back(ravenemu::GbaSaveType::eeprom_512) == 0xff, "EEPROM 512 o : écriture restée sur le bus SRAM");
    check(read_back(ravenemu::GbaSaveType::sram) == 0x5a, "SRAM : écriture perdue");
    check(read_back(std::nullopt) == 0x5a, "sans sauvegarde détectée : SRAM de repli perdue");
}

} // namespace

/** Carte mémoire exposée aux succès : IWRAM, EWRAM puis sauvegarde. */
void achievement_memory_map_test() {
    auto core = ravenemu::make_gba_core(ravenemu::GbaSaveType::sram);
    std::vector<std::uint8_t> battery(32U * 1024U);
    for (std::size_t i = 0; i < battery.size(); ++i) battery[i] = static_cast<std::uint8_t>(i * 7U + 3U);
    core->load_rom(minimal_gba_rom(), battery);

    std::vector<std::uint8_t> out(0x40000);
    const auto read = [&](std::uint32_t address, std::size_t size) {
        return core->read_achievement_memory(address, std::span{out}.first(size));
    };
    check(read(0x0, 0x8000) == 0x8000, "IWRAM non exposée en entier");
    check(read(0x8000, 0x40000) == 0x40000, "EWRAM non exposée en entier");
    check(read(0x48000, 0x10000) == battery.size() && out[0] == battery[0] && out[0x7fff] == battery.back(),
          "sauvegarde mal exposée");
    check(read(0x50000, 1) == 0 && read(0x58000, 1) == 0, "lecture au-delà de la sauvegarde acceptée");
    // Une lecture qui franchit une frontière de région rend les mêmes octets
    // que des lectures d'un octet, et s'arrête au même endroit.
    for (const std::uint32_t start : {0x7ff0U, 0x47ff0U, 0x4fff0U, 0x57ff0U}) {
        std::array<std::uint8_t, 0x40> block{};
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t offset = 0; offset < block.size(); ++offset) {
            std::array<std::uint8_t, 1> one{};
            if (core->read_achievement_memory(start + offset, one) != 1) break;
            bytes.push_back(one[0]);
        }
        const auto count = core->read_achievement_memory(start, block);
        check(count == bytes.size() && std::equal(bytes.begin(), bytes.end(), block.begin()),
              "lecture par blocs différente des lectures d'un octet");
    }
}

/**
 * Un reset ne perd pas une écriture pas encore enregistrée : la sauvegarde
 * reste à réécrire, et un acquittement d'avant le reset ne compte plus. Son
 * compteur ne repart pas de zéro : une génération déjà rendue ne resservira pas.
 */
void reset_keeps_unsaved_battery_test() {
    auto core = ravenemu::make_gba_core(ravenemu::GbaSaveType::sram);
    core->load_rom(minimal_gba_rom(), {});
    std::vector<std::int32_t> frame(core->video_spec().pixel_count());
    core->run_frame(frame, true);
    const auto before = core->snapshot_battery_ram();
    check(before && core->battery_ram_dirty(), "écriture SRAM GBA non signalée");
    core->reset();
    check(core->battery_ram_dirty(), "reset GBA : écriture non enregistrée oubliée");
    core->acknowledge_battery_ram_saved(before->generation);
    check(core->battery_ram_dirty(), "reset GBA : acquittement d'avant le reset accepté");
    const auto after = core->snapshot_battery_ram();
    check(after && after->data == before->data, "reset GBA : SRAM altérée");
    core->acknowledge_battery_ram_saved(after->generation);
    check(!core->battery_ram_dirty(), "reset GBA : acquittement refusé");

    // Sauvegarde à jour : le reset ne la marque pas, et l'acquittement tardif
    // d'une génération rendue avant lui ne couvre pas l'écriture qui suit.
    core->reset();
    check(!core->battery_ram_dirty(), "reset GBA : sauvegarde à jour marquée à réécrire");
    core->run_frame(frame, true);
    core->acknowledge_battery_ram_saved(before->generation);
    check(core->battery_ram_dirty(), "reset GBA : génération déjà rendue réutilisée");
}

/**
 * L'heure réglée par un jeu suit sa sauvegarde (bloc de 16 octets de mGBA) :
 * relue au chargement de la ROM, gardée par le reset.
 */
void rtc_settings_follow_battery_test() {
    constexpr std::int64_t epoch = 1'767'225'600;
    constexpr std::int64_t ten_days = 10 * 86'400;
    constexpr std::size_t sram = 32U * 1024U;
    auto core = ravenemu::make_gba_core(ravenemu::GbaSaveType::sram);
    core->set_gba_forced_rtc(true);
    core->set_clock_epoch(epoch);
    core->load_rom(minimal_gba_rom(), {});
    auto battery = core->snapshot_battery_ram()->data;
    check(battery.size() == sram + 16, "réglages de l'horloge GBA absents de la sauvegarde");

    // Même date de jeu, relevée dix jours plus tôt sur l'appareil : le jeu
    // avait avancé son horloge de dix jours.
    for (unsigned byte = 0; byte < 8; ++byte)
        battery[sram + 8 + byte] =
            static_cast<std::uint8_t>(static_cast<std::uint64_t>(epoch - ten_days) >> (8U * byte));
    core->load_rom(minimal_gba_rom(), battery);

    const auto seconds = static_cast<std::time_t>(epoch + ten_days);
    std::tm expected{};
#if defined(_WIN32)
    localtime_s(&expected, &seconds);
#else
    localtime_r(&seconds, &expected);
#endif
    const auto bcd = [](int value) {
        return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
    };
    const auto loaded = core->snapshot_battery_ram()->data;
    check(loaded[sram + 1] == bcd(expected.tm_mon + 1) && loaded[sram + 2] == bcd(expected.tm_mday) &&
              loaded[sram + 4] == bcd(expected.tm_hour),
          "heure réglée par le jeu perdue au chargement de la ROM");
    check(!core->battery_ram_dirty(), "sauvegarde relue marquée à réécrire");

    core->reset();
    check(core->snapshot_battery_ram()->data == loaded, "heure réglée par le jeu perdue au reset");
}

// Programme ARM original : commandes série, puis état/date/heure dans EWRAM.
// Aucun octet d'un pilote tiers ; les références GPIO sont réellement utilisées.
void automatic_rtc_public_core_test() {
    auto rom = ravenemu::testing::pokemon_rtc_gba_rom();
    rom.resize(0x10000);
    const auto word = [&](std::size_t at, std::uint32_t value) {
        for (unsigned b = 0; b < 4; ++b) rom[at + b] = static_cast<std::uint8_t>(value >> (b * 8));
    };
    for (const auto at : {0x240U,0x280U,0x2c0U}) word(at,0);
    word(0,0xea0000fe); // B 08000400
    word(0x3d0,0x080000c4); word(0x3d4,0x02000000); word(0x3d8,0x080000c6); word(0x3dc,0x080000c8);
    std::size_t at = 0x400;
    const auto emit = [&](std::uint32_t opcode) { word(at,opcode); at += 4; };
    emit(0xe51f0038); emit(0xe51f2038); emit(0xe51f5038); emit(0xe51f6038); // LDR r0,r2,r5,r6 : ports et résultat
    const auto write = [&](unsigned base, unsigned value, unsigned repeat = 1) {
        emit(0xe3a01000U | value); // MOV r1,#value
        for (unsigned n = 0; n < repeat; ++n) emit(0xe1c010b0U | (base << 16U)); // STRH r1,[base]
    };
    write(6,1);
    unsigned destination = 0;
    for (const auto& transfer : {std::pair{0x63U,1U}, std::pair{0x65U,7U}, std::pair{0x67U,3U}}) {
        write(0,1); write(0,5); write(5,7); // ordre des broches observé dans CFRU
        for (int bit = 7; bit >= 0; --bit) {
            const auto pins = 4U | (((transfer.first >> static_cast<unsigned>(bit)) & 1U) << 1U);
            write(0,pins,3); write(0,pins | 1U);
        }
        write(5,5);
        for (unsigned byte = 0; byte < transfer.second; ++byte) {
            emit(0xe3a04000); // MOV r4,#0
            for (unsigned bit = 0; bit < 8; ++bit) {
                write(0,4,5); write(0,5);
                emit(0xe1d030b0); emit(0xe2033002); // LDRH r3,[r0] ; AND r3,r3,#2
                emit(0xe1844003U | (bit == 0 ? 0xa0U : (bit - 1) << 7U)); // ORR r4,r4,r3,shift
            }
            emit(0xe5c24000U | destination++); // STRB r4,[r2,#destination]
        }
        write(0,1,2);
    }
    emit(0xeafffffe);
    auto core = ravenemu::make_gba_core(std::nullopt);
    constexpr std::int64_t epoch = 1'767'225'600;
    core->set_clock_epoch(epoch); core->load_rom(rom,{});
    check(core->gba_rtc_active() && core->gba_save_type() == ravenemu::GbaSaveType::flash_128k,
          "interface publique : hack non détecté automatiquement");
    std::vector<std::int32_t> frame(core->video_spec().pixel_count());
    const auto read = [&] {
        core->run_frame(frame,false);
        std::array<std::uint8_t,11> bytes{};
        check(core->read_achievement_memory(0x8000,bytes) == bytes.size(), "résultat RTC EWRAM inaccessible");
        return bytes;
    };
    const auto expected = [](std::int64_t seconds) {
        const auto value = static_cast<std::time_t>(seconds); std::tm now{};
#if defined(_WIN32)
        localtime_s(&now,&value);
#else
        localtime_r(&value,&now);
#endif
        const auto bcd = [](int n) { return static_cast<std::uint8_t>((n / 10) * 16 + n % 10); };
        const auto hour = static_cast<std::uint8_t>(bcd(now.tm_hour) | (now.tm_hour >= 12 ? 0x80 : 0));
        return std::array<std::uint8_t,11>{0x40,bcd((now.tm_year + 1900) % 100),bcd(now.tm_mon + 1),bcd(now.tm_mday),
            static_cast<std::uint8_t>(now.tm_wday),hour,bcd(now.tm_min),bcd(now.tm_sec),hour,bcd(now.tm_min),bcd(now.tm_sec)};
    };
    check(read() == expected(epoch), "programme ARM : date/heure/état RTC incorrects");
    const auto first = core->snapshot_battery_ram()->data;
    check(first.size() == 0x20000 + 16, "taille .sav d'un hack RTC incorrecte");
    core->set_clock_epoch(epoch + 86'461); core->reset();
    check(read() == expected(epoch + 86'461), "reset ou avancement de l'heure hôte perdu");
    core->load_rom(rom,first);
    check(read() == expected(epoch + 86'461), "rechargement .sav a décalé la date système");
    auto legacy = first; legacy.resize(0x20000); legacy[0x123] = 0x5a; legacy[0x10123] = 0xa5;
    core->load_rom(rom,legacy);
    auto battery = core->snapshot_battery_ram()->data;
    check(std::equal(legacy.begin(),legacy.end(),battery.begin()), "ancienne .sav Flash altérée");
    // Un décalage déjà enregistré doit suivre le reset de l'émulateur.
    for (unsigned byte = 0; byte < 8; ++byte)
        battery[0x20008 + byte] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(epoch + 86'461 - 3600) >> (byte * 8));
    core->load_rom(rom,battery); core->reset();
    check(read() == expected(epoch + 86'461 + 3600), "décalage batterie perdu au reset automatique");
    core->set_gba_forced_rtc(false); core->load_rom(rom,legacy);
    check(!core->gba_rtc_active() && core->snapshot_battery_ram()->data.size() == 0x20000,
          "forçage RTC off perdu dans l'interface publique");
    core->set_gba_forced_rtc(std::nullopt); core->set_clock_epoch(std::nullopt);
    const auto before = std::time(nullptr);
    core->load_rom(rom,{});
    const auto host_battery = core->snapshot_battery_ram()->data;
    const auto after = std::time(nullptr);
    std::uint64_t latch = 0;
    for (unsigned byte = 0; byte < 8; ++byte) latch |= std::uint64_t{host_battery[0x20008 + byte]} << (byte * 8);
    check(latch >= static_cast<std::uint64_t>(before) && latch <= static_cast<std::uint64_t>(after),
          "RTC sans override n'utilise pas l'horloge système réelle");
    const auto local = expected(static_cast<std::int64_t>(latch));
    for (std::size_t field = 0; field < 7; ++field)
        check(host_battery[0x20000 + field] == (local[field + 1] & (field == 4 ? 0x7f : 0xff)),
              "horloge système : date locale du bloc RTC incorrecte");
}

int main() {
    achievement_memory_map_test();
    keypad_interrupt_state_validation_test();
    public_header_factory_test();
    odd_audio_capacity_keeps_stereo_frames_test();
    audio_state_continuation_test();
    prefetch_state_continuation_test();
    cheats_test();
    real_bios_test();
    irq_state_continuation_test();
    bios_busy_state_continuation_test();
    bios_halt_state_continuation_test();
    multiplayer_data_state_test();
    multiplayer_link_test();
    normal_link_test();
    link_cable_validation_test();
    invalid_rom_test();
    gba_smoke_test();
    gba_save_detection_test();
    rumble_cartridge_test();
    eeprom_cartridge_has_no_sram_test();
    reset_keeps_unsaved_battery_test();
    rtc_settings_follow_battery_test();
    automatic_rtc_public_core_test();
    return 0;
}
