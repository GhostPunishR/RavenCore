/* Texte sur un fond en mode texte : une tuile de la police par caractère. */
#include "text.h"

#include "charset.h"

static unsigned glyph_of(char character) {
    if (character >= 'a' && character <= 'z') character = (char)(character - 'a' + 'A');
    const char* charset = CHARSET;
    for (unsigned index = 0; index < GLYPH_COUNT; ++index) {
        if (charset[index] == character) return index;
    }
    return 0;
}

void text_clear(Screen screen) {
    for (unsigned entry = 0; entry < 32u * 32u; ++entry) screen.map[entry] = 0;
}

void text_print(Screen screen, unsigned x, unsigned y, unsigned palette, const char* text) {
    for (; *text != '\0' && x < 32u; ++text, ++x) screen.map[y * 32u + x] = (uint16_t)(glyph_of(*text) | (palette << 12u));
}

/* Division par 10 sans diviseur : multiplication par 0xCCCCCCCD (UMULL). */
static uint32_t div10(uint32_t value) {
    return (uint32_t)(((uint64_t)value * 0xcccccccdu) >> 35u);
}

void text_number(Screen screen, unsigned x, unsigned y, unsigned palette, uint32_t value, unsigned digits) {
    for (unsigned index = digits; index > 0; --index) {
        const uint32_t quotient = div10(value);
        const unsigned digit = (unsigned)(value - quotient * 10u);
        screen.map[y * 32u + x + index - 1u] = (uint16_t)((1u + digit) | (palette << 12u));
        value = quotient;
    }
}
