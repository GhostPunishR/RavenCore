/*
 * Homebrew de démonstration du cœur NDS de RavenCore, côté ARM9. Écrit pour
 * RavenCore d'après GBATEK, sans libnds. Il affiche du texte sur les deux
 * écrans (moteurs 2D en mode texte) et montre en direct :
 * - les images comptées par VBlankIntrWait du BIOS et un gestionnaire d'IRQ ;
 * - les secondes, par l'IRQ d'un minuteur ;
 * - la réponse de l'ARM7 par la FIFO de l'IPC (41, puis 42) ;
 * - le pseudo des réglages utilisateur du firmware ;
 * - les touches appuyées.
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
    show_nickname(top, 12, 9);
    text_print(top, 1, 12, PALETTE_MUTED, "TOUCHES");
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
    IE = IRQ_VBLANK | IRQ_TIMER0;
    IME = 1;

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
