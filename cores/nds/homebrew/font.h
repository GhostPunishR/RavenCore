#ifndef RAVENCORE_HOMEBREW_FONT_H
#define RAVENCORE_HOMEBREW_FONT_H

#include <stdint.h>

/* Écrit les tuiles de la police à partir de `tiles` (VRAM, par demi-mots). */
void font_load(volatile uint16_t* tiles);

#endif
