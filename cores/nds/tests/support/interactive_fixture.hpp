#pragma once

#include "program_fixture.hpp"

namespace ravenemu::nds::test_support {

// Homebrew original sans BIOS : croix = déplacer le carré ; A = recentrer ;
// X = carré blanc ; contact tactile = couleur de l'écran inférieur selon ADC X.
inline std::vector<std::uint8_t> interactive_cartridge() {
    Program p;
    const auto write = [&](std::uint32_t address, std::uint32_t value) { p.load(1,value); p.store(1,address); };
    const auto branch = [](Program& code, unsigned target, std::uint32_t condition = 0xea000000) {
        const auto here = static_cast<unsigned>(code.code.size());
        code.code.push_back(condition | ((target - here - 2U) & 0xffffffU));
    };
    write(0x04000304,0x8203); write(0x04000000,0x11010); write(0x04001000,0x10000);
    write(0x05000000,0x03e0); write(0x05000400,0x7c00);
    p.load(0,0x04000241); p.code.insert(p.code.end(),{0xe3a01082,0xe5c01000}); // VRAM B = OBJ A
    p.load(0,0x06400000); p.load(1,0x11111111); p.code.push_back(0xe3a02008);
    auto loop = static_cast<unsigned>(p.code.size());
    p.code.insert(p.code.end(),{0xe4801004,0xe2522001}); branch(p,loop,0x1a000000);
    p.load(0,0x07000000); p.load(1,0x200); p.code.push_back(0xe3a02080);
    loop = static_cast<unsigned>(p.code.size());
    p.code.insert(p.code.end(),{0xe4801008,0xe2522001}); branch(p,loop,0x1a000000);
    p.code.insert(p.code.end(),{0xe3a06078,0xe3a07058}); // x=120,y=88
    const auto frame = static_cast<unsigned>(p.code.size());
    p.load(0,0x04000006);
    loop = static_cast<unsigned>(p.code.size());
    p.code.insert(p.code.end(),{0xe1d010b0,0xe35100c0}); branch(p,loop,0x3a000000); // attendre VBlank
    loop = static_cast<unsigned>(p.code.size());
    p.code.insert(p.code.end(),{0xe1d010b0,0xe35100c0}); branch(p,loop,0x2a000000); // puis ligne zéro
    p.load(0,0x04000130); p.code.push_back(0xe1d010b0);
    p.code.insert(p.code.end(),{0xe3110010,0x02866001,0xe3110020,0x02466001,
        0xe3110080,0x02877001,0xe3110040,0x02477001,0xe20660ff,0xe20770ff,
        0xe3110001,0x03a06078,0x03a07058});
    p.load(0,0x07000000); p.code.insert(p.code.end(),{0xe1c070b0,0xe1c060b2});
    p.load(0,0x02001010); p.code.push_back(0xe5901000);
    p.load(2,0x7fff); p.code.insert(p.code.end(),{0xe3110001,0x13a0201f}); // X : blanc, sinon rouge
    p.load(0,0x05000202); p.code.push_back(0xe1c020b0);
    p.load(0,0x02001014); p.code.push_back(0xe5902000);
    p.load(3,0x7c00); p.code.insert(p.code.end(),{0xe3110040,0x11a02003}); // sans stylet : bleu
    p.load(0,0x05000400); p.code.push_back(0xe1c020b0);
    branch(p,frame);

    Program a7;
    a7.load(0,0x04000136); a7.code.push_back(0xe1d020b0); a7.store(2,0x02001010);
    a7.load(0,0x040001c0); a7.load(5,0x8a01); a7.code.push_back(0xe1c050b0);
    a7.spi_byte(0xd0); a7.spi_byte(0,2);
    a7.load(5,0x8201); a7.code.push_back(0xe1c050b0); a7.spi_byte(0,3);
    a7.code.insert(a7.code.end(),{0xe1a02402,0xe1822003,0xe1a021a2}); a7.store(2,0x02001014);
    branch(a7,0);
    auto rom = cartridge_image(p.finish(),a7.finish());
    constexpr char title[] = "RAVEN DEMO";
    for (unsigned i = 0; i < sizeof(title) - 1; ++i) rom[i] = static_cast<std::uint8_t>(title[i]);
    return rom;
}

} // namespace ravenemu::nds::test_support
