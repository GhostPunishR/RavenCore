#include "machine/machine.hpp"
#include "machine/user_settings.hpp"

#include <algorithm>

// Démarrage direct : l'état que le BIOS et le firmware laissent au lancement
// d'une cartouche, d'après GBATEK (« BIOS RAM Usage », « DS Cartridge
// Header », « ARM CP15 »). Les piles et le réglage de CP15 sont des choix de
// RavenCore, signalés comme tels : les programmes les règlent eux-mêmes.
namespace ravenemu::nds {

namespace {

void write32(Memory& memory, std::uint32_t address, std::uint32_t value) { store(memory.main(address), value, 4); }
void write16(Memory& memory, std::uint32_t address, std::uint32_t value) { store(memory.main(address), value, 2); }

} // namespace

void Machine::direct_boot() {
    const auto rom = hw.card.rom();
    if (secure_area(rom, header) == SecureArea::encrypted)
        throw CartridgeError("zone sécurisée chiffrée : seules les ROM déchiffrées démarrent sans BIOS");
    hw.cp15.reset();
    arm9.reset();
    arm7.reset();
    boot_memory();
    boot_processors();
}

void Machine::boot_memory() {
    auto& memory = hw.memory;
    memory.wram_control = 3;  // les 32 Kio partagés à l'ARM7
    const auto rom = hw.card.rom();
    for (std::uint32_t index = 0; index < header.arm9_size; ++index)
        *memory.main(header.arm9_address + index) = rom[header.arm9_offset + index];
    for (std::uint32_t index = 0; index < header.arm7_size; ++index) {
        const auto address = header.arm7_address + index;
        auto* at = address >= 0x0300'0000U ? memory.wram7(address) : memory.main(address);
        *at = rom[header.arm7_offset + index];
    }
    // Le BIOS détruit l'identifiant « encryObj » de la zone sécurisée.
    if (header.arm9_offset <= 0x4000 && header.arm9_offset + header.arm9_size >= 0x4008 &&
        secure_area(rom, header) == SecureArea::decrypted) {
        const auto id = header.arm9_address + (0x4000U - header.arm9_offset);
        write32(memory, id, 0xe7ff'deffU);
        write32(memory, id + 4, 0xe7ff'deffU);
    }
    const auto id = hw.card.chip_id();
    for (const std::uint32_t base : {0x027f'f800U, 0x027f'fc00U}) {
        write32(memory, base, id);
        write32(memory, base + 4, id);
        write16(memory, base + 8, header.header_crc);
        write16(memory, base + 10, header.secure_area_crc);
    }
    write16(memory, 0x027f'f810U, 0xffffU);  // tâche de démarrage
    write16(memory, 0x027f'f850U, 0x5835U);  // CRC du BIOS ARM7 annoncé par GBATEK
    write16(memory, 0x027f'fc10U, 0x5835U);
    write32(memory, 0x027f'f880U, 7);  // message de l'ARM9 à l'ARM7
    write32(memory, 0x027f'f884U, 6);  // tâche de l'ARM7
    write16(memory, 0x027f'fc40U, 1);  // démarrage normal
    for (std::uint32_t index = 0; index < CartridgeHeader::size; ++index) *memory.main(0x027f'fe00U + index) = rom[index];
    const auto settings = user_settings_block(UserSettings{});
    std::copy(settings.begin(), settings.end(), memory.main(0x027f'fc80U));
    hw.post_boot = {1, 1};
    hw.external_memory = {0x6000U, 0};  // slots à l'ARM9, RAM principale en mode synchrone
}

void Machine::boot_processors() {
    // CP15 : TCM activées, DTCM à 0x027C0000 (base par défaut de GBATEK),
    // ITCM de 32 Mio (miroirs), vecteurs hauts, unité de protection et caches
    // activés comme par le firmware (ni l'une ni les autres ne sont modélisés).
    hw.cp15.write({15, 0, 9, 1, 0}, 0x027c'000aU);  // DTCM : base, 16 Kio
    hw.cp15.write({15, 0, 9, 1, 1}, 0x0000'0020U);  // ITCM : 32 Mio
    hw.cp15.write({15, 0, 1, 0, 0}, 0x0005'307dU);
    sync_cp15();
    // Piles : superviseur, IRQ, puis système, en haut de la DTCM (ARM9) et de
    // la WRAM de l'ARM7, sous les mots réservés au BIOS.
    const auto set_stacks = [](auto& cpu, std::uint32_t svc, std::uint32_t irq, std::uint32_t sys) {
        cpu.regs.switch_mode(arm::mode::supervisor);
        cpu.regs.r[13] = svc;
        cpu.regs.switch_mode(arm::mode::irq);
        cpu.regs.r[13] = irq;
        cpu.regs.switch_mode(arm::mode::system);
        cpu.regs.r[13] = sys;
        cpu.regs.irq_disabled = false;
        cpu.regs.fiq_disabled = false;
    };
    set_stacks(arm9, 0x027c'3fc0U, 0x027c'3f80U, 0x027c'3d80U);
    set_stacks(arm7, 0x0380'ffc0U, 0x0380'ff80U, 0x0380'fd80U);
    arm9.regs.r[12] = arm9.regs.r[14] = header.arm9_entry;
    arm7.regs.r[12] = arm7.regs.r[14] = header.arm7_entry;
    arm9.jump(header.arm9_entry, false);
    arm7.jump(header.arm7_entry, false);
}

} // namespace ravenemu::nds
