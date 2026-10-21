#pragma once

#include "cartridge/cartridge.hpp"

namespace ravenemu::cgb {

class Mbc0 final : public Cartridge {
public:
    using Cartridge::Cartridge;
    int rom_bank_at(int address) const noexcept override { return address < rom_bank_size ? 0 : 1; }
    int read_rom(int address) const override { return rom_byte(rom_bank_at(address), address); }
    void write_control(int, int) override {}
    int read_ram(int address) const override {
        const auto offset = static_cast<std::size_t>(address - 0xa000);
        return offset < ram_.size() ? ram_[offset] : 0xff;
    }
    void write_ram(int address, int value) override {
        const auto offset = static_cast<std::size_t>(address - 0xa000);
        if (offset < ram_.size()) { ram_[offset] = static_cast<std::uint8_t>(value); mark_written(); }
    }
    void save_state(BinaryWriter& out) const override { out.raw(ram_); }
    void load_state(BinaryReader& in) override { in.raw(ram_); }
};

} // namespace ravenemu::cgb
