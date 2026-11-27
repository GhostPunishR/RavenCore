// Construit la ROM .nds du homebrew à partir des binaires de l'ARM9 et de
// l'ARM7, et d'un fichier de données facultatif placé à 0x8000, première
// adresse que la commande B7 lit : en-tête de GBATEK (« DS Cartridge
// Header »), sans logo ni zone sécurisée, puis vérification par l'analyse
// d'en-tête du cœur.
#include "cartridge/header.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace {

using ravenemu::nds::CartridgeError;
using ravenemu::nds::crc16;
using ravenemu::nds::parse_header;

constexpr std::uint32_t arm9_address = 0x0200'0000U;
constexpr std::uint32_t arm7_address = 0x037f'8000U;
constexpr std::uint32_t arm9_offset = 0x200;
constexpr std::uint32_t data_offset = 0x8000;

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::uint32_t align(std::size_t value, std::uint32_t step) {
    return static_cast<std::uint32_t>((value + step - 1) / step * step);
}

void put32(std::vector<std::uint8_t>& rom, std::size_t at, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) rom[at + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
}

void put_text(std::vector<std::uint8_t>& rom, std::size_t at, std::string_view text) {
    for (const auto character : text) rom[at++] = static_cast<std::uint8_t>(character);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fputs("usage : nds_make_rom arm9.bin arm7.bin sortie.nds [données]\n", stderr);
        return 2;
    }
    const auto arm9 = read_file(argv[1]);
    const auto arm7 = read_file(argv[2]);
    const auto data = argc == 5 ? read_file(argv[4]) : std::vector<std::uint8_t>{};
    if (arm9.empty() || arm7.empty()) {
        std::fputs("nds_make_rom : binaire ARM9 ou ARM7 vide ou illisible\n", stderr);
        return 1;
    }
    const auto arm7_offset = align(arm9_offset + arm9.size(), 0x200);
    if (!data.empty() && arm7_offset + arm7.size() > data_offset) {
        std::fputs("nds_make_rom : les binaires débordent sur les données à 0x8000\n", stderr);
        return 1;
    }
    auto size = align(arm7_offset + arm7.size(), 0x200);
    if (!data.empty()) size = align(data_offset + data.size(), 0x200);
    std::vector<std::uint8_t> rom(size);
    put_text(rom, 0x000, "RAVENCORE");  // titre, 12 caractères au plus
    put_text(rom, 0x00c, "####");       // code de jeu des homebrews
    put_text(rom, 0x010, "00");         // fabricant
    std::uint8_t capacity = 0;          // puce de 128 Kio << capacity
    while ((std::size_t{128 * 1024} << capacity) < rom.size()) ++capacity;
    rom[0x014] = capacity;
    const auto place = [&](std::size_t field, std::uint32_t offset, std::uint32_t address,
                           const std::vector<std::uint8_t>& code) {
        put32(rom, field, offset);
        put32(rom, field + 4, address);  // point d'entrée
        put32(rom, field + 8, address);  // adresse de chargement
        put32(rom, field + 12, static_cast<std::uint32_t>(code.size()));
        std::copy(code.begin(), code.end(), rom.begin() + offset);
    };
    place(0x020, arm9_offset, arm9_address, arm9);
    place(0x030, arm7_offset, arm7_address, arm7);
    std::copy(data.begin(), data.end(), rom.begin() + data_offset);
    // ROMCTRL des commandes normales et de la zone sécurisée : valeurs par
    // défaut des puces MROM (GBATEK).
    put32(rom, 0x060, 0x0058'6000U);
    put32(rom, 0x064, 0x0018'08f8U);
    put32(rom, 0x080, static_cast<std::uint32_t>(rom.size()));  // taille utilisée
    put32(rom, 0x084, 0x4000);                                   // taille de l'en-tête
    const auto crc = crc16(std::span<const std::uint8_t>{rom}.first(0x15e));
    rom[0x15e] = static_cast<std::uint8_t>(crc);
    rom[0x15f] = static_cast<std::uint8_t>(crc >> 8U);
    try {
        static_cast<void>(parse_header(rom));
    } catch (const CartridgeError& error) {
        std::fprintf(stderr, "nds_make_rom : en-tête refusé par le cœur : %s\n", error.what());
        return 1;
    }
    std::ofstream out{argv[3], std::ios::binary};
    out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    return out ? 0 : 1;
}
