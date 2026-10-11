#pragma once

#include "save/sram.hpp"
#include "save/flash.hpp"
#include "save/eeprom.hpp"

namespace ravenemu::gba {

// Seul point qui connaît la liste des mémoires de sauvegarde : la chaîne
// d'identification laissée dans la ROM par l'éditeur décide du type, et un
// réglage imposé par le joueur prime au prochain reset.
inline std::unique_ptr<SaveMemory> make_save(GbaSaveType type) {
    switch (type) {
    case GbaSaveType::sram: return std::make_unique<Sram>();
    case GbaSaveType::flash_64k:
    case GbaSaveType::flash_128k: return std::make_unique<Flash>(type);
    case GbaSaveType::eeprom_512:
    case GbaSaveType::eeprom_8k: return std::make_unique<Eeprom>(type);
    case GbaSaveType::none: return nullptr;
    }
    return nullptr;
}
inline GbaSaveType detect_save(std::span<const std::uint8_t> rom) {
    constexpr std::array markers{
        std::pair{std::string_view{"FLASH1M_V"}, GbaSaveType::flash_128k},
        std::pair{std::string_view{"FLASH512_V"}, GbaSaveType::flash_64k},
        std::pair{std::string_view{"FLASH_V"}, GbaSaveType::flash_64k},
        std::pair{std::string_view{"EEPROM_V"}, GbaSaveType::eeprom_512},
        std::pair{std::string_view{"SRAM_F_V"}, GbaSaveType::sram},
        std::pair{std::string_view{"SRAM_V"}, GbaSaveType::sram},
    };
    for (const auto& [marker, type] : markers) {
        for (std::size_t offset = 0; offset + marker.size() <= rom.size(); offset += 4) {
            if (std::equal(marker.begin(), marker.end(), rom.begin() + static_cast<std::ptrdiff_t>(offset))) {
                return type;
            }
        }
    }
    return GbaSaveType::none;
}

} // namespace ravenemu::gba
