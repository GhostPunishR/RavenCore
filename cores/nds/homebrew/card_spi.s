@ Un octet sur le SPI de la cartouche, en mode sauvegarde (GBATEK, « DS
@ Cartridge I/O Ports ») : r0 = octet envoyé, r1 = 1 pour relâcher la puce
@ après lui ; rend l'octet reçu. Comme le SDK, la commande et l'adresse partent
@ d'une fonction (card_spi_command), les données d'une autre (card_spi_data) :
@ le cœur reconnaît ainsi la largeur d'adresse de la puce. En assembleur, pour
@ que chaque fonction n'ait qu'une écriture d'AUXSPIDATA, quel que soit le
@ compilateur (une boucle déroulée ou une fonction dupliquée en aurait plusieurs).
    .text
    .arm
    .macro spi_byte
    mov r2, #0x04000000
    add r2, r2, #0x1a0          @ AUXSPICNT
    mov r3, #0xa000             @ slot activé, mode SPI
    cmp r1, #0
    orreq r3, r3, #0x40         @ garder la puce sélectionnée
    strh r3, [r2]
    strh r0, [r2, #2]           @ AUXSPIDATA : l'octet part
1:
    ldrh r3, [r2]
    tst r3, #0x80               @ occupé ?
    bne 1b
    ldrh r0, [r2, #2]
    and r0, r0, #0xff
    bx lr
    .endm

    .global card_spi_command
    .type card_spi_command, %function
card_spi_command:
    spi_byte

    .global card_spi_data
    .type card_spi_data, %function
card_spi_data:
    spi_byte
