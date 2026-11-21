// Tests du PPU GB/GBC : État laissé par la boot ROM sans boot ROM (boot/post_boot_vram.hpp).

#include "ppu_test_support.hpp"

namespace ravenemu::cgb::testing {

/**
 * Sans boot ROM, la VRAM reçoit ce que la boot ROM y laisse : le logo de
 * l'en-tête, tuiles 1 à 24, et le glyphe « ® », tuile 25 ; la carte BG le
 * montre sur Game Boy. Rien en GBC native, où la boot ROM n'a pas été mesurée.
 */
void post_boot_logo_test() {
    auto rom = ravenemu::testing::minimal_game_boy_rom();
    rom[0x0104] = 0xce; // premier octet du logo : quartets 1100 et 1110
    constexpr std::array<std::uint8_t, 8> glyph{0x3c, 0x42, 0xb9, 0xa5, 0xb9, 0xa5, 0x42, 0x3c};
    for (const auto mode : {gbc::HardwareMode::dmg, gbc::HardwareMode::cgb_compatibility,
                            gbc::HardwareMode::cgb_native}) {
        auto image_bytes = rom;
        if (mode == gbc::HardwareMode::cgb_native) image_bytes[0x143] = 0x80;
        auto image = std::make_shared<const std::vector<std::uint8_t>>(image_bytes);
        Machine machine(image, [] { return std::int64_t{0}; }, mode);
        const auto& vram = machine.ppu.vram;
        const bool logo = mode != gbc::HardwareMode::cgb_native;
        // 1100 -> 11110000 sur deux lignes, puis 1110 -> 11111100 sur deux.
        check((vram[0x10] == 0xf0 && vram[0x12] == 0xf0 && vram[0x14] == 0xfc && vram[0x16] == 0xfc &&
               vram[0x11] == 0) == logo, "logo de l'en-tête mal recopié en VRAM");
        bool glyph_found = true;
        for (std::size_t row = 0; row < glyph.size(); ++row) {
            glyph_found = glyph_found && vram[0x190 + row * 2] == glyph[row] && vram[0x191 + row * 2] == 0;
        }
        check(glyph_found == logo, "glyphe « ® » absent de la tuile 25");
        const bool map = mode == gbc::HardwareMode::dmg;
        check((vram[0x1904] == 1 && vram[0x190f] == 12 && vram[0x1910] == 0x19 &&
               vram[0x1924] == 13 && vram[0x192f] == 24) == map, "carte BG du logo incorrecte");
    }
}

/**
 * Sans boot ROM, le PPU du DMG reprend où la boot ROM le laisse : en V-Blank,
 * ligne 153, à 60 points de la ligne 0 (GBMicrotest, poweron_stat et
 * poweron_ly). La GBC garde le début de la ligne 0, faute de mesure.
 */
void dmg_post_boot_ppu_phase_test() {
    auto image = std::make_shared<const std::vector<std::uint8_t>>(ravenemu::testing::minimal_game_boy_rom());
    Machine dmg(image, [] { return std::int64_t{0}; }, gbc::HardwareMode::dmg);
    check(dmg.ppu.mode() == Ppu::mode_vblank && dmg.ppu.ly() == 0 && dmg.ppu.line_dot() == 396,
          "DMG : PPU pas en fin de ligne 153 à l'entrée en $0100");
    check((dmg.ppu.read_stat() & 0x07) == 0x05, "DMG : STAT ne lit pas V-Blank et LY=LYC au démarrage");
    check(dmg.ppu.obp0() == 0xff && dmg.ppu.obp1() == 0xff, "DMG : OBP0 et OBP1 ne lisent pas $FF au démarrage");
    for (int mcycle = 0; mcycle < 15; ++mcycle) dmg.bus.tick_mcycle();
    check(dmg.ppu.ly() == 0 && dmg.ppu.line_dot() == 0 && dmg.ppu.mode() == Ppu::mode_oam,
          "DMG : la ligne 0 ne commence pas 60 points après $0100");

    Machine cgb(image, [] { return std::int64_t{0}; }, gbc::HardwareMode::cgb_compatibility);
    check(cgb.ppu.ly() == 0 && cgb.ppu.line_dot() == 0 && cgb.ppu.mode() == Ppu::mode_oam,
          "GBC : position du PPU au démarrage modifiée sans mesure");
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    post_boot_logo_test();
    dmg_post_boot_ppu_phase_test();
    return 0;
}
