#include <ravenemu/nds/core.hpp>
#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/interactive_fixture.hpp"
#include "support/firmware_fixture.hpp"

#include <bit>
#include <algorithm>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
std::uint32_t pixel(const std::vector<std::int32_t>& frame, unsigned x, unsigned y) {
    return std::bit_cast<std::uint32_t>(frame[y * 256 + x]);
}
void frames(FrontendCore& core, std::vector<std::int32_t>& frame, unsigned count = 2) {
    while (count--) core.run_frame(frame,true);
}
void frontend() {
    auto firmware_bytes = firmware();
    CoreOptions options; options.firmware = firmware_bytes; options.backup_type = BackupType::fram_8k;
    auto core = make_core(options);
    firmware_bytes.clear(); // la fabrique possède déjà les données
    const auto rom = interactive_cartridge();
    std::vector<std::uint8_t> battery(8192,0xa5);
    std::vector<std::int32_t> frame(256 * 384);
    check(core->console() == ravenemu::Console::nintendo_ds && core->video_spec().width == 256 &&
          core->video_spec().height == 384 && core->supports_video_frame_skipping() && !core->supports_save_states(), "contrat NDS frontend");
    expect_failure<ravenemu::Error>([&] { core->run_frame(frame,true); }, "exécution sans ROM");
    expect_failure<ravenemu::Error>([&] { core->reset(); }, "reset sans ROM");
    core->load_rom(rom,battery); frames(*core,frame);
    check(pixel(frame,120,88) == 0xffff0000 && pixel(frame,0,0) == 0xff00ff00 && pixel(frame,0,192) == 0xff0000ff,
          "écrans, palette RGB555 ou sprite du homebrew");
    core->set_button(ravenemu::Button::right,true); frames(*core,frame);
    core->set_button(ravenemu::Button::right,false);
    check(pixel(frame,120,88) == 0xff00ff00 && pixel(frame,122,88) == 0xffff0000, "déplacement par boutons");
    core->set_key(Key::x,true); frames(*core,frame);
    check(pixel(frame,122,88) == 0xffffffff, "bouton X perdu");
    core->set_key(Key::x,false); core->set_button(ravenemu::Button::a,true); frames(*core,frame);
    core->set_button(ravenemu::Button::a,false);
    check(pixel(frame,120,88) == 0xffff0000, "bouton A ne recentre pas");
    check(core->touch_available(), "étalonnage firmware absent");
    core->set_touch_position(128,96,true); frames(*core,frame);
    check(pixel(frame,0,192) != 0xff0000ff, "contact n'atteint pas le SPI ARM7");
    core->set_touch_position(224,96,true); const auto previous = pixel(frame,0,192); frames(*core,frame);
    check(pixel(frame,0,192) != previous, "coordonnée X n'atteint pas le convertisseur tactile");
    core->set_touch_position(0,0,false); frames(*core,frame);
    check(pixel(frame,0,192) == 0xff0000ff, "relâchement du stylet perdu");
    const auto before = frame; core->run_frame(frame,false); check(frame == before, "frame skipping écrase la sortie");
    expect_failure<std::invalid_argument>([&] { core->run_frame(std::span(frame).first(12),true); }, "buffer trop petit accepté");
    expect_failure<std::invalid_argument>([&] { core->set_touch_position(256,0,true); }, "contact hors écran accepté");
    expect_failure<ravenemu::SaveStateError>([&] { static_cast<void>(core->save_state()); }, "savestate fictive");
    expect_failure<ravenemu::SaveStateError>([&] { core->load_state({}); }, "chargement de savestate fictive");
    std::array<std::int16_t,8192> audio{};
    auto samples = core->read_audio(audio);
    check(samples > 0 && std::all_of(audio.begin(),audio.begin() + samples,[](auto value) { return value == 0; }), "silence initial absent");
    core->set_button(ravenemu::Button::b,true); core->run_frame(frame,false);
    samples = core->read_audio(audio);
    check(samples >= 1094 && samples <= 1098 && std::any_of(audio.begin(),audio.begin() + samples,[](auto value) { return value != 0; }),
          "B ne produit pas le PSG pendant le saut vidéo");
    for (std::size_t i = 0; i < samples; i += 2) check(audio[i] == audio[i + 1], "PSG centré non stéréo");
    core->set_button(ravenemu::Button::b,false); frames(*core,frame);
    core->read_audio(audio); core->run_frame(frame,false); samples = core->read_audio(audio);
    check(samples > 0 && std::all_of(audio.begin(),audio.begin() + samples,[](auto value) { return value == 0; }), "B relâché reste sonore");
    check(core->has_battery_ram() && core->snapshot_battery_ram()->data == battery, "import sauvegarde");
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { core->load_rom(invalid,{}); }, "ROM invalide acceptée");
    expect_failure<ravenemu::Error>([&] { core->load_rom(rom,std::span(battery).first(10)); }, "sauvegarde invalide acceptée");
    frames(*core,frame); check(core->snapshot_battery_ram()->data == battery, "chargement refusé perd la sauvegarde");
    core->reset(); frames(*core,frame);
    check(pixel(frame,120,88) == 0xffff0000 && core->snapshot_battery_ram()->data == battery && core->touch_available(),
          "reset ne conserve pas sauvegarde et firmware");
    auto bare = make_core(); bare->load_rom(rom,{});
    check(!bare->touch_available() && !bare->has_battery_ram() && !bare->snapshot_battery_ram(), "périphériques fictifs sans configuration");
    expect_failure<ravenemu::Error>([&] { bare->set_touch_position(0,0,true); }, "contact sans étalonnage accepté");
    bare->set_touch_position(0,0,false);
}

void reset_pending_backup() {
    Machine machine; const auto rom = cartridge_image(); machine.load_cartridge(Cartridge{rom});
    machine.load_backup(BackupType::eeprom_512);
    auto& memory = machine.memory(); constexpr auto cpu = Processor::arm9;
    const auto byte = [&](std::uint8_t value, bool last) {
        memory.write16(cpu,0x040001a0,last ? 0xa000 : 0xa040); memory.write8(cpu,0x040001a2,value); machine.advance_cycles(64);
    };
    byte(6,true); byte(2,false); byte(0x12,false); byte(0x5a,true);
    check(memory.backup().busy(), "opération interne EEPROM absente");
    machine.reset(); check(memory.backup().busy(), "reset perd une écriture acceptée");
    machine.advance_cycles(167570);
    check(machine.snapshot_backup().data[0x12] == 0x5a && machine.backup_dirty(), "EEPROM perdue au redémarrage");
}

void presentation_and_battery() {
    for (const auto power : std::array<std::uint8_t,3>{5,9,0x40}) {
        Program a9;
        const auto write = [&](std::uint32_t address, std::uint32_t value) { a9.load(1,value); a9.store(1,address); };
        write(0x04000304,0x8203); write(0x04000000,0x10000); write(0x04001000,0x10000);
        write(0x05000000,0x3e0); write(0x05000400,0x7c00);
        Program a7; a7.load(0,0x040001c0); a7.load(2,0x8800); a7.code.push_back(0xe1c020b0);
        a7.spi_byte(0); a7.load(2,0x8000); a7.code.push_back(0xe1c020b0); a7.spi_byte(power);
        auto core = make_core(); core->load_rom(cartridge_image(a9.finish(),a7.finish()),{});
        std::vector<std::int32_t> frame(256 * 384); frames(*core,frame,1);
        check(pixel(frame,0,0) == (power == 9 ? 0xff00ff00 : 0xff000000) &&
              pixel(frame,0,192) == (power == 5 ? 0xff0000ff : 0xff000000), "présentation des rétroéclairages ou extinction");
        check(core->powered_off() == (power == 0x40), "état d'extinction frontend");
        core->reset(); check(!core->powered_off(), "reset ne rallume pas le frontend");
    }
    Program p; p.load(0,0x040001a0);
    const auto byte = [&](std::uint8_t value, bool last) {
        p.load(5,last ? 0xa000 : 0xa040); p.code.push_back(0xe1c050b0); p.spi_byte(value);
    };
    byte(6,true); byte(2,false); byte(0,false); byte(0x12,false); byte(0x5a,true);
    const auto rom = cartridge_image(p.finish());
    CoreOptions options; options.backup_type = BackupType::fram_8k;
    auto core = make_core(options); core->load_rom(rom,{});
    std::vector<std::int32_t> frame(256 * 384); frames(*core,frame,1);
    const auto saved = core->snapshot_battery_ram();
    check(saved && saved->data[0x12] == 0x5a && core->battery_ram_dirty(), "écriture FRAM du homebrew non exportée");
    core->reset(); check(core->snapshot_battery_ram()->data == saved->data && core->battery_ram_dirty(), "reset nettoie la génération dirty");
    core->acknowledge_battery_ram_saved(saved->generation); check(!core->battery_ram_dirty(), "acquittement FRAM refusé");
    core->load_rom(rom,{}); frames(*core,frame,1); core->acknowledge_battery_ram_saved(saved->generation);
    check(core->battery_ram_dirty(), "ancien acquittement nettoie une nouvelle cartouche");
}
} // namespace

int main() { frontend(); reset_pending_backup(); presentation_and_battery(); }
