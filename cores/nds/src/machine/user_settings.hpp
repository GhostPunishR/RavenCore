#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

// Réglages utilisateur que le firmware recopie en RAM à 0x027FFC80 (GBATEK,
// « DS Firmware User Settings »), générés par RavenCore : aucun firmware de
// Nintendo n'est lu. Le pseudo, la langue et la date de naissance seront
// réglables par l'hôte ; ce sont ici des valeurs par défaut.
namespace ravenemu::nds {

struct UserSettings {
    std::u16string_view nickname{u"RavenEmu"};
    unsigned language{2};  // 0 japonais, 1 anglais, 2 français, 3 allemand, 4 italien, 5 espagnol
    unsigned favorite_color{0};
    unsigned birthday_month{1};
    unsigned birthday_day{1};
};

// Les 0x70 octets recopiés en RAM. La calibration du tactile fait
// correspondre 16 pas du convertisseur de 12 bits à un point de l'écran.
inline std::array<std::uint8_t, 0x70> user_settings_block(const UserSettings& settings) {
    std::array<std::uint8_t, 0x70> block{};
    const auto put16 = [&](std::size_t at, unsigned value) {
        block[at] = static_cast<std::uint8_t>(value);
        block[at + 1] = static_cast<std::uint8_t>(value >> 8U);
    };
    put16(0x00, 5);  // version
    block[0x02] = static_cast<std::uint8_t>(settings.favorite_color & 15U);
    block[0x03] = static_cast<std::uint8_t>(settings.birthday_month);
    block[0x04] = static_cast<std::uint8_t>(settings.birthday_day);
    const auto length = std::min<std::size_t>(settings.nickname.size(), 10);
    for (std::size_t index = 0; index < length; ++index) put16(0x06 + index * 2, settings.nickname[index]);
    put16(0x1a, static_cast<unsigned>(length));
    const auto calibrate = [&](std::size_t at, unsigned x, unsigned y) {
        put16(at, x * 16U);
        put16(at + 2, y * 16U);
        block[at + 4] = static_cast<std::uint8_t>(x);
        block[at + 5] = static_cast<std::uint8_t>(y);
    };
    calibrate(0x58, 16, 16);
    calibrate(0x5e, 239, 175);
    // Langue, et réglages déclarés valides (bits 10, 11, 13, 14, 15).
    put16(0x64, (settings.language & 7U) | 0xec00U);
    for (std::size_t at = 0x6c; at < 0x70; ++at) block[at] = 0xff;
    return block;
}

} // namespace ravenemu::nds
