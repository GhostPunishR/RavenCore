/*
 * Homebrew de démonstration, côté ARM7. Il répond par la FIFO de l'IPC
 * (GBATEK, « DS Inter Process Communication ») :
 * - à TOUCH_REQUEST, par la position du crayon, lue sur l'écran tactile par
 *   le bus SPI (GBATEK, « DS Touch Screen Controller (TSC) ») ;
 * - à TIME_REQUEST, par l'heure, lue bit par bit sur l'horloge temps réel
 *   (GBATEK, « DS Real-Time Clock (RTC) ») ;
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

/* Quelques microsecondes entre deux fronts de /SCK (GBATEK : au moins 5 µs). */
static void rtc_wait(void) {
    for (volatile unsigned count = 0; count < 40u; ++count) {
    }
}

/* Un octet vers l'horloge, poids faible en tête : donnée posée /SCK bas,
 * lue par la puce au front montant. */
static void rtc_send(uint8_t value) {
    for (unsigned bit = 0; bit < 8u; ++bit) {
        const uint16_t data = (uint16_t)((value >> bit) & 1u);
        RTC = (uint16_t)(0x74u | data); /* CS haut, /SCK bas, lignes en sortie */
        rtc_wait();
        RTC = (uint16_t)(0x76u | data); /* /SCK haut */
        rtc_wait();
    }
}

/* Un octet de l'horloge : elle sort chaque bit au front descendant. */
static uint8_t rtc_receive(void) {
    uint8_t value = 0;
    for (unsigned bit = 0; bit < 8u; ++bit) {
        RTC = 0x64u; /* /SCK bas, donnée en entrée */
        rtc_wait();
        RTC = 0x66u;
        rtc_wait();
        value = (uint8_t)(value | ((RTC & 1u) << bit));
    }
    return value;
}

/* Heure, minute, seconde (registre 6, lecture : commande E6h), en BCD. */
static uint32_t time_message(void) {
    RTC = 0x72u; /* CS bas, /SCK haut */
    rtc_wait();
    RTC = 0x76u; /* CS haut */
    rtc_wait();
    rtc_send(0xe6u);
    const uint32_t hour = rtc_receive() & 0x3fu; /* sans le bit de l'après-midi */
    const uint32_t minute = rtc_receive();
    const uint32_t second = rtc_receive();
    RTC = 0x72u; /* CS bas : fin */
    return TIME_REQUEST | (hour << 16u) | (minute << 8u) | second;
}

int main(void) {
    IPCFIFOCNT = FIFO_ENABLE | FIFO_SEND_CLEAR;
    for (;;) {
        while ((IPCFIFOCNT & FIFO_RECV_EMPTY) != 0) {
        }
        const uint32_t message = IPCFIFORECV;
        if (message == TOUCH_REQUEST) IPCFIFOSEND = touch_message();
        else if (message == TIME_REQUEST) IPCFIFOSEND = time_message();
        else IPCFIFOSEND = message + 1u;
    }
}
