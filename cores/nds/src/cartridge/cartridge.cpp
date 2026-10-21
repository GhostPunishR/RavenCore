#include <ravenemu/nds/cartridge.hpp>
#include <ravenemu/core.hpp>

#include <limits>

namespace ravenemu::nds {
namespace {

std::uint32_t read32(std::span<const std::uint8_t> rom, std::size_t offset) {
    std::uint32_t result{};
    for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(rom[offset + i]) << (8U * i);
    return result;
}

Executable executable(std::span<const std::uint8_t> rom, std::size_t offset) {
    const Executable image{read32(rom, offset), read32(rom, offset + 4),
                           read32(rom, offset + 8), read32(rom, offset + 12)};
    // Soustraction après contrôle de l'offset : aucune addition 32 bits ne
    // doit pouvoir faire accepter une plage débordante fournie par la ROM.
    if (image.size == 0 || image.rom_offset < 0x200 || image.rom_offset > rom.size() ||
        image.size > rom.size() - image.rom_offset)
        throw RomLoadError("NDS : exécutable absent ou hors de la ROM");
    if (image.size - 1U > std::numeric_limits<std::uint32_t>::max() - image.load_address)
        throw RomLoadError("NDS : plage de chargement débordante");
    // Le bit 0 d'une entrée Thumb ne fait pas partie de l'adresse.
    const auto entry = image.entry_address & ~std::uint32_t{1};
    if (entry < image.load_address || entry - image.load_address >= image.size)
        throw RomLoadError("NDS : point d'entrée hors de l'exécutable");
    return image;
}

std::string text(std::span<const std::uint8_t> bytes) {
    std::string result;
    for (const auto byte : bytes) {
        if (byte == 0) break;
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

} // namespace

Cartridge::Cartridge(std::span<const std::uint8_t> rom) {
    if (rom.size() < 0x200) throw RomLoadError("NDS : en-tête tronqué");
    if (rom[0x12] != 0) throw RomLoadError("NDS : images DSi non prises en charge");
    header_ = {text(rom.first(12)), text(rom.subspan(12, 4)), executable(rom, 0x20), executable(rom, 0x30)};
    rom_ = std::make_shared<const std::vector<std::uint8_t>>(rom.begin(), rom.end());
}

std::span<const std::uint8_t> Cartridge::arm9_image() const noexcept {
    if (!rom_) return {}; // objet déplacé
    return rom().subspan(header_.arm9.rom_offset, header_.arm9.size);
}

std::span<const std::uint8_t> Cartridge::arm7_image() const noexcept {
    if (!rom_) return {};
    return rom().subspan(header_.arm7.rom_offset, header_.arm7.size);
}

} // namespace ravenemu::nds
