#pragma once

// Aides partagées des tests du PPU GB/GBC.

#include "ppu/ppu.hpp"
#include <algorithm>
#include <array>
#include "check.hpp"
#include "machine/machine.hpp"
#include "synthetic_roms.hpp"
#include <ravenemu/gbc/core.hpp>
#include <ravenemu/binary_io.hpp>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

inline int enter_and_measure_mode3(Ppu& ppu) {
    while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
    const int start = ppu.line_dot();
    while (ppu.mode() == Ppu::mode_transfer) ppu.tick(1);
    return ppu.line_dot() - start;
}

inline int measure_mode3_after_mode2(Ppu& ppu, int remaining_mode2_dots) {
    ppu.tick(remaining_mode2_dots);
    while (ppu.mode() != Ppu::mode_transfer) ppu.tick(1);
    check(ppu.mode() == Ppu::mode_transfer, "PPU absent du mode 3 après le scan OAM");
    const int start = ppu.line_dot();
    while (ppu.mode() == Ppu::mode_transfer) ppu.tick(1);
    return ppu.line_dot() - start;
}

inline std::vector<std::uint8_t> snapshot(const Ppu& ppu) {
    detail::BinaryWriter writer;
    ppu.save(writer);
    return std::move(writer).take();
}

inline void check_snapshot_equal(const std::vector<std::uint8_t>& expected,
                          const std::vector<std::uint8_t>& actual,
                          std::string_view message) {
    check(expected.size() == actual.size(), "taille d'état PPU différente après restauration");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] != actual[index]) {
            throw std::runtime_error(std::string{message} + " (octet " + std::to_string(index) + ")");
        }
    }
}

/**
 * PPU nu, BG seul (LCDC $91, SCX = SCY = 0) : la tuile 0 a sa ligne 0 en
 * couleur 0 et toutes les autres en couleur 3, la tuile 1 est en couleur 3.
 * `change` est appelé une fois, ligne `line`, quand le mode 3 a sorti
 * `x` pixels ; la fonction rend la couleur affichée au milieu de la tuile
 * `column`.
 */
template <typename Change>
inline std::int32_t fetcher_probe(gbc::HardwareMode mode, int line, int x, int column, Change change) {
    InterruptController interrupts;
    Ppu ppu(interrupts, mode);
    ppu.write_lcdc(0x11);
    ppu.set_bgp(0xe4);
    ppu.write_bcps(0x80);
    for (const int color : {0x0000, 0x001f, 0x03e0, 0x7c00}) {
        ppu.write_bcpd(color & 0xff);
        ppu.write_bcpd(color >> 8);
    }
    for (int offset = 2; offset < 32; ++offset) ppu.vram[static_cast<std::size_t>(offset)] = 0xff;
    ppu.write_lcdc(0x91);
    while (!(ppu.ly() == line && ppu.mode() == Ppu::mode_transfer && ppu.transfer_x() >= x)) ppu.tick(1);
    change(ppu);
    while (ppu.ly() != Ppu::height) ppu.tick(1);
    return ppu.completed_frame[static_cast<std::size_t>(line * Ppu::width + column * 8 + 3)];
}

} // namespace ravenemu::cgb::testing
