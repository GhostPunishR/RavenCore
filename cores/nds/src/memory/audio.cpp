#include <ravenemu/nds/memory.hpp>

namespace ravenemu::nds {

std::uint8_t Memory::audio_source(const void* context, std::uint32_t address) noexcept {
    const auto& memory = *static_cast<const Memory*>(context);
    // Accès matériel ARM7, sans TCM ARM9 ni effets de lecture des ports MMIO.
    // Les sources hors RAM/VRAM sont actuellement rendues silencieuses ;
    // le préchargement FIFO et les valeurs open-bus ne sont pas modélisés.
    if (address >= 0x02000000 && address < 0x04000000)
        return memory.ram_[memory.index(Processor::arm7,address)];
    return memory.video_.read_byte(Processor::arm7,address).value_or(0);
}

} // namespace ravenemu::nds
