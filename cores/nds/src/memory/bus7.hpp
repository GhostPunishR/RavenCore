#pragma once

#include "arm/cpu.hpp"
#include "bios/bios_image.hpp"
#include "machine/hardware.hpp"
#include "memory/bus9.hpp"
#include "memory/memory_timing.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Bus de l'ARM7 (GBATEK, « NDS7 Memory Map ») : BIOS, RAM principale, WRAM
// partagée et propre, E/S, VRAM qui lui est donnée (banques C et D), slot
// GBA. Il compte en cycles système. La Wi-Fi n'est pas encore modélisée.
namespace ravenemu::nds {

class Bus7 {
public:
    explicit Bus7(Hardware& hardware) : hw_(hardware), bios_(bios_image(arm7_bios_code, arm7_bios_size)) {}

    std::uint32_t code16(std::uint32_t address) { return fetch(address, 2); }
    std::uint32_t code32(std::uint32_t address) { return fetch(address, 4); }
    std::uint32_t read8(std::uint32_t address) { return read(address, 1); }
    std::uint32_t read16(std::uint32_t address) { return read(address, 2); }
    std::uint32_t read32(std::uint32_t address) { return read(address, 4); }
    void write8(std::uint32_t address, std::uint32_t value) { write(address, value, 1); }
    void write16(std::uint32_t address, std::uint32_t value) { write(address, value, 2); }
    void write32(std::uint32_t address, std::uint32_t value) { write(address, value, 4); }
    void internal(int cycles) noexcept { hw_.arm7_cycles += static_cast<Cycles>(cycles); }

    // L'ARM7 n'a pas de coprocesseur : MRC et MCR sont indéfinies.
    static std::optional<std::uint32_t> coprocessor_read(arm::CoprocessorAccess) noexcept { return std::nullopt; }
    static bool coprocessor_write(arm::CoprocessorAccess, std::uint32_t) noexcept { return false; }
    bool software_interrupt(std::uint32_t comment, bool thumb) {
        return swi.call != nullptr && swi.call(swi.context, comment, thumb);
    }
    // Rechargement du pipeline : N à la cible, puis S ; la lecture suivante, à
    // la cible, compte alors comme séquentielle.
    void refill(std::uint32_t address, bool thumb) {
        Region region{};
        static_cast<void>(locate(address, false, region));
        const auto width = thumb ? 2U : 4U;
        const auto costs = arm7_code_costs(region);
        hw_.arm7_cycles += static_cast<Cycles>(costs.cost(false, width) + costs.cost(true, width));
        code_ = {region, address - width, width};
    }

    [[nodiscard]] std::uint32_t peek(std::uint32_t address, unsigned width) {
        Region region{};
        const auto* at = locate(address, false, region);
        return at != nullptr ? load(at, width) : 0U;
    }

    std::uint32_t dma_read16(std::uint32_t address) { return read(address, 2); }
    std::uint32_t dma_read32(std::uint32_t address) { return read(address, 4); }
    void dma_write16(std::uint32_t address, std::uint32_t value) { write(address, value, 2); }
    void dma_write32(std::uint32_t address, std::uint32_t value) { write(address, value, 4); }
    void trigger_dma(DmaTiming timing) {
        for (unsigned index = 0; index < 4; ++index) {
            auto& channel = hw_.dma7[index];
            if (channel.enabled() && dma_timing(channel.control, false, index) == timing)
                run_dma(*this, channel, index, false, hw_.irq7);
        }
    }

    SwiHook swi;

private:
    struct Sequence {
        Region region{Region::none};
        std::uint32_t address{};
        unsigned width{};

        bool follows(Region next, std::uint32_t at) const noexcept { return next == region && at == address + width; }
    };

    std::uint8_t* locate(std::uint32_t address, bool write, Region& region) noexcept {
        switch (address >> 24U) {
        case 0x00:
            if (address < arm7_bios_size) {
                region = Region::bios;
                return write ? nullptr : &bios_[address];
            }
            break;
        case 0x02: region = Region::main_ram; return hw_.memory.main(address);
        case 0x03: region = Region::wram; return hw_.memory.wram7(address);
        case 0x04: region = Region::io; return nullptr;
        case 0x06: region = Region::vram; return nullptr;
        case 0x08:
        case 0x09: region = Region::gba_rom; return nullptr;
        case 0x0a: region = Region::gba_ram; return nullptr;
        default: break;
        }
        region = Region::none;
        return nullptr;
    }

    std::uint32_t fetch(std::uint32_t address, unsigned width) {
        Region region{};
        const auto* at = locate(address, false, region);
        hw_.arm7_cycles += static_cast<Cycles>(arm7_code_costs(region).cost(code_.follows(region, address), width));
        code_ = {region, address, width};
        return at != nullptr ? load(at, width) : other_read(address, width, region);
    }
    std::uint32_t read(std::uint32_t address, unsigned width) {
        Region region{};
        const auto* at = locate(address, false, region);
        charge_data(region, address, width);
        return at != nullptr ? load(at, width) : other_read(address, width, region);
    }
    void write(std::uint32_t address, std::uint32_t value, unsigned width) {
        Region region{};
        auto* at = locate(address, true, region);
        charge_data(region, address, width);
        if (at != nullptr) store(at, value, width);
        else if (region == Region::io) io_write(address, value, width);
        else if (region == Region::vram) hw_.vram.write(Vram::arm7, address & 0x3ffffU, value, width);
    }
    void charge_data(Region region, std::uint32_t address, unsigned width) noexcept {
        hw_.arm7_cycles += static_cast<Cycles>(arm7_data_costs(region).cost(data_.follows(region, address), width));
        data_ = {region, address, width};
    }

    std::uint32_t other_read(std::uint32_t address, unsigned width, Region region);
    std::uint32_t io_word(std::uint32_t address);
    void io_write(std::uint32_t address, std::uint32_t value, unsigned width);
    void io_write_word(std::uint32_t address, std::uint32_t value, std::uint32_t mask);

    Hardware& hw_;
    std::vector<std::uint8_t> bios_;
    Sequence code_;
    Sequence data_;
};

} // namespace ravenemu::nds
