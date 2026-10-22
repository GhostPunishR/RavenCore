#include <ravenemu/nds/machine.hpp>

namespace ravenemu::nds {
namespace {
std::uint16_t read16(std::span<const std::uint8_t> bytes, unsigned offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<unsigned>(bytes[offset + 1]) << 8U));
}
}

void Machine::prepare_boot_memory(Memory& memory, const Cartridge& cartridge) {
    memory.complete_boot();
    const auto rom = cartridge.rom();
    constexpr auto cpu = Processor::arm7;
    // Informations post-firmware de GBATEK, NDS BIOS RAM Usage. Les CRC sont
    // ceux déclarés par les images, pas une authentification BIOS/KEY1.
    for (const auto base : {0x027ff800U,0x027ffc00U}) {
        memory.write32(cpu,base,memory.gamecard_chip_id());
        memory.write32(cpu,base + 4,memory.gamecard_chip_id());
        memory.write16(cpu,base + 8,read16(rom,0x15e));
        memory.write16(cpu,base + 0xa,read16(rom,0x6c));
    }
    memory.write16(cpu,0x027ff810,0xffff); // boot handler terminé
    memory.write32(cpu,0x027ff860,cartridge.header().arm7.load_address);
    memory.write32(cpu,0x027ff880,7); // message ARM9 vers ARM7 au boot cartouche
    memory.write32(cpu,0x027ff884,6); // tâche de boot ARM7 terminée
    memory.write16(cpu,0x027ffc40,1); // démarrage cartouche normal
    for (unsigned i = 0; i < 0x170; ++i) memory.write8(cpu,0x027ffe00U + i,rom[i]);

    const auto& firmware = memory.firmware();
    const auto& settings = firmware.settings();
    memory.write32(cpu,0x027ff864,settings ? 0 : 1);
    if (!firmware.bytes().empty()) {
        memory.write32(cpu,0x027ff868,static_cast<std::uint32_t>(read16(firmware.bytes(),0x20)) * 8U);
        memory.write16(cpu,0x027ff874,read16(firmware.bytes(),0x26));
    }
    if (settings) for (std::uint32_t i = 0; i < settings->data.size(); ++i)
        memory.write8(cpu,firmware_settings_address + i,settings->data[i]);

    // Les piles ARM9 post-firmware résident dans 16 Kio de DTCM, pas dans la
    // WRAM partagée (attribuée à ARM7). Le MPU et les caches restent désactivés.
    memory.cp15().write(9,1,0,0x0300000a);
    memory.cp15().write(1,0,0,0x00012078);
}

} // namespace ravenemu::nds
