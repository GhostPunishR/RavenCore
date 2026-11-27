/*
 * memset et memcpy, que le compilateur peut appeler même en mode autonome
 * (-ffreestanding) ; écrites pour le homebrew.
 */
#include <stddef.h>

void* memset(void* destination, int value, size_t count) {
    unsigned char* bytes = destination;
    while (count-- > 0) *bytes++ = (unsigned char)value;
    return destination;
}

void* memcpy(void* destination, const void* source, size_t count) {
    unsigned char* to = destination;
    const unsigned char* from = source;
    while (count-- > 0) *to++ = *from++;
    return destination;
}
