#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Décompressions qui lisent leur source par des fonctions de rappel du jeu
// (GBATEK, « BIOS Decompression Functions », « NDS/DSi Decompression
// Callbacks ») : LZ77 (SWI 12h) et RL (15h) écrits par demi-mots, Huffman
// (13h) par mots. Code ARM écrit pour RavenCore (assemblé par llvm-mc et
// ld.lld, source en commentaire), aucun octet des BIOS de Nintendo. Il ne
// demande que l'ARMv4T et sert aux deux BIOS, placé à `swi_code_offset`.
//
// L'hôte ne prend pas ces trois SWI (bios.hpp) : le CPU entre au vecteur SWI,
// en mode superviseur, IRQ masquées, et ce code appelle les fonctions du jeu
// (ARM ou Thumb, bit 0 de l'adresse) par `mov lr, pc` et `bx`. r3 pointe sur
// leurs adresses : Open, Close (0 : aucune), Get_8bit, Get_16bit (inutilisée),
// Get_32bit. Chacune reçoit dans r0 l'adresse source courante, qui avance de
// 4 après Open et Get_32bit, de 1 après Get_8bit. La SWI rend dans r0 la
// taille décompressée, ou le code d'erreur négatif de Open ou de Close.
// Huffman recopie son arbre dans le tampon pointé par r2 (aligné sur 2,
// 512 octets au plus).
//
// Registres : r4 source, r5 destination, r6 octets restants, r7 fonctions,
// r8 drapeaux ou racine de l'arbre, r9 octet en attente ou nœud, r10 compteur,
// r11 taille. Les fonctions du jeu peuvent écraser r0-r3 et r12.
// Limite : une taille impaire laisse le dernier octet non écrit (demi-mots).
namespace ravenemu::nds {

inline constexpr std::size_t swi_code_offset = 0x100;

inline constexpr std::array<std::uint32_t, 160> callback_decompression_code{
    // .Lswi:
    0xe92d4ff0U,  // stmfd sp!, {r4-r11, lr}
    0xe55ec002U,  // ldrb r12, [lr, #-2]         ; numéro : octet du commentaire, en ARM comme en Thumb
    0xe35c0012U,  // cmp r12, #0x12
    0x028fa0bcU,  // adreq r10, .Llz77
    0x0a000005U,  // beq .Lstart
    0xe35c0015U,  // cmp r12, #0x15
    0x028faf4dU,  // adreq r10, .Lrl
    0x0a000002U,  // beq .Lstart
    0xe35c0013U,  // cmp r12, #0x13
    0x1a000017U,  // bne .Lreturn
    0xe28faf61U,  // adr r10, .Lhuff             ; sinon : retour sans rien faire
    // .Lstart:
    0xe1a04000U,  // mov r4, r0
    0xe1a05001U,  // mov r5, r1
    0xe1a07003U,  // mov r7, r3
    0xe1a08002U,  // mov r8, r2                  ; paramètre de l'utilisateur, ou tampon de l'arbre
    0xe597c000U,  // ldr r12, [r7]
    0xe1a0e00fU,  // mov lr, pc
    0xe12fff1cU,  // bx r12                      ; Open(r0 source, r1 destination, r2 paramètre)
    0xe3500000U,  // cmp r0, #0
    0xba00000dU,  // blt .Lreturn
    0xe2844004U,  // add r4, r4, #4
    0xe1a0b420U,  // mov r11, r0, lsr #8         ; taille décompressée, rendue dans r0
    0xe1a0600bU,  // mov r6, r11
    0xe3a09000U,  // mov r9, #0
    0xe1a0f00aU,  // mov pc, r10
    // .Lclose:
    0xe597c004U,  // ldr r12, [r7, #4]
    0xe35c0000U,  // cmp r12, #0
    0x0a000004U,  // beq .Ldone
    0xe1a00004U,  // mov r0, r4
    0xe1a0e00fU,  // mov lr, pc
    0xe12fff1cU,  // bx r12                      ; Close(r0 source)
    0xe3500000U,  // cmp r0, #0
    0xba000000U,  // blt .Lreturn
    // .Ldone:
    0xe1a0000bU,  // mov r0, r11
    // .Lreturn:
    0xe8bd4ff0U,  // ldmfd sp!, {r4-r11, lr}
    0xe1b0f00eU,  // movs pc, lr                 ; retour au jeu, CPSR repris de SPSR
    // .Lget8:
    0xe92d4000U,  // stmfd sp!, {lr}
    0xe1a00004U,  // mov r0, r4
    0xe597c008U,  // ldr r12, [r7, #8]
    0xe1a0e00fU,  // mov lr, pc
    0xe12fff1cU,  // bx r12                      ; Get_8bit(r0 source)
    0xe2844001U,  // add r4, r4, #1
    0xe20000ffU,  // and r0, r0, #0xff
    0xe8bd8000U,  // ldmfd sp!, {pc}
    // .Lput:
    0xe04bc006U,  // sub r12, r11, r6
    0xe31c0001U,  // tst r12, #1
    0x020090ffU,  // andeq r9, r0, #0xff
    0x11899400U,  // orrne r9, r9, r0, lsl #8
    0x124cc001U,  // subne r12, r12, #1
    0x118590bcU,  // strhne r9, [r5, r12]        ; écriture par demi-mots (VRAM)
    0xe2466001U,  // sub r6, r6, #1
    0xe1a0f00eU,  // mov pc, lr
    // .Llz77:
    0xe3560000U,  // cmp r6, #0
    0x0affffe2U,  // beq .Lclose
    0xebffffecU,  // bl .Lget8
    0xe1a08000U,  // mov r8, r0
    0xe3a0a008U,  // mov r10, #8
    // .Llz77_block:
    0xe3560000U,  // cmp r6, #0
    0x0affffddU,  // beq .Lclose
    0xe25aa001U,  // subs r10, r10, #1
    0x4afffff6U,  // bmi .Llz77
    0xe3180080U,  // tst r8, #0x80
    0xe1a08088U,  // mov r8, r8, lsl #1
    0x1a000002U,  // bne .Llz77_copy
    0xebffffe2U,  // bl .Lget8
    0xebffffe9U,  // bl .Lput
    0xeafffff5U,  // b .Llz77_block
    // .Llz77_copy:
    0xebffffdfU,  // bl .Lget8
    0xe92d0001U,  // stmfd sp!, {r0}
    0xebffffddU,  // bl .Lget8
    0xe8bd0002U,  // ldmfd sp!, {r1}
    0xe201200fU,  // and r2, r1, #0x0f
    0xe1802402U,  // orr r2, r0, r2, lsl #8
    0xe2822001U,  // add r2, r2, #1
    0xe1a01221U,  // mov r1, r1, lsr #4
    0xe2811003U,  // add r1, r1, #3
    // .Llz77_repeat:
    0xe3560000U,  // cmp r6, #0
    0x0affffcaU,  // beq .Lclose
    0xe04bc006U,  // sub r12, r11, r6
    0xe04cc002U,  // sub r12, r12, r2
    0xe7d5000cU,  // ldrb r0, [r5, r12]          ; relit la destination (déplacement de 0 : octet en attente)
    0xebffffd9U,  // bl .Lput
    0xe2511001U,  // subs r1, r1, #1
    0x1afffff7U,  // bne .Llz77_repeat
    0xeaffffe3U,  // b .Llz77_block
    // .Lrl:
    0xe3560000U,  // cmp r6, #0
    0x0affffc1U,  // beq .Lclose
    0xebffffcbU,  // bl .Lget8
    0xe200a07fU,  // and r10, r0, #0x7f
    0xe3100080U,  // tst r0, #0x80
    0x1a000007U,  // bne .Lrl_run
    0xe28aa001U,  // add r10, r10, #1
    // .Lrl_copy:
    0xe3560000U,  // cmp r6, #0
    0x0affffbaU,  // beq .Lclose
    0xebffffc4U,  // bl .Lget8
    0xebffffcbU,  // bl .Lput
    0xe25aa001U,  // subs r10, r10, #1
    0x1afffff9U,  // bne .Lrl_copy
    0xeafffff1U,  // b .Lrl
    // .Lrl_run:
    0xe28aa003U,  // add r10, r10, #3
    0xebffffbeU,  // bl .Lget8
    0xe1a08000U,  // mov r8, r0
    // .Lrl_fill:
    0xe3560000U,  // cmp r6, #0
    0x0affffb0U,  // beq .Lclose
    0xe1a00008U,  // mov r0, r8
    0xebffffc1U,  // bl .Lput
    0xe25aa001U,  // subs r10, r10, #1
    0x1afffff9U,  // bne .Lrl_fill
    0xeaffffe7U,  // b .Lrl
    // .Lhuff:
    0xe1a0a000U,  // mov r10, r0
    0xe1a09008U,  // mov r9, r8
    0xebffffb3U,  // bl .Lget8
    0xe4c90001U,  // strb r0, [r9], #1
    0xe1a06080U,  // mov r6, r0, lsl #1
    0xe2866001U,  // add r6, r6, #1
    // .Lhuff_tree:
    0xebffffafU,  // bl .Lget8
    0xe4c90001U,  // strb r0, [r9], #1
    0xe2566001U,  // subs r6, r6, #1
    0x1afffffbU,  // bne .Lhuff_tree
    0xe1a0600bU,  // mov r6, r11
    0xe2888001U,  // add r8, r8, #1
    0xe1a09008U,  // mov r9, r8
    0xe20a300fU,  // and r3, r10, #15            ; taille des données, en bits
    0xe3a01000U,  // mov r1, #0
    0xe3a02000U,  // mov r2, #0
    // .Lhuff_word:
    0xe3560000U,  // cmp r6, #0
    0xdaffff99U,  // ble .Lclose
    0xe92d000eU,  // stmfd sp!, {r1-r3}
    0xe1a00004U,  // mov r0, r4
    0xe597c010U,  // ldr r12, [r7, #16]
    0xe1a0e00fU,  // mov lr, pc
    0xe12fff1cU,  // bx r12                      ; Get_32bit(r0 source)
    0xe8bd000eU,  // ldmfd sp!, {r1-r3}
    0xe2844004U,  // add r4, r4, #4
    0xe1a0a000U,  // mov r10, r0
    0xe3a0c020U,  // mov r12, #32
    // .Lhuff_bit:
    0xe5d90000U,  // ldrb r0, [r9]
    0xe3c9e001U,  // bic lr, r9, #1
    0xe200903fU,  // and r9, r0, #0x3f
    0xe08e9089U,  // add r9, lr, r9, lsl #1
    0xe2899002U,  // add r9, r9, #2
    0xe1b0a08aU,  // movs r10, r10, lsl #1
    0x22899001U,  // addcs r9, r9, #1
    0x21a00080U,  // movcs r0, r0, lsl #1        ; bit 6 (fin du nœud 1) vu en bit 7
    0xe3100080U,  // tst r0, #0x80
    0x0a00000aU,  // beq .Lhuff_next
    0xe5d90000U,  // ldrb r0, [r9]
    0xe1811210U,  // orr r1, r1, r0, lsl r2
    0xe0822003U,  // add r2, r2, r3
    0xe1a09008U,  // mov r9, r8
    0xe3520020U,  // cmp r2, #32
    0xba000004U,  // blt .Lhuff_next
    0xe4851004U,  // str r1, [r5], #4            ; écriture par mots de 32 bits
    0xe3a01000U,  // mov r1, #0
    0xe3a02000U,  // mov r2, #0
    0xe2566004U,  // subs r6, r6, #4
    0xdaffff7bU,  // ble .Lclose
    // .Lhuff_next:
    0xe25cc001U,  // subs r12, r12, #1
    0x1affffe8U,  // bne .Lhuff_bit
    0xeaffffdcU,  // b .Lhuff_word
};

} // namespace ravenemu::nds
