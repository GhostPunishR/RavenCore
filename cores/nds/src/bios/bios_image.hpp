#pragma once

#include "bios/callback_decompression.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Contenu des BIOS de RavenCore : des vecteurs d'exception écrits pour ce
// dépôt (assemblés par llvm-mc, source en commentaire), aucun octet des BIOS
// de Nintendo. Les fonctions SWI ne passent pas par là : l'hôte les exécute
// (bios.hpp), sauf les décompressions par fonctions de rappel, dont le code
// suit les vecteurs (callback_decompression.hpp). Le vecteur d'IRQ appelle le
// gestionnaire du jeu, rangé à
// DTCM + 0x3FFC (ARM9, ARM ou Thumb) ou à 0x0380FFFC (ARM7, ARM seul),
// comme le décrit GBATEK (« DS Interrupts »).
namespace ravenemu::nds {

inline constexpr std::size_t arm9_bios_size = 4 * 1024;   // vu à 0xFFFF0000
inline constexpr std::size_t arm7_bios_size = 16 * 1024;  // vu à 0x00000000

inline constexpr std::array<std::uint32_t, 17> arm9_bios_code{
    0xeafffffeU,  // b .            reset
    0xeafffffeU,  // b .            instruction indéfinie
    0xea00003cU,  // b .Lswi        SWI : l'hôte, sauf 12h, 13h, 15h (à 0x100)
    0xeafffffeU,  // b .            abandon de préchargement
    0xeafffffeU,  // b .            abandon de données
    0xeafffffeU,  // b .            réservé
    0xea000000U,  // b .Lirq
    0xeafffffeU,  // b .            FIQ
    // .Lirq:
    0xe92d500fU,  // stmfd sp!, {r0-r3, r12, lr}
    0xee190f11U,  // mrc p15, 0, r0, c9, c1, 0   ; région de la DTCM
    0xe1a00620U,  // mov r0, r0, lsr #12
    0xe1a00600U,  // mov r0, r0, lsl #12
    0xe2800901U,  // add r0, r0, #0x4000
    0xe28fe000U,  // add lr, pc, #0                ; retour sur ldmfd
    0xe510f004U,  // ldr pc, [r0, #-4]             ; [DTCM + 0x3FFC]
    0xe8bd500fU,  // ldmfd sp!, {r0-r3, r12, lr}
    0xe25ef004U,  // subs pc, lr, #4
};

inline constexpr std::array<std::uint32_t, 14> arm7_bios_code{
    0xeafffffeU,  // b .
    0xeafffffeU,  // b .
    0xea00003cU,  // b .Lswi        SWI : l'hôte, sauf 12h, 13h, 15h (à 0x100)
    0xeafffffeU,  // b .
    0xeafffffeU,  // b .
    0xeafffffeU,  // b .
    0xea000000U,  // b .Lirq
    0xeafffffeU,  // b .
    // .Lirq:
    0xe92d500fU,  // stmfd sp!, {r0-r3, r12, lr}
    0xe3a00301U,  // mov r0, #0x04000000
    0xe28fe000U,  // add lr, pc, #0                ; retour sur ldmfd
    0xe510f004U,  // ldr pc, [r0, #-4]             ; [0x03FFFFFC], miroir de 0x0380FFFC
    0xe8bd500fU,  // ldmfd sp!, {r0-r3, r12, lr}
    0xe25ef004U,  // subs pc, lr, #4
};

template <std::size_t Words>
std::vector<std::uint8_t> bios_image(const std::array<std::uint32_t, Words>& code, std::size_t size) {
    static_assert(Words * 4 <= swi_code_offset, "les vecteurs débordent sur le code des SWI");
    std::vector<std::uint8_t> image(size);
    const auto place = [&](std::size_t offset, std::uint32_t word) {
        for (unsigned byte = 0; byte < 4; ++byte) image[offset + byte] = static_cast<std::uint8_t>(word >> (8U * byte));
    };
    for (std::size_t index = 0; index < Words; ++index) place(index * 4, code[index]);
    for (std::size_t index = 0; index < callback_decompression_code.size(); ++index)
        place(swi_code_offset + index * 4, callback_decompression_code[index]);
    return image;
}

} // namespace ravenemu::nds
