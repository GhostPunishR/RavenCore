#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ravenemu::cgb {

enum class OamCorruption : std::uint8_t { write, read, read_with_increment };

/**
 * Bug de corruption de l'OAM du DMG (Pan Docs, « OAM Corruption Bug »).
 * Un accès CPU à $FE00-$FEFF, ou l'incrémentation 16 bits d'un registre
 * qui y pointe, pendant le mode 2 abîme la ligne de 8 octets (deux objets)
 * que le PPU lit pendant ce M-cycle, `row`. Les opérations sont bit à bit : on
 * les applique octet par octet aux mots 16 bits de l'OAM. Le CGB n'a pas ce
 * défaut, même en mode de compatibilité ; le PPU ne l'appelle que sur DMG.
 */
inline void corrupt_oam_row(std::array<std::uint8_t, 0xa0>& oam, int row, OamCorruption kind) noexcept {
    if (row <= 0 || row >= 20) return; // la ligne des objets 0 et 1 est épargnée
    const auto at = [&oam](int line, int offset) -> std::uint8_t& {
        return oam[static_cast<std::size_t>(line * 8 + offset)];
    };
    const auto copy_row = [&](int from, int to) {
        for (int offset = 0; offset < 8; ++offset) at(to, offset) = at(from, offset);
    };
    if (kind == OamCorruption::read_with_increment) {
        // Lecture et écriture dans le même M-cycle : la ligne précédente
        // est d'abord corrompue puis recopiée, sauf pour les quatre
        // premières lignes et la dernière ; une corruption de lecture suit.
        if (row >= 4 && row < 19) {
            for (int offset = 0; offset < 2; ++offset) {
                const int a = at(row - 2, offset);
                const int b = at(row - 1, offset);
                const int c = at(row, offset);
                const int d = at(row - 1, offset + 4);
                at(row - 1, offset) = static_cast<std::uint8_t>((b & (a | c | d)) | (a & c & d));
            }
            copy_row(row - 1, row);
            copy_row(row - 1, row - 2);
        }
        kind = OamCorruption::read;
    }
    for (int offset = 0; offset < 2; ++offset) {
        const int a = at(row, offset);
        const int b = at(row - 1, offset);
        const int c = at(row - 1, offset + 4);
        at(row, offset) = static_cast<std::uint8_t>(
            kind == OamCorruption::write ? ((a ^ c) & (b ^ c)) ^ c : b | (a & c));
    }
    for (int offset = 2; offset < 8; ++offset) at(row, offset) = at(row - 1, offset);
}

} // namespace ravenemu::cgb
