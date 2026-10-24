#include "ravenemu/core.hpp"
#include "ravenemu/gb/core.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
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
class ProbeInfraredPort final : public ravenemu::InfraredPortEndpoint {
public:
    [[nodiscard]] bool infrared_led_on() const noexcept override { return output; }
    void set_infrared_light(bool detected) noexcept override { input = detected; }

    bool output{};
    bool input{};
};

void game_boy_smoke_test() {
    auto core = ravenemu::make_game_boy_core();
    const auto rom = minimal_game_boy_rom();
    core->load_rom(rom, {});
    check(core->console() == ravenemu::Console::game_boy, "console GB incorrecte");
    check(
        core->framebuffer_format() == ravenemu::FramebufferFormat::indexed_4,
        "format vidéo DMG incorrect"
    );
    std::vector<std::int32_t> frame(core->video_spec().pixel_count());
    core->run_frame(frame, true);
    const auto state = core->save_state();
    check(!state.empty(), "état GB vide");
    core->run_frame(frame, true);
    core->load_state(state);
    core->run_frame(frame, true);

    const auto before_failure = core->save_state();
    auto truncated = before_failure;
    truncated.pop_back();
    expect_failure<ravenemu::SaveStateError>(
        [&] { core->load_state(truncated); },
        "un état GB tronqué a été accepté"
    );
    check(
        core->save_state() == before_failure,
        "la restauration GB en échec a modifié la machine"
    );

    auto old_version = before_failure;
    old_version[4] = 9;
    old_version[5] = 0;
    expect_failure<ravenemu::SaveStateError>(
        [&] { core->load_state(old_version); },
        "un état GB/GBC de version 9 a été accepté comme version 10"
    );
    check(core->save_state() == before_failure,
          "le refus d'une ancienne version de save state a modifié la machine");

    auto other_rom = rom;
    other_rom[0x0134] = 1;
    auto other_core = ravenemu::make_game_boy_core();
    other_core->load_rom(other_rom, {});
    expect_failure<ravenemu::SaveStateError>(
        [&] { other_core->load_state(before_failure); },
        "un état GB d'une autre ROM a été accepté"
    );

    std::vector<std::int32_t> short_frame(core->video_spec().pixel_count() - 1U);
    expect_failure<std::invalid_argument>(
        [&] { core->run_frame(short_frame, true); },
        "un framebuffer GB trop petit a été accepté"
    );
}

void game_boy_color_and_battery_test() {
    auto cgb_rom = minimal_game_boy_rom();
    cgb_rom[0x0143] = 0x80;
    auto cgb = ravenemu::make_game_boy_core();
    cgb->load_rom(cgb_rom, {});
    check(
        cgb->framebuffer_format() == ravenemu::FramebufferFormat::argb_8888,
        "le mode couleur CGB n'a pas été activé"
    );

    auto battery_rom = minimal_game_boy_rom();
    battery_rom[0x0147] = 0x03; // MBC1 + RAM + pile
    battery_rom[0x0149] = 0x02; // 8 Kio
    std::vector<std::uint8_t> battery(8U * 1024U, 0x5a);
    auto battery_core = ravenemu::make_game_boy_core();
    battery_core->load_rom(battery_rom, battery);
    check(battery_core->has_battery_ram(), "RAM à pile GB non détectée");
    check(!battery_core->battery_ram_dirty(), "RAM GB importée marquée sale");
    const auto snapshot = battery_core->snapshot_battery_ram();
    check(snapshot.has_value(), "instantané batterie GB absent");
    check(snapshot->data == battery, "contenu batterie GB altéré à l'import");
    const auto state = battery_core->save_state();
    battery_core->load_state(state);
    check(battery_core->battery_ram_dirty(), "RAM restaurée GB non marquée à persister");
    battery_core->acknowledge_battery_ram_saved(snapshot->generation);
    check(battery_core->battery_ram_dirty(), "ancien acquittement accepté après restauration GB");
    const auto restored = battery_core->snapshot_battery_ram();
    check(restored && restored->data == battery, "RAM restaurée GB altérée");
    battery_core->acknowledge_battery_ram_saved(restored->generation);
    check(!battery_core->battery_ram_dirty(), "nouvel acquittement GB refusé");
    battery_core->load_state(state);
    battery_core->acknowledge_battery_ram_saved(restored->generation);
    check(battery_core->battery_ram_dirty(), "génération réutilisée après seconde restauration GB");

    // Un reset ne perd pas une écriture pas encore enregistrée.
    const auto before_reset = battery_core->snapshot_battery_ram();
    battery_core->reset();
    check(battery_core->battery_ram_dirty(), "reset GB : écriture non enregistrée oubliée");
    battery_core->acknowledge_battery_ram_saved(before_reset->generation);
    check(battery_core->battery_ram_dirty(), "reset GB : acquittement d'avant le reset accepté");
    const auto after_reset = battery_core->snapshot_battery_ram();
    check(after_reset && after_reset->data == battery, "reset GB : RAM altérée");
    battery_core->acknowledge_battery_ram_saved(after_reset->generation);
    check(!battery_core->battery_ram_dirty(), "reset GB : acquittement refusé");

    // Sauvegarde à jour : le reset ne la marque pas, et l'acquittement tardif
    // d'une génération rendue avant lui ne couvre pas la modification qui suit.
    battery_core->reset();
    check(!battery_core->battery_ram_dirty(), "reset GB : sauvegarde à jour marquée à réécrire");
    battery_core->load_state(state);
    battery_core->acknowledge_battery_ram_saved(restored->generation);
    check(battery_core->battery_ram_dirty(), "reset GB : génération déjà rendue réutilisée");
}

void mbc6_large_state_test() {
    std::vector<std::uint8_t> rom(1U * 1024U * 1024U);
    rom[0x0147] = 0x20;
    auto core = ravenemu::make_game_boy_core();
    core->load_rom(rom, {});
    check(core->has_battery_ram(), "persistance MBC6 absente de l'API du cœur");

    const auto state = core->save_state();
    check(state.size() > (1U << 20U) && state.size() <= (2U << 20U),
          "taille du save state MBC6 hors de la garde documentée");
    core->load_state(state);
    check(core->save_state() == state,
          "save state MBC6 supérieur à 1 Mio non restauré déterministement");
}

void mbc7_core_contract_test() {
    std::vector<std::uint8_t> rom(2U * 1024U * 1024U);
    rom[0x0143] = 0xc0;
    rom[0x0147] = 0x22;
    auto core = ravenemu::make_game_boy_core();
    core->load_rom(rom, {});
    check(core->has_battery_ram(), "persistance MBC7 absente de l'API du cœur");
    const auto before = core->save_state();
    core->set_game_boy_acceleration(0x70, -0x70);
    const auto tilted = core->save_state();
    check(tilted != before, "entrée d'inclinaison MBC7 absente du cœur public");
    core->reset();
    check(core->save_state() == tilted,
          "reset MBC7 a perdu l'inclinaison physique courante");
    core->load_state(before);
    check(core->save_state() == before,
          "save state MBC7 n'a pas restauré l'entrée d'accéléromètre");

    auto preloaded = ravenemu::make_game_boy_core();
    preloaded->set_game_boy_acceleration(0x70, -0x70);
    preloaded->load_rom(rom, {});
    check(preloaded->save_state() == tilted,
          "inclinaison MBC7 envoyée avant le chargement perdue");
}

void huc1_core_contract_test() {
    std::vector<std::uint8_t> rom(1U * 1024U * 1024U);
    rom[0x0143] = 0x80;
    rom[0x0147] = 0xff;
    rom[0x0148] = 0x05;
    rom[0x0149] = 0x03;
    std::vector<std::uint8_t> battery(32U * 1024U, 0xa5);

    // Le backend est possédé par l'hôte et englobe la durée de vie du cœur.
    ravenemu::LocalInfraredEndpoint infrared;
    ProbeInfraredPort peer;
    ProbeInfraredPort spare;
    check(infrared.attach(peer), "sonde distante HuC1 non attachée");
    auto core = ravenemu::make_game_boy_core();
    check(core->connect_infrared_endpoint(&infrared),
          "backend IR refusé avant le chargement HuC1");
    core->load_rom(rom, battery);
    check(!infrared.attach(spare),
          "routeur HuC1 absent du backend après chargement");
    check(core->has_battery_ram(), "persistance HuC1 absente de l'API du cœur");
    const auto snapshot = core->snapshot_battery_ram();
    check(snapshot && snapshot->data == battery,
          "SRAM HuC1 altérée par le chemin public de persistance");

    const auto state = core->save_state();
    core->reset();
    check(!infrared.attach(spare),
          "routeur HuC1 non reconnecté automatiquement après reset");
    core->load_state(state);
    check(!infrared.attach(spare) && core->save_state() == state,
          "save state HuC1 non restauré déterministement par le cœur public");
}

void huc3_core_contract_test() {
    std::vector<std::uint8_t> rom(2U * 1024U * 1024U);
    rom[0x0147] = 0xfe;
    rom[0x0148] = 0x06;
    std::vector<std::uint8_t> legacy_sram(32U * 1024U, 0x5c);

    ravenemu::LocalInfraredEndpoint infrared;
    ProbeInfraredPort peer;
    ProbeInfraredPort spare;
    check(infrared.attach(peer), "sonde distante HuC3 non attachée");
    auto core = ravenemu::make_game_boy_core();
    core->set_clock_epoch(50'000);
    check(core->connect_infrared_endpoint(&infrared),
          "backend IR refusé avant le chargement HuC3");
    core->load_rom(rom, legacy_sram);
    check(!infrared.attach(spare),
          "routeur HuC3 absent du backend après chargement");
    check(core->has_battery_ram() && !core->battery_ram_dirty(),
          "persistance HuC3 absente ou importée sale dans l'API du cœur");
    const auto snapshot = core->snapshot_battery_ram();
    constexpr std::size_t footer_size = 143;
    check(snapshot && snapshot->data.size() == legacy_sram.size() + footer_size &&
          std::equal(legacy_sram.begin(), legacy_sram.end(), snapshot->data.begin()) &&
          snapshot->data[legacy_sram.size()] == 'R' &&
          snapshot->data[legacy_sram.size() + 1] == 'V' &&
          snapshot->data[legacy_sram.size() + 2] == 'H' &&
          snapshot->data[legacy_sram.size() + 3] == '3',
          "SRAM brute HuC3 non migrée vers son pied de page RTC");

    const auto state = core->save_state();
    core->reset();
    check(!infrared.attach(spare) && core->save_state() == state,
          "reset HuC3 a perdu son état persistant ou son endpoint");
    core->load_state(state);
    check(core->save_state() == state,
          "save state HuC3 non restauré déterministement par le cœur public");
}

void invalid_rom_test() {
    auto gb = ravenemu::make_game_boy_core();
    expect_failure<ravenemu::RomLoadError>(
        [&] { gb->load_rom(std::vector<std::uint8_t>(16), {}); },
        "une ROM GB trop courte a été acceptée"
    );
}

} // namespace

/**
 * Registres CPU, interruptions et timer d'un état complet : tous sont des
 * registres 8 ou 16 bits que le cœur masque toujours. Des valeurs hors de ces
 * plages étaient restaurées telles quelles et faisaient déborder
 * l'arithmétique signée (`pc + 1`, décompte de recharge...).
 */
// Un programme écrit à partir de 0x150, lancé par JP 0x150.
std::vector<std::uint8_t> rom_running(std::vector<std::uint8_t> rom, std::initializer_list<std::uint8_t> program) {
    rom[0x100] = 0xc3;
    rom[0x101] = 0x50;
    rom[0x102] = 0x01;
    std::copy(program.begin(), program.end(), rom.begin() + 0x150);
    return rom;
}

/**
 * Ce que les succès lisent est ce que lit le CPU : registres d'E/S, banque de
 * ROM choisie par le jeu, et première fenêtre que le MBC1 commute en mode 1.
 */
void achievement_memory_follows_the_cpu_test() {
    std::vector<std::uint8_t> out(0x4000);
    const auto run = [](ravenemu::Core& core) {
        std::vector<std::int32_t> frame(core.video_spec().pixel_count());
        core.run_frame(frame, true);
    };
    // LD HL,$FF40 ; LD DE,$C000 ; LD B,12 ; boucle : LD A,(HL+) ; LD (DE),A ; INC DE ; DEC B ; JR NZ ; JR $
    for (const bool color : {false, true}) {
        auto rom = rom_running(minimal_game_boy_rom(), {0x21, 0x40, 0xff, 0x11, 0x00, 0xc0, 0x06, 0x0c, 0x2a, 0x12,
                                                        0x13, 0x05, 0x20, 0xfa, 0x18, 0xfe});
        rom[0x143] = color ? 0x80 : 0x00;
        auto core = ravenemu::make_game_boy_core();
        core->load_rom(rom, {});
        run(*core);
        std::array<std::uint8_t, 12> registers{}, copied{};
        check(core->read_achievement_memory(0xff40, registers) == 12 && core->read_achievement_memory(0xc000, copied) == 12,
              "registres LCD illisibles");
        // LY (FF44) et le mode de STAT (FF41) avancent entre la copie et la lecture.
        for (const std::size_t index : {0U, 2U, 3U, 5U, 6U, 7U, 8U, 9U, 10U, 11U})
            check(registers[index] == copied[index], "registre d'E/S différent de la lecture du CPU");
    }
    // MBC5, 16 banques remplies de leur numéro ; LD A,5 ; LD ($2000),A ; JR $
    {
        auto rom = minimal_game_boy_rom();
        rom.resize(16U * 0x4000U);
        for (std::size_t bank = 1; bank < 16; ++bank)
            std::fill_n(rom.begin() + static_cast<std::ptrdiff_t>(bank * 0x4000U), 0x4000, static_cast<std::uint8_t>(bank));
        rom[0x147] = 0x19;
        rom[0x148] = 0x03;
        rom = rom_running(std::move(rom), {0x3e, 0x05, 0xea, 0x00, 0x20, 0x18, 0xfe});
        auto core = ravenemu::make_game_boy_core();
        core->load_rom(rom, {});
        check(core->read_achievement_memory(0x4000, out) == 0x4000 && out.front() == 1 && out.back() == 1,
              "banque 1 absente au démarrage");
        run(*core);
        check(core->read_achievement_memory(0x4000, out) == 0x4000 &&
              std::all_of(out.begin(), out.end(), [](std::uint8_t byte) { return byte == 5; }),
              "la banque de ROM choisie par le jeu n'est pas celle exposée");
        check(core->read_achievement_memory(0x0000, out) == 0x4000 && std::equal(out.begin(), out.end(), rom.begin()),
              "banque 0 de ROM incorrecte");
    }
    // MBC1 de 1 Mio en mode 1 : la première fenêtre passe à la banque $20.
    // LD A,1 ; LD ($6000),A ; LD ($4000),A ; JR $ (aussi présent en banque $20)
    {
        auto rom = minimal_game_boy_rom();
        rom.resize(64U * 0x4000U);
        for (std::size_t bank = 1; bank < 64; ++bank)
            std::fill_n(rom.begin() + static_cast<std::ptrdiff_t>(bank * 0x4000U), 0x4000, static_cast<std::uint8_t>(bank));
        rom[0x147] = 0x01;
        rom[0x148] = 0x05;
        rom = rom_running(std::move(rom), {0x3e, 0x01, 0xea, 0x00, 0x60, 0xea, 0x00, 0x40, 0x18, 0xfe});
        std::copy_n(rom.begin() + 0x150, 10, rom.begin() + 0x20 * 0x4000 + 0x150);
        auto core = ravenemu::make_game_boy_core();
        core->load_rom(rom, {});
        std::array<std::uint8_t, 1> byte{};
        check(core->read_achievement_memory(0x3000, byte) == 1 && byte[0] == 0, "banque 0 absente au démarrage");
        run(*core);
        check(core->read_achievement_memory(0x3000, byte) == 1 && byte[0] == 0x20,
              "la première fenêtre ne suit pas le MBC1 en mode 1");
    }
}

void cpu_interrupt_timer_state_validation_test() {
    auto core = ravenemu::make_game_boy_core();
    core->load_rom(minimal_game_boy_rom(), {});
    const auto valid = core->save_state();
    // En-tête 39 octets + mode matériel ; CPU : AF BC DE HL SP PC (i32)
    // puis six booléens ; IF, IE ; timer : DIV TIMA TMA TAC recharge.
    constexpr std::size_t sp = 56, pc = 60, interrupt_flags = 70, interrupt_enable = 74;
    constexpr std::size_t tac = 90, reload_delay = 94;
    const auto i32_at = [&valid](std::size_t offset) {
        return static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(valid[offset]) << 24U) |
            (static_cast<std::uint32_t>(valid[offset + 1]) << 16U) |
            (static_cast<std::uint32_t>(valid[offset + 2]) << 8U) | valid[offset + 3]);
    };
    check(i32_at(sp) == 0xfffe && i32_at(pc) == 0x0100, "disposition CPU de l'état GB inattendue");
    check(i32_at(tac) == 0xf8 && i32_at(reload_delay) == -1, "disposition timer de l'état GB inattendue");

    for (const auto& [offset, value] : std::array<std::pair<std::size_t, std::int32_t>, 8>{{
             {sp, 0x10000}, {pc, 0x10000}, {pc, -1}, {interrupt_flags, 0x100},
             {interrupt_enable, -1}, {tac, 0x100}, {reload_delay, 5}, {reload_delay, -2}}}) {
        auto corrupt = valid;
        const auto raw = static_cast<std::uint32_t>(value);
        for (std::size_t i = 0; i < 4; ++i) {
            corrupt[offset + i] = static_cast<std::uint8_t>(raw >> (24U - 8U * i));
        }
        expect_failure<ravenemu::SaveStateError>([&] { core->load_state(corrupt); },
                                                 "registre CPU/IRQ/timer GB hors limites accepté");
    }
    core->load_state(valid);
}

/**
 * Carte mémoire exposée aux succès (disposition rcheevos GB/GBC) : bornes de
 * chaque région et contenu de la RAM de cartouche, importée avec un motif.
 */
void achievement_memory_map_test() {
    for (const bool color : {false, true}) {
        auto rom = minimal_game_boy_rom();
        rom[0x143] = color ? 0x80 : 0x00;
        rom[0x147] = 0x1b; // MBC5 + RAM + batterie
        rom[0x149] = 0x03; // 32 Kio
        std::vector<std::uint8_t> battery(32U * 1024U);
        for (std::size_t i = 0; i < battery.size(); ++i) battery[i] = static_cast<std::uint8_t>(i * 7U + 3U);
        auto core = ravenemu::make_game_boy_core();
        core->load_rom(rom, battery);

        std::vector<std::uint8_t> out(0x20000);
        const auto read = [&](std::uint32_t address, std::size_t size) {
            return core->read_achievement_memory(address, std::span{out}.first(size));
        };
        check(read(0x0000, 0x8000) == 0x8000, "ROM non exposée en entier");
        check(read(0x8000, 0x2000) == 0x2000, "VRAM non exposée en entier");
        check(read(0xa000, 0x2000) == 0x2000 && out[0] == battery[0] && out[0x1fff] == battery[0x1fff],
              "première banque de RAM de cartouche incorrecte");
        check(read(0xc000, 0x3e00) == 0x3e00, "WRAM et son miroir non exposés");
        check(read(0xfe00, 0x100) == 0xa0, "OAM mal bornée");
        check(read(0xff00, 0x80) == 0x80, "registres E/S non exposés");
        check(read(0xff80, 0x80) == 0x80, "HRAM et IE non exposés");
        check(read(0x10000, 0x6000) == (color ? 0x6000U : 0U), "banques WRAM CGB mal exposées");
        check(read(0x16000, 0x20000) == battery.size() - 0x2000 &&
              out[0] == battery[0x2000] && out[battery.size() - 0x2001] == battery.back(),
              "banques suivantes de RAM de cartouche incorrectes");
        // Une lecture qui franchit une frontière de région rend les mêmes
        // octets que des lectures d'un octet, et s'arrête au même endroit.
        for (const std::uint32_t start : {0x3ff0U, 0x7ff0U, 0x9ff0U, 0xbff0U, 0xdff0U, 0xfdf0U, 0xfe90U, 0xfef0U,
                                          0xff70U, 0xfff0U, 0xfff8U, 0x15ff0U, 0x1dff0U, 0x33ff0U}) {
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
}

// Deux Game Boy reliées préparent leur transfert dans la même image : le
// maître (horloge interne) envoie 0x42, l'esclave (horloge externe) 0x99, et
// chacun range l'octet reçu en 0xC000. Si le maître avançait toute son image
// avant l'esclave, ses huit bits partiraient vers un port pas encore prêt.
std::vector<std::uint8_t> serial_rom(std::uint8_t sent, std::uint8_t control) {
    auto rom = minimal_game_boy_rom();
    const std::array<std::uint8_t, 3> entry{0xc3, 0x50, 0x01};  // jp 0150
    std::copy(entry.begin(), entry.end(), rom.begin() + 0x100);
    const std::array<std::uint8_t, 20> program{
        0x3e, sent, 0xe0, 0x01,      // ld a, octet ; ldh (SB), a
        0x3e, control, 0xe0, 0x02,   // ld a, contrôle ; ldh (SC), a
        0xf0, 0x02, 0xcb, 0x7f,      // attente : ldh a, (SC) ; bit 7, a
        0x20, 0xfa,                  // jr nz, attente
        0xf0, 0x01, 0xea, 0x00,      // ldh a, (SB) ; ld (C000), a
        0xc0, 0x18};                 // … ; jr .
    std::copy(program.begin(), program.end(), rom.begin() + 0x150);
    rom[0x150 + program.size()] = 0xfe;
    return rom;
}

/**
 * Game Genie : l'octet de ROM est remplacé à la lecture, si la comparaison
 * l'accepte. GameShark : l'octet est récrit à chaque image, dans la banque
 * de WRAM ou de RAM de cartouche que choisit le code.
 */
void cheats_test() {
    const auto parse = [](const char* code) { return ravenemu::parse_cheat(ravenemu::Console::game_boy, code); };
    const auto byte_at = [](const ravenemu::Core& core, std::uint32_t address) {
        std::array<std::uint8_t, 1> byte{};
        check(core.read_achievement_memory(address, byte) == 1, "octet illisible");
        return byte[0];
    };
    std::vector<std::int32_t> frame(160 * 144);
    // boucle : LD A,($4A12) ; LD ($C000),A ; LD A,($0200) ; LD ($C001),A ; JR boucle
    auto rom = rom_running(minimal_game_boy_rom(),
                           {0xfa, 0x12, 0x4a, 0xea, 0x00, 0xc0, 0xfa, 0x00, 0x02, 0xea, 0x01, 0xc0, 0x18, 0xf2});
    rom[0x4a12] = 0x05;
    rom[0x0200] = 0x77;
    // 4A12 : 05 devient 3E ; 0200 : 05 deviendrait 99, mais l'octet vaut 77.
    const std::vector<ravenemu::Cheat> cheats{parse("3EA-12B-FAE + 992-00F-FAE"), parse("01AA10C0"),
                                              parse("933300D0")};
    for (const bool color : {false, true}) {
        rom[0x143] = color ? 0x80 : 0x00;
        auto core = ravenemu::make_game_boy_core();
        core->load_rom(rom, {});
        core->set_cheats(cheats);
        core->run_frame(frame, true);
        check(byte_at(*core, 0xc000) == 0x3e, "Game Genie : octet de ROM non remplacé");
        check(byte_at(*core, 0xc001) == 0x77, "Game Genie : comparaison ignorée");
        check(byte_at(*core, 0xc010) == 0xaa, "GameShark : octet non écrit");
        // Banque 3 de la WRAM de la GBC ; une Game Boy n'a que la banque 1.
        check(byte_at(*core, color ? 0x11000 : 0xd000) == 0x33, "GameShark : banque de WRAM");

        const auto state = core->save_state();
        core->reset();
        core->run_frame(frame, true);
        check(byte_at(*core, 0xc000) == 0x3e, "Game Genie perdu à la réinitialisation");
        core->set_cheats({});
        core->load_state(state);
        core->run_frame(frame, true);
        check(byte_at(*core, 0xc000) == 0x05, "Game Genie retiré toujours appliqué");
    }

    // MBC5 + RAM + pile, 4 banques de RAM : le code vise la banque 2, RAM désactivée.
    auto battery_rom = rom_running(minimal_game_boy_rom(), {0x18, 0xfe});
    battery_rom[0x147] = 0x1b;
    battery_rom[0x149] = 0x03;
    auto core = ravenemu::make_game_boy_core();
    core->load_rom(battery_rom, {});
    const std::array<ravenemu::Cheat, 1> sram{parse("025500A0")};
    core->set_cheats(sram);
    core->run_frame(frame, true);
    const auto battery = core->snapshot_battery_ram();
    check(core->battery_ram_dirty() && battery && battery->data.size() == 0x8000 && battery->data[0x4000] == 0x55,
          "GameShark : banque de RAM de cartouche");

    const std::array<ravenemu::Cheat, 1> gba{ravenemu::parse_cheat(ravenemu::Console::game_boy_advance, "32000000 0001")};
    expect_failure<std::invalid_argument>([&] { core->set_cheats(gba); }, "code GBA accepté par la Game Boy");
}

/**
 * Registres laissés par la boot ROM (Pan Docs) : une GBA démarre en GBC avec
 * B = 1, ou, pour un jeu Game Boy, avec B + 1 et les drapeaux de ce INC B.
 */
void agb_hardware_test() {
    // PUSH AF ; POP DE ; LD A,B ; LD ($C000),A ; LD A,E ; LD ($C001),A ; JR $
    auto rom = rom_running(minimal_game_boy_rom(), {0xf5, 0xd1, 0x78, 0xea, 0x00, 0xc0, 0x7b, 0xea, 0x01, 0xc0, 0x18, 0xfe});
    // Jeu Game Boy « Nintendo » dont le titre fait 0x0F : B vaut 0x0F sur GBC.
    rom[0x14b] = 0x01;
    rom[0x134] = 0x0f;
    struct Case {
        ravenemu::gb::HardwareModel model;
        std::uint8_t cgb_flag;
        std::uint8_t b;
        std::uint8_t f;
    };
    for (const auto& [model, cgb_flag, b, f] : {Case{ravenemu::gb::HardwareModel::cgb, 0x80, 0x00, 0x80},
                                                Case{ravenemu::gb::HardwareModel::agb, 0x80, 0x01, 0x00},
                                                Case{ravenemu::gb::HardwareModel::cgb, 0x00, 0x0f, 0x80},
                                                Case{ravenemu::gb::HardwareModel::agb, 0x00, 0x10, 0x20}}) {
        rom[0x143] = cgb_flag;
        auto core = ravenemu::gb::make_core(model);
        core->load_rom(rom, {});
        std::vector<std::int32_t> frame(160 * 144);
        core->run_frame(frame, true);
        std::array<std::uint8_t, 2> registers{};
        check(core->read_achievement_memory(0xc000, registers) == 2, "registres de démarrage illisibles");
        check(registers[0] == b && registers[1] == f, "registres de démarrage GBC/GBA incorrects");
        check(core->framebuffer_format() == ravenemu::FramebufferFormat::argb_8888, "GBA : image en couleurs attendue");
    }
}

/**
 * Un état pris pendant la boot ROM exige la même boot ROM ; pris après elle,
 * il se recharge avec ou sans boot ROM (le joueur peut l'ajouter ou la retirer).
 */
void boot_rom_state_test() {
    const auto boot_rom = [](std::uint8_t marker) {
        std::vector<std::uint8_t> image(0x100);
        // LD A,marker ; LD ($C010),A ; NOP… ; LD A,1 ; LDH ($50),A : le jeu commence en 0100.
        const std::array<std::uint8_t, 5> start{0x3e, marker, 0xea, 0x10, 0xc0};
        const std::array<std::uint8_t, 4> leave{0x3e, 0x01, 0xe0, 0x50};
        std::copy(start.begin(), start.end(), image.begin());
        std::copy(leave.begin(), leave.end(), image.begin() + 0xfc);
        return image;
    };
    const auto rom = rom_running(minimal_game_boy_rom(), {0x18, 0xfe});
    std::vector<std::int32_t> frame(160 * 144);
    const auto make = [&](std::span<const std::uint8_t> boot) {
        auto core = ravenemu::gb::make_core(ravenemu::gb::HardwareModel::dmg, boot);
        core->load_rom(rom, {});
        return core;
    };
    const auto boot = boot_rom(0x42), other = boot_rom(0x24);
    auto with_boot = make(boot);
    const auto during = with_boot->save_state();
    with_boot->run_frame(frame, true);
    std::array<std::uint8_t, 1> marker{};
    check(with_boot->read_achievement_memory(0xc010, marker) == 1 && marker[0] == 0x42, "boot ROM non exécutée");
    const auto after = with_boot->save_state();

    auto without = make({});
    without->load_state(after);
    expect_failure<ravenemu::SaveStateError>([&] { without->load_state(during); },
                                             "état pris pendant la boot ROM accepté sans elle");
    auto other_boot = make(other);
    other_boot->load_state(after);
    expect_failure<ravenemu::SaveStateError>([&] { other_boot->load_state(during); },
                                             "état pris pendant la boot ROM accepté avec une autre");
    with_boot->load_state(during);
    with_boot->load_state(without->save_state());
}

/**
 * Jeu Game Boy sur GBC sans boot ROM : les couleurs que la boot ROM donne à un
 * jeu inconnu, ou des gris ; le réglage s'applique tout de suite, et survit aux
 * états et au reset.
 */
void gbc_compatibility_colors_test() {
    // XOR A ; LDH ($40),A ; LD HL,$8000 ; LD B,8 ; boucle : LD A,$FF ; LD (HL+),A ; XOR A ;
    // LD (HL+),A ; DEC B ; JR NZ ; LD A,$E4 ; LDH ($47),A ; LD A,$91 ; LDH ($40),A ; JR $
    const auto rom = rom_running(minimal_game_boy_rom(),
                                 {0xaf, 0xe0, 0x40, 0x21, 0x00, 0x80, 0x06, 0x08, 0x3e, 0xff, 0x22, 0xaf, 0x22,
                                  0x05, 0x20, 0xf8, 0x3e, 0xe4, 0xe0, 0x47, 0x3e, 0x91, 0xe0, 0x40, 0x18, 0xfe});
    auto core = ravenemu::gb::make_core(ravenemu::gb::HardwareModel::cgb);
    core->load_rom(rom, {});
    std::vector<std::int32_t> frame(160 * 144);
    // Couleur 1 de la palette de fond, sous le premier pixel.
    const auto color = [&] {
        core->run_frame(frame, true);
        core->run_frame(frame, true);
        const auto argb = static_cast<std::uint32_t>(frame[0]);
        return std::array<std::uint32_t, 3>{(argb >> 16) & 0xffU, (argb >> 8) & 0xffU, argb & 0xffU};
    };
    const auto boot = color();
    // Couleur $1BEF de la boot ROM : rouge 15, vert 31, bleu 6.
    check(boot[1] > boot[0] && boot[0] > boot[2], "couleur de la boot ROM GBC absente");
    core->set_gbc_compatibility_colors(ravenemu::GbcCompatibilityColors::grayscale);
    const auto gray = color();
    check(gray[0] == gray[1] && gray[1] == gray[2] && gray[0] != 0 && gray[0] != 0xff, "gris attendu");
    const auto state = core->save_state();
    core->set_gbc_compatibility_colors(ravenemu::GbcCompatibilityColors::automatic);
    core->load_state(state);
    check(color() == boot, "les couleurs d'un état ne suivent pas le réglage");
    core->set_gbc_compatibility_colors(ravenemu::GbcCompatibilityColors::grayscale);
    core->reset();
    check(color() == gray, "réglage des couleurs perdu au reset");
}

void link_cable_test() {
    auto master = ravenemu::make_game_boy_core();
    auto slave = ravenemu::make_game_boy_core();
    master->load_rom(serial_rom(0x42, 0x81), {});
    slave->load_rom(serial_rom(0x99, 0x80), {});
    const std::array<ravenemu::Core*, 2> players{master.get(), slave.get()};
    auto cable = ravenemu::connect_link_cable(players);
    std::vector<std::int32_t> first(160 * 144), second(160 * 144);
    const std::array<std::span<std::int32_t>, 2> frames{first, second};
    for (int frame = 0; frame < 2; ++frame) cable->run_frame(frames, true);
    std::array<std::uint8_t, 1> received{};
    check(master->read_achievement_memory(0xc000, received) == 1 && received[0] == 0x99,
          "câble GB : le maître n'a pas reçu l'octet de l'esclave");
    check(slave->read_achievement_memory(0xc000, received) == 1 && received[0] == 0x42,
          "câble GB : l'esclave n'a pas reçu l'octet du maître");
}

int main() {
    achievement_memory_map_test();
    achievement_memory_follows_the_cpu_test();
    cpu_interrupt_timer_state_validation_test();
    invalid_rom_test();
    game_boy_smoke_test();
    link_cable_test();
    agb_hardware_test();
    gbc_compatibility_colors_test();
    boot_rom_state_test();
    cheats_test();
    game_boy_color_and_battery_test();
    mbc6_large_state_test();
    mbc7_core_contract_test();
    huc1_core_contract_test();
    huc3_core_contract_test();
    return 0;
}
