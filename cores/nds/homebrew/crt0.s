@ Démarrage commun à l'ARM9 et à l'ARM7 (ARMv4T) : met à zéro la section
@ .bss, puis appelle main. Les piles sont celles du démarrage direct.
    .section .text.start, "ax"
    .arm
    .global _start
_start:
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:
    cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    bl main
2:
    b 2b
    .ltorg
