/*
 * Homebrew de démonstration du cœur NDS de RavenCore, côté ARM9. Écrit pour
 * RavenCore d'après GBATEK, sans libnds. Il affiche du texte sur les deux
 * écrans (moteurs 2D en mode texte) et montre en direct :
 * - les images comptées par VBlankIntrWait du BIOS et un gestionnaire d'IRQ ;
 * - les secondes, par l'IRQ d'un minuteur ;
 * - la réponse de l'ARM7 par la FIFO de l'IPC (41, puis 42) ;
 * - le pseudo des réglages utilisateur du firmware ;
 * - les touches appuyées ;
 * - un message lu dans la ROM par le bus de la cartouche (commande B7, DMA
 *   « cartouche », IRQ de fin de transfert attendue par IntrWait) ;
 * - le nombre de démarrages, gardé dans l'EEPROM de sauvegarde, dont le cœur
 *   reconnaît seul le type.
 */
#include "ds.h"
#include "font.h"
#include "text.h"

/* Mots du BIOS en haut de la DTCM (0x027C0000) : drapeaux d'IRQ attendus par
 * IntrWait, puis adresse du gestionnaire (GBATEK, « DS Interrupts »). */
#define BIOS_IRQ_FLAGS REG32(0x027c3ff8u)
#define BIOS_IRQ_HANDLER REG32(0x027c3ffcu)

/* Réglages utilisateur du firmware recopiés en RAM principale (GBATEK, « DS
 * Firmware User Settings ») : pseudo en UTF-16, puis sa longueur. */
#define NICKNAME ((volatile const uint16_t*)0x027ffc86u)
#define NICKNAME_LENGTH REG16(0x027ffc9au)

/* En-tête de la cartouche recopié à 0x027FFE00 : ROMCTRL des commandes
 * normales (060h). */
#define HEADER_ROMCTRL REG32(0x027ffe60u)

/* Message à 0x8000 dans la ROM (cartridge_message.txt), lu en un bloc. */
enum { MESSAGE_ADDRESS = 0x8000, BLOCK_WORDS = 0x200 / 4 };
static uint32_t card_block[BLOCK_WORDS];

/* Couleurs de RavenEmu en BGR555 : fond #0B0C12, texte #F5F2FF, accent
 * #A78BFA, texte atténué #AAA2BC. */
enum { BACKDROP = 0x0821, TEXT = 0x7fde, ACCENT = 0x7e34, MUTED = 0x5e95 };
enum { PALETTE_TEXT = 0, PALETTE_ACCENT = 1, PALETTE_MUTED = 2 };

/* Carte du fond au bloc de 2 Kio n° 31, tuiles au bloc 0. */
enum { MAP_BLOCK = 31 };

/* 33 513 982 Hz / 1024 : 32 728 coups par seconde. */
enum { TICKS_PER_SECOND = 32728 };

static volatile uint32_t seconds;

static void irq_handler(void) {
    const uint32_t flags = IF;
    IF = flags;
    if ((flags & IRQ_TIMER0) != 0) ++seconds;
    BIOS_IRQ_FLAGS |= flags;
}

static void vblank_wait(void) {
    __asm__ volatile("swi 0x050000" ::: "r0", "r1", "r2", "r3", "r12", "memory");
}

/* IntrWait (SWI 04h) : r0 = 1 oublie les drapeaux déjà levés. */
static void interrupt_wait(uint32_t mask) {
    register uint32_t r0 __asm__("r0") = 1;
    register uint32_t r1 __asm__("r1") = mask;
    __asm__ volatile("swi 0x040000" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "memory");
}

/* Lignes de 32 octets du cache de données de l'ARM946E-S : nettoyées et
 * invalidées avant que la DMA écrive, invalidées après (CP15, c7). */
static void cache_clean_invalidate(const void* start, unsigned bytes) {
    for (uintptr_t line = (uintptr_t)start & ~31u; line < (uintptr_t)start + bytes; line += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(line) : "memory");
}
static void cache_invalidate(const void* start, unsigned bytes) {
    for (uintptr_t line = (uintptr_t)start & ~31u; line < (uintptr_t)start + bytes; line += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(line) : "memory");
}

/* Lit 0x200 octets de la cartouche à `address` par la commande B7 : la DMA 0,
 * en mode « cartouche », prend chaque mot reçu, et l'IRQ de fin de transfert
 * réveille IntrWait (GBATEK, « DS Cartridge I/O Ports »). */
static void card_read_block(uint32_t address, uint32_t* destination) {
    cache_clean_invalidate(destination, BLOCK_WORDS * 4u);
    DMA0SAD = CARD_DATA;
    DMA0DAD = (uint32_t)(uintptr_t)destination;
    /* activée, départ « cartouche », mots de 32 bits, répétée, source fixe, 1 mot */
    DMA0CNT = 0x80000000u | (5u << 27u) | (1u << 26u) | (1u << 25u) | (2u << 23u) | 1u;
    AUXSPICNT = 0xc000u; /* slot activé, IRQ de fin de transfert */
    CARD_COMMAND[0] = 0xb7u | ((address >> 24u) << 8u) | (((address >> 16u) & 0xffu) << 16u) |
                      (((address >> 8u) & 0xffu) << 24u);
    CARD_COMMAND[1] = address & 0xffu;
    /* réglages de l'en-tête, blocs de 0x200 octets, départ */
    ROMCTRL = (HEADER_ROMCTRL & 0x187f7fffu) | 0xa1000000u;
    interrupt_wait(IRQ_CARD);
    DMA0CNT = 0;
    cache_invalidate(destination, BLOCK_WORDS * 4u);
}

/* Le SPI de la cartouche, en mode sauvegarde : `last` relâche la puce après
 * l'octet (AUXSPICNT bit 6 à 0, GBATEK, « DS Cartridge I/O Ports »). Comme
 * le SDK, la commande et l'adresse partent d'une fonction, les données d'une
 * autre : le cœur en déduit la largeur d'adresse de la puce. */
static void spi_select(int last) {
    AUXSPICNT = (uint16_t)(0xa000u | (last ? 0u : 0x40u)); /* slot activé, mode SPI */
}
static void spi_wait(void) {
    while ((AUXSPICNT & 0x80u) != 0) {
    }
}
__attribute__((noinline)) static void spi_command(const uint8_t* bytes, unsigned count, int last) {
    for (unsigned index = 0; index < count; ++index) {
        spi_select(last && index + 1u == count);
        AUXSPIDATA = bytes[index];
        spi_wait();
    }
}
__attribute__((noinline)) static uint8_t spi_data(uint8_t value, int last) {
    spi_select(last);
    AUXSPIDATA = value;
    spi_wait();
    return (uint8_t)AUXSPIDATA;
}

/* Compteur de démarrages dans une EEPROM à adresse de 2 octets : « R » puis le
 * compte, à l'adresse 0 (GBATEK, « DS Cartridge Backup »). */
static unsigned count_boot(void) {
    static const uint8_t read[] = {0x03, 0x00, 0x00};  /* RD, adresse 0000h */
    static const uint8_t enable[] = {0x06};            /* WREN */
    static const uint8_t write[] = {0x02, 0x00, 0x00}; /* WR, adresse 0000h */
    static const uint8_t status[] = {0x05};            /* RDSR */
    spi_command(read, 3, 0);
    const uint8_t marker = spi_data(0, 0);
    const uint8_t stored = spi_data(0, 1);
    const unsigned count = (marker == 'R' ? stored : 0u) + 1u;
    spi_command(enable, 1, 1);
    spi_command(write, 3, 0);
    spi_data('R', 0);
    spi_data((uint8_t)count, 1);
    spi_command(status, 1, 0); /* jusqu'à la fin de l'écriture (WIP) */
    while ((spi_data(0, 0) & 1u) != 0) {
    }
    spi_data(0, 1);
    return count & 0xffu;
}

static void show_card_message(Screen screen, unsigned y) {
    card_read_block(MESSAGE_ADDRESS, card_block);
    char message[31];
    const char* bytes = (const char*)card_block;
    unsigned count = 0;
    while (count < 30 && bytes[count] != '\0' && bytes[count] != '\n') {
        message[count] = bytes[count];
        ++count;
    }
    message[count] = '\0';
    text_print(screen, 1, y, PALETTE_TEXT, message);
}

static Screen screen_of(unsigned engine) {
    const Screen screen = {BG_VRAM(engine) + MAP_BLOCK * 0x800u / 2u};
    return screen;
}

static void setup_engine(unsigned engine) {
    volatile uint16_t* palette = BG_PALETTE(engine);
    palette[0] = BACKDROP;
    palette[PALETTE_TEXT * 16 + 1] = TEXT;
    palette[PALETTE_ACCENT * 16 + 1] = ACCENT;
    palette[PALETTE_MUTED * 16 + 1] = MUTED;
    font_load(BG_VRAM(engine));
    BG0CNT(engine) = MAP_BLOCK << 8u;
    DISPCNT(engine) = 0x00010100u; /* affichage normal, mode 0, BG0 */
    text_clear(screen_of(engine));
}

static unsigned length(const char* text) {
    unsigned count = 0;
    while (text[count] != '\0') ++count;
    return count;
}

/* Pseudo du firmware, en ASCII : les autres caractères deviennent des espaces. */
static void show_nickname(Screen screen, unsigned x, unsigned y) {
    char name[11];
    unsigned count = NICKNAME_LENGTH;
    if (count > 10) count = 10;
    for (unsigned index = 0; index < count; ++index) {
        const uint16_t character = NICKNAME[index];
        name[index] = character < 0x80u ? (char)character : ' ';
    }
    name[count] = '\0';
    text_print(screen, x, y, PALETTE_TEXT, name);
}

static void show_keys(Screen screen, unsigned y) {
    static const char* const names[10] = {"A", "B", "SELECT", "START", "DROITE", "GAUCHE", "HAUT", "BAS", "R", "L"};
    const unsigned pressed = ~(unsigned)KEYINPUT & 0x3ffu;
    for (unsigned x = 0; x < 32; ++x) screen.map[y * 32u + x] = 0;
    if (pressed == 0) {
        text_print(screen, 1, y, PALETTE_MUTED, "AUCUNE");
        return;
    }
    unsigned x = 1;
    for (unsigned key = 0; key < 10; ++key) {
        if ((pressed & (1u << key)) == 0) continue;
        text_print(screen, x, y, PALETTE_TEXT, names[key]);
        x += length(names[key]) + 1u;
    }
}

static void draw_labels(Screen top, Screen bottom) {
    text_print(top, 1, 1, PALETTE_ACCENT, "RAVENCORE - COEUR NDS");
    text_print(top, 1, 3, PALETTE_TEXT, "BONJOUR DEPUIS LE DS !");
    text_print(top, 1, 6, PALETTE_MUTED, "IMAGES");
    text_print(top, 1, 7, PALETTE_MUTED, "SECONDES");
    text_print(top, 1, 8, PALETTE_MUTED, "ARM7 IPC");
    text_print(top, 12, 8, PALETTE_TEXT, "..");
    text_print(top, 1, 9, PALETTE_MUTED, "PSEUDO");
    text_print(top, 1, 10, PALETTE_MUTED, "DEMARRAGES");
    show_nickname(top, 12, 9);
    text_print(top, 1, 12, PALETTE_MUTED, "TOUCHES");
    text_print(top, 1, 15, PALETTE_MUTED, "CARTOUCHE");
    text_print(bottom, 1, 1, PALETTE_ACCENT, "ECRAN DU BAS - MOTEUR B");
    text_print(bottom, 1, 3, PALETTE_TEXT, "DEUX PROCESSEURS ARM");
    text_print(bottom, 1, 4, PALETTE_TEXT, "DEUX MOTEURS 2D");
    text_print(bottom, 1, 6, PALETTE_MUTED, "ECRIT POUR RAVENCORE");
}

int main(void) {
    POWCNT1 = 0x8203u;  /* écran, moteurs A et B ; A sur l'écran du haut */
    VRAMCNT_A = 0x81u;  /* banque A : fonds du moteur A */
    VRAMCNT_C = 0x84u;  /* banque C : fonds du moteur B */
    setup_engine(0);
    setup_engine(1);
    const Screen top = screen_of(0);
    const Screen bottom = screen_of(1);
    draw_labels(top, bottom);

    IPCFIFOCNT = FIFO_ENABLE | FIFO_SEND_CLEAR;
    IPCFIFOSEND = 41;

    TM0CNT_L = (uint16_t)(0x10000u - TICKS_PER_SECOND);
    TM0CNT_H = TIMER_ENABLE | TIMER_IRQ | TIMER_DIV_1024;

    BIOS_IRQ_HANDLER = (uint32_t)(uintptr_t)irq_handler;
    DISPSTAT = DISPSTAT_VBLANK_IRQ;
    IE = IRQ_VBLANK | IRQ_TIMER0 | IRQ_CARD;
    IME = 1;
    show_card_message(top, 16);
    text_number(top, 12, 10, PALETTE_TEXT, count_boot(), 3);

    uint32_t frames = 0;
    for (;;) {
        vblank_wait();
        ++frames;
        text_number(top, 12, 6, PALETTE_TEXT, frames, 6);
        text_number(top, 12, 7, PALETTE_TEXT, seconds, 4);
        if ((IPCFIFOCNT & FIFO_RECV_EMPTY) == 0) text_number(top, 12, 8, PALETTE_TEXT, IPCFIFORECV, 2);
        show_keys(top, 13);
    }
}
