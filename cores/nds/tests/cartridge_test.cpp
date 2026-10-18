#include <ravenemu/nds/cartridge.hpp>
#include <ravenemu/core.hpp>
#include "check.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {

void put32(std::vector<std::uint8_t>& rom, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) rom[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}

std::vector<std::uint8_t> synthetic_rom() {
    std::vector<std::uint8_t> rom(0x208);
    const std::array<std::uint8_t, 16> title{'R', 'A', 'V', 'E', 'N', ' ', 'N', 'D', 'S', 0, 0, 0,
                                              'T', 'E', 'S', 'T'};
    std::copy(title.begin(), title.end(), rom.begin());
    put32(rom, 0x20, 0x200);
    put32(rom, 0x24, 0x02000000);
    put32(rom, 0x28, 0x02000000);
    put32(rom, 0x2c, 4);
    put32(rom, 0x30, 0x204);
    put32(rom, 0x34, 0x03800001); // entrée Thumb
    put32(rom, 0x38, 0x03800000);
    put32(rom, 0x3c, 4);
    // ARM : b . ; Thumb : b . et nop. Ces instructions ne sont pas encore exécutées.
    const std::array<std::uint8_t, 8> code{0xfe, 0xff, 0xff, 0xea, 0xfe, 0xe7, 0xc0, 0x46};
    std::copy(code.begin(), code.end(), rom.begin() + 0x200);
    return rom;
}

void valid_image_test() {
    auto rom = synthetic_rom();
    const ravenemu::nds::Cartridge cartridge{rom};
    check(cartridge.header().title == "RAVEN NDS", "titre NDS");
    check(cartridge.header().game_code == "TEST", "code jeu NDS");
    check(cartridge.header().arm9.load_address == 0x02000000, "adresse ARM9 little-endian");
    check(cartridge.header().arm7.entry_address == 0x03800001, "entrée Thumb ARM7");
    check(cartridge.arm9_image().size() == 4 && cartridge.arm9_image()[3] == 0xea, "image ARM9");
    check(cartridge.arm7_image().size() == 4 && cartridge.arm7_image()[1] == 0xe7, "image ARM7");
    rom.assign(1, 0);
    check(cartridge.arm9_image()[0] == 0xfe, "la cartouche doit posséder ses octets");
    const auto copy = cartridge;
    check(copy.arm7_image()[1] == 0xe7, "copie de cartouche");
    auto source = cartridge;
    const auto moved = std::move(source);
    check(moved.arm9_image()[3] == 0xea, "déplacement de cartouche");
    // Un objet déplacé reste consultable, même si son contenu n'est pas garanti.
    static_cast<void>(source.arm9_image());
    static_cast<void>(source.arm7_image());
}

void invalid_image_test() {
    for (const auto length : {0U, 0x1fU, 0x1ffU}) {
        const std::vector<std::uint8_t> rom(length);
        expect_failure<ravenemu::RomLoadError>([&] { ravenemu::nds::Cartridge cartridge{rom}; },
                                             "en-tête tronqué accepté");
    }
    // Chaque descripteur doit subir les mêmes vérifications.
    for (const auto base : {0x20U, 0x30U}) {
        const std::array<std::array<std::uint32_t, 2>, 7> mutations{{
            {0, 0x100}, {0, 0x209}, {0, 0xfffffff0}, {12, 0},
            {12, 0xffffffff}, {4, 0x04000000}, {8, 0xfffffffe},
        }};
        for (const auto mutation : mutations) {
            auto rom = synthetic_rom();
            put32(rom, base + mutation[0], mutation[1]);
            expect_failure<ravenemu::RomLoadError>([&] { ravenemu::nds::Cartridge cartridge{rom}; },
                                                 "descripteur invalide accepté");
        }
    }
    auto rom = synthetic_rom();
    rom.pop_back();
    expect_failure<ravenemu::RomLoadError>([&] { ravenemu::nds::Cartridge cartridge{rom}; },
                                         "programme ARM7 tronqué accepté");
    rom = synthetic_rom();
    rom[0x12] = 2;
    expect_failure<ravenemu::RomLoadError>([&] { ravenemu::nds::Cartridge cartridge{rom}; },
                                         "DSi accepté silencieusement");
}

} // namespace

int main() {
    valid_image_test();
    invalid_image_test();
}
