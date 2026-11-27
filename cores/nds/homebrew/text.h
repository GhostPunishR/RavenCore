#ifndef RAVENCORE_HOMEBREW_TEXT_H
#define RAVENCORE_HOMEBREW_TEXT_H

#include <stdint.h>

/* Carte de 32 × 32 entrées d'un fond texte : tuile, puis palette (bits 12-15). */
typedef struct {
    volatile uint16_t* map;
} Screen;

void text_clear(Screen screen);
/* Écrit `text` à la colonne x, rangée y ; les minuscules passent en capitales,
 * les caractères absents de la police en espaces. */
void text_print(Screen screen, unsigned x, unsigned y, unsigned palette, const char* text);
/* Nombre décimal sur `digits` chiffres, complété de zéros. */
void text_number(Screen screen, unsigned x, unsigned y, unsigned palette, uint32_t value, unsigned digits);

#endif
