#include "spi/firmware_image.hpp"

#include "cartridge/header.hpp"

#include <algorithm>
#include <span>

namespace ravenemu::nds {

namespace {

constexpr std::size_t firmware_size = 256 * 1024;
constexpr std::size_t settings_offset = 0x3fe00;

void put16(std::vector<std::uint8_t>& image, std::size_t at, unsigned value) {
    image[at] = static_cast<std::uint8_t>(value);
    image[at + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_header(std::vector<std::uint8_t>& image) {
    std::fill(image.begin(), image.begin() + 0x2a, std::uint8_t{0});
    const char id[] = {'R', 'A', 'V', 'N'};
    std::copy(std::begin(id), std::end(id), image.begin() + 0x08);
    put16(image, 0x14, 2U << 12U);  // taille de la puce / 128 Kio
    image[0x1d] = 0xff;              // Nintendo DS
    image[0x1e] = image[0x1f] = 0xff;
    put16(image, 0x20, static_cast<unsigned>(settings_offset / 8));
    put16(image, 0x22, 0x7ec0);  // valeurs habituelles des DS (GBATEK)
    put16(image, 0x24, 0x7e40);
    image[0x28] = image[0x29] = 0xff;
}

void write_wifi(std::vector<std::uint8_t>& image) {
    constexpr unsigned length = 0x138;
    std::fill(image.begin() + 0x2c, image.begin() + 0x2c + length, std::uint8_t{0});
    put16(image, 0x2c, length);
    const std::uint8_t mac[] = {0x02, 0x52, 0x41, 0x56, 0x45, 0x4e};  // adresse locale (bit 1 du premier octet)
    std::copy(std::begin(mac), std::end(mac), image.begin() + 0x36);
    put16(image, 0x3c, 0x3ffe);  // canaux 1 à 13
    put16(image, 0x3e, 0xffff);
    image[0x40] = 0x02;  // puce radio de type 2
    image[0x41] = 0x18;  // 24 bits par entrée RF
    image[0x42] = 0x0c;  // 12 entrées RF
    image[0x43] = 0x01;
    put16(image, 0x2a, crc16(std::span<const std::uint8_t>{image}.subspan(0x2c, length), 0));
}

void write_settings(std::vector<std::uint8_t>& image, std::size_t at, const UserSettings& settings, unsigned counter) {
    const auto block = user_settings_block(settings);
    std::copy(block.begin(), block.end(), image.begin() + static_cast<std::ptrdiff_t>(at));
    put16(image, at + 0x70, counter);
    put16(image, at + 0x72, crc16(std::span<const std::uint8_t>{block}));
}

} // namespace

std::vector<std::uint8_t> firmware_image(const UserSettings& settings) {
    std::vector<std::uint8_t> image(firmware_size, 0xff);
    write_header(image);
    write_wifi(image);
    write_settings(image, settings_offset, settings, 0);
    write_settings(image, settings_offset + 0x100, settings, 1);
    return image;
}

} // namespace ravenemu::nds
