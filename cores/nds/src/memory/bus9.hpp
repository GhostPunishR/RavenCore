#pragma once

#include "arm/cpu.hpp"
#include "bios/bios_image.hpp"
#include "machine/hardware.hpp"
#include "memory/memory_timing.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Bus de l'ARM9 (GBATEK, « NDS9 Memory Map ») : TCM, RAM principale, WRAM,
// E/S, palettes, VRAM, OAM, slot GBA et BIOS. Il compte en cycles de l'ARM9.
namespace ravenemu::nds {

// Appel d'une SWI, renvoyé au BIOS pris par l'hôte.
struct SwiHook {
    bool (*call)(void* context, std::uint32_t comment, bool thumb){};
    void* context{};
};

class Bus9 {
public:
    explicit Bus9(Hardware& hardware) : hw_(hardware), bios_(bios_image(arm9_bios_code, arm9_bios_size)) {}

    std::uint32_t code32(std::uint32_t address) {
        Region region{};
        const auto* at = locate(address, false, true, region);
        hw_.arm9_cycles += static_cast<Cycles>(code_cost(region));
        return at != nullptr ? load(at, 4) : other_read(address, 4, region);
    }
    // Un demi-mot Thumb coûte la moitié du mot lu par l'ARM9.
    std::uint32_t code16(std::uint32_t address) {
        Region region{};
        const auto* at = locate(address, false, true, region);
        hw_.arm9_cycles += static_cast<Cycles>((code_cost(region) + 1) / 2);
        return at != nullptr ? load(at, 2) : other_read(address, 2, region);
    }
    std::uint32_t read8(std::uint32_t address) { return read(address, 1); }
    std::uint32_t read16(std::uint32_t address) { return read(address, 2); }
    std::uint32_t read32(std::uint32_t address) { return read(address, 4); }
    void write8(std::uint32_t address, std::uint32_t value) { write(address, value, 1); }
    void write16(std::uint32_t address, std::uint32_t value) { write(address, value, 2); }
    void write32(std::uint32_t address, std::uint32_t value) { write(address, value, 4); }
    void internal(int cycles) noexcept { hw_.arm9_cycles += static_cast<Cycles>(cycles); }

    std::optional<std::uint32_t> coprocessor_read(arm::CoprocessorAccess access) const noexcept {
        if (access.coprocessor != 15) return std::nullopt;
        return hw_.cp15.read(access);
    }
    bool coprocessor_write(arm::CoprocessorAccess access, std::uint32_t value) noexcept {
        if (access.coprocessor != 15) return false;
        if (hw_.cp15.write(access, value)) hw_.arm9_halted = true;
        cp15_changed = true;
        return true;
    }
    bool software_interrupt(std::uint32_t comment, bool thumb) {
        return swi.call != nullptr && swi.call(swi.context, comment, thumb);
    }
    // Rechargement du pipeline : deux mots en ARM ; en Thumb, les deux
    // demi-mots viennent d'une seule lecture de 32 bits.
    void refill(std::uint32_t address, bool thumb) {
        Region region{};
        static_cast<void>(locate(address, false, true, region));
        hw_.arm9_cycles += static_cast<Cycles>(code_cost(region) * (thumb ? 1 : 2));
    }

    // Lecture sans coût ni effet sur les E/S, pour l'hôte (tests, débogage).
    [[nodiscard]] std::uint32_t peek(std::uint32_t address, unsigned width) {
        Region region{};
        const auto* at = locate(address, false, false, region);
        return at != nullptr ? load(at, width) : 0U;
    }

    SwiHook swi;
    // Levé à chaque écriture dans CP15 : la machine en recopie les réglages
    // (vecteurs, changement d'état par LDR PC) dans le CPU.
    bool cp15_changed{};

private:
    // Octet visé par l'accès, ou nullptr pour les E/S, le slot GBA, la VRAM et
    // les zones vides. La DTCM ne sert pas au code.
    std::uint8_t* locate(std::uint32_t address, bool write, bool code, Region& region) noexcept {
        auto& cp = hw_.cp15;
        if (cp.in_itcm(address) && (write || cp.itcm_reads())) {
            region = Region::tcm;
            return cp.itcm(address);
        }
        if (!code && cp.in_dtcm(address) && (write || cp.dtcm_reads())) {
            region = Region::tcm;
            return cp.dtcm(address);
        }
        switch (address >> 24U) {
        case 0x02: region = Region::main_ram; return hw_.memory.main(address);
        case 0x03: region = Region::wram; return hw_.memory.wram9(address);
        case 0x04: region = Region::io; return nullptr;
        case 0x05: region = Region::palette; return &hw_.memory.palette[address & 0x7ffU];
        case 0x06: region = Region::vram; return nullptr;
        case 0x07: region = Region::oam; return &hw_.memory.oam[address & 0x7ffU];
        case 0x08:
        case 0x09: region = Region::gba_rom; return nullptr;
        case 0x0a: region = Region::gba_ram; return nullptr;
        case 0xff:
            if (address >= 0xffff'0000U) {
                region = Region::bios;
                return write ? nullptr : &bios_[address & (arm9_bios_size - 1)];
            }
            [[fallthrough]];
        default: region = Region::none; return nullptr;
        }
    }

    [[nodiscard]] int code_cost(Region region) const noexcept {
        const auto cached = (hw_.cp15.control() & Cp15::instruction_cache) != 0;
        if (cached && (region == Region::main_ram || region == Region::bios)) return 1;
        return arm9_code_cost(region);
    }
    void charge_data(Region region, std::uint32_t address, unsigned width, bool write) noexcept {
        const auto sequential = region == last_region_ && address == last_address_ + last_width_;
        auto cost = arm9_data_costs(region).cost(sequential, width);
        if (!write && region == Region::main_ram && (hw_.cp15.control() & Cp15::data_cache) != 0) cost = 1;
        hw_.arm9_cycles += static_cast<Cycles>(cost);
        last_region_ = region;
        last_address_ = address;
        last_width_ = width;
    }

    std::uint32_t read(std::uint32_t address, unsigned width) {
        Region region{};
        const auto* at = locate(address, false, false, region);
        charge_data(region, address, width, false);
        return at != nullptr ? load(at, width) : other_read(address, width, region);
    }
    void write(std::uint32_t address, std::uint32_t value, unsigned width) {
        Region region{};
        auto* at = locate(address, true, false, region);
        charge_data(region, address, width, true);
        // Palettes, VRAM et OAM ignorent les écritures de 8 bits (GBATEK).
        if (width == 1 && (region == Region::palette || region == Region::vram || region == Region::oam)) return;
        if (at != nullptr) store(at, value, width);
        else if (region == Region::io) io_write(address, value, width);
        else if (region == Region::vram) vram_write(address, value, width);
    }

    std::uint32_t other_read(std::uint32_t address, unsigned width, Region region);
    void vram_write(std::uint32_t address, std::uint32_t value, unsigned width);
    std::uint32_t io_word(std::uint32_t address);
    void io_write(std::uint32_t address, std::uint32_t value, unsigned width);
    void io_write_word(std::uint32_t address, std::uint32_t value, std::uint32_t mask);

    Hardware& hw_;
    std::vector<std::uint8_t> bios_;
    Region last_region_{Region::none};
    std::uint32_t last_address_{};
    unsigned last_width_{};
};

} // namespace ravenemu::nds
