#pragma once

#include <array>
#include <cstdint>

// IMA-ADPCM du DS (GBATEK, « DS Sound Notes ») : en-tête de 32 bits (valeur
// PCM16 initiale en bits 0-15, index en bits 16-22), puis des quartets, le
// premier dans les bits bas de chaque octet. La table des pas est générée par
// la formule en entiers de 32 bits que donne GBATEK, qui la reproduit
// exactement (X = 776D2h, puis X += X / 10 ; entrées 3, 4 et 88 corrigées).
namespace ravenemu::nds::adpcm {

inline constexpr std::array<std::int32_t, 89> step_table = [] {
    std::array<std::int32_t, 89> table{};
    std::uint32_t x = 0x0007'76d2U;
    for (auto& entry : table) {
        entry = static_cast<std::int32_t>(x >> 16U);
        x += x / 10U;
    }
    table[3] = 0x000a;
    table[4] = 0x000b;
    table[88] = 0x7fff;
    return table;
}();

inline constexpr std::array<int, 8> index_table{-1, -1, -1, -1, 2, 4, 6, 8};

struct State {
    std::int32_t pcm{};
    int index{};
};

// Un quartet, avec l'erreur d'arrondi et le bornage que décrit GBATEK : le
// pas est la somme de table/8, table/4, table/2 et table selon les bits 0-2 ;
// +7FFFh au plus, -7FFFh au moins.
constexpr void decode(State& state, unsigned nibble) noexcept {
    const auto step = step_table[static_cast<std::size_t>(state.index)];
    auto diff = step / 8;
    if ((nibble & 1U) != 0) diff += step / 4;
    if ((nibble & 2U) != 0) diff += step / 2;
    if ((nibble & 4U) != 0) diff += step;
    if ((nibble & 8U) == 0) state.pcm = state.pcm + diff > 0x7fff ? 0x7fff : state.pcm + diff;
    else state.pcm = state.pcm - diff < -0x7fff ? -0x7fff : state.pcm - diff;
    state.index += index_table[nibble & 7U];
    state.index = state.index < 0 ? 0 : (state.index > 88 ? 88 : state.index);
}

} // namespace ravenemu::nds::adpcm
