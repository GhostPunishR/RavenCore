#include "ravenemu/core.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
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

int main() {
    invalid_rom_test();
    game_boy_smoke_test();
    game_boy_color_and_battery_test();
    mbc6_large_state_test();
    mbc7_core_contract_test();
    huc1_core_contract_test();
    huc3_core_contract_test();
    return 0;
}
