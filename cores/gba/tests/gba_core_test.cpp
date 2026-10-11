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


void invalid_rom_test() {
    auto rom = minimal_gba_rom();
    rom[0xb2] = 0;
    auto gba = ravenemu::make_gba_core(std::nullopt);
    expect_failure<ravenemu::RomLoadError>(
        [&] { gba->load_rom(rom, {}); },
        "une ROM GBA sans marqueur a été acceptée"
    );
}

} // namespace

int main() {
    invalid_rom_test();
    gba_smoke_test();
    gba_save_detection_test();
    return 0;
}
