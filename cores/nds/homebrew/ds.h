/*
 * Registres du DS utilisés par le homebrew de démonstration (GBATEK, « DS I/O
 * Maps »). Écrit pour RavenCore, sans libnds.
 */
#ifndef RAVENCORE_HOMEBREW_DS_H
#define RAVENCORE_HOMEBREW_DS_H

#include <stdint.h>

#define REG32(address) (*(volatile uint32_t*)(address))
#define REG16(address) (*(volatile uint16_t*)(address))
#define REG8(address) (*(volatile uint8_t*)(address))

/* Moteurs 2D : A à 0x04000000, B à 0x04001000. */
#define DISPCNT(engine) REG32(0x04000000u + (engine) * 0x1000u)
#define BG0CNT(engine) REG16(0x04000008u + (engine) * 0x1000u)
#define DISPSTAT REG16(0x04000004u)
#define DISPSTAT_VBLANK_IRQ 0x0008u

/* Mémoire vidéo : palettes standard des fonds, VRAM des fonds de A et de B. */
#define BG_PALETTE(engine) ((volatile uint16_t*)(0x05000000u + (engine) * 0x400u))
#define BG_VRAM(engine) ((volatile uint16_t*)((engine) == 0 ? 0x06000000u : 0x06200000u))
#define VRAMCNT_A REG8(0x04000240u)
#define VRAMCNT_C REG8(0x04000242u)

/* POWCNT1 : écran, moteurs A et B ; bit 15, moteur A sur l'écran du haut. */
#define POWCNT1 REG16(0x04000304u)

#define KEYINPUT REG16(0x04000130u) /* touches : bit à 0 = appuyée */
#define EXTKEYIN REG16(0x04000136u) /* ARM7 : bit 6 à 0 = crayon posé */

/* Bus SPI de l'ARM7 : contrôle, puis octet échangé. */
#define SPICNT REG16(0x040001c0u)
#define SPIDATA REG16(0x040001c2u)

/* Diviseur de l'ARM9 : contrôle, numérateur, dénominateur, quotient. */
#define DIVCNT REG32(0x04000280u)
#define DIV_NUMER REG32(0x04000290u)
#define DIV_DENOM REG32(0x04000298u)
#define DIV_RESULT REG32(0x040002a0u)

/* Messages de l'IPC entre les deux processeurs du homebrew. */
#define TOUCH_REQUEST 0x40000000u /* ARM9 : position du crayon ? */
#define TOUCH_REPLY 0x80000000u   /* ARM7 : bit 24 crayon posé, X en bits 12-23, Y en 0-11 */

/* Minuteurs : compteur et rechargement, puis contrôle. */
#define TM0CNT_L REG16(0x04000100u)
#define TM0CNT_H REG16(0x04000102u)
#define TIMER_ENABLE 0x0080u
#define TIMER_IRQ 0x0040u
#define TIMER_DIV_1024 0x0003u

/* Interruptions. */
#define IME REG32(0x04000208u)
#define IE REG32(0x04000210u)
#define IF REG32(0x04000214u)
#define IRQ_VBLANK 0x0001u
#define IRQ_TIMER0 0x0008u
#define IRQ_CARD 0x00080000u /* fin de transfert de la cartouche */

/* DMA 0 : source, destination, contrôle. */
#define DMA0SAD REG32(0x040000b0u)
#define DMA0DAD REG32(0x040000b4u)
#define DMA0CNT REG32(0x040000b8u)

/* Bus de la cartouche : AUXSPICNT, ROMCTRL, commande de 8 octets (premier
 * octet à l'adresse basse), mots reçus. */
#define AUXSPICNT REG16(0x040001a0u)
#define AUXSPIDATA REG16(0x040001a2u)
#define ROMCTRL REG32(0x040001a4u)
#define CARD_COMMAND ((volatile uint32_t*)0x040001a8u)
#define CARD_DATA 0x04100010u

/* FIFO de l'IPC : contrôle, envoi, réception. */
#define IPCFIFOCNT REG32(0x04000184u)
#define IPCFIFOSEND REG32(0x04000188u)
#define IPCFIFORECV REG32(0x04100000u)
#define FIFO_ENABLE 0x8000u
#define FIFO_SEND_CLEAR 0x0008u
#define FIFO_RECV_EMPTY 0x0100u

#endif
