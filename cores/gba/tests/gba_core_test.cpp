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


void audio_state_continuation_test() {
    auto rom = minimal_gba_rom();
    // Configure PSG once, then loop without further IO writes.
    const std::array<std::uint32_t, 13> code{
        0xe59f0020, 0xe59f1020, 0xe5801080, 0xe3a01080, 0xe5801084,
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
    audio_state_continuation_test();
    invalid_rom_test();
    gba_smoke_test();
    gba_save_detection_test();
    return 0;
}
