/*
 * Homebrew de démonstration, côté ARM7. Il répond par la FIFO de l'IPC
 * (GBATEK, « DS Inter Process Communication ») :
 * - à TOUCH_REQUEST, par la position du crayon, lue sur l'écran tactile par
 *   le bus SPI (GBATEK, « DS Touch Screen Controller (TSC) ») ;
 * - à toute autre valeur n, par n + 1.
 */
#include "ds.h"

/* Un octet sur le SPI ; SPICNT choisit l'appareil et garde la puce. */
static uint8_t spi_exchange(uint16_t control, uint8_t value) {
    SPICNT = control;
    SPIDATA = value;
    while ((SPICNT & 0x80u) != 0) {
    }
    return (uint8_t)SPIDATA;
}

/* Conversion de 12 bits en mode différentiel : commande, puis deux octets de
 * réponse (un bit nul, 12 bits, trois zéros). Écran tactile à 2 MHz. */
static uint32_t touch_read(uint8_t command) {
    spi_exchange(0x8a01u, command);
    const uint32_t high = spi_exchange(0x8a01u, 0);
    const uint32_t low = spi_exchange(0x8201u, 0); /* dernier octet : puce relâchée */
    return ((high << 8u) | low) >> 3u;
}

static uint32_t touch_message(void) {
    if ((EXTKEYIN & 0x40u) != 0) return TOUCH_REPLY; /* crayon levé */
    return TOUCH_REPLY | 0x01000000u | (touch_read(0xd0u) << 12u) | touch_read(0x90u); /* X, puis Y */
}

int main(void) {
    IPCFIFOCNT = FIFO_ENABLE | FIFO_SEND_CLEAR;
    for (;;) {
        while ((IPCFIFOCNT & FIFO_RECV_EMPTY) != 0) {
        }
        const uint32_t message = IPCFIFORECV;
        IPCFIFOSEND = message == TOUCH_REQUEST ? touch_message() : message + 1u;
    }
}
