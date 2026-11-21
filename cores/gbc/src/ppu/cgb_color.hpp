#pragma once

#include <cstdint>

namespace ravenemu::cgb {

/**
 * Couleur CGB 15 bits (BGR555, RAM couleur) vers le pixel ARGB de l'image.
 */
// Chaque canal 5 bits monte en haut de son octet, puis ses 3 bits forts
// sont recopiés dessous ((c << 3) | (c >> 2)), les trois canaux d'un coup :
// la boucle de décodage d'une image entière se vectorise ainsi.
inline constexpr std::int32_t bgr555_to_argb(int value) noexcept {
    const auto bgr = static_cast<std::uint32_t>(value);
    const std::uint32_t top = ((bgr & 0x1fU) << 19U) | ((bgr & 0x3e0U) << 6U) | ((bgr & 0x7c00U) >> 7U);
    return static_cast<std::int32_t>(0xff000000U | top | ((top >> 5U) & 0x070707U));
}

} // namespace ravenemu::cgb
