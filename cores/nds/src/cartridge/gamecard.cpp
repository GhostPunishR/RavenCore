#include <ravenemu/nds/gamecard.hpp>
#include <ravenemu/core.hpp>

#include <utility>

namespace ravenemu::nds {

void Gamecard::load(const Cartridge& cartridge, Mode mode) {
    const auto rom = cartridge.rom();
    if (rom.empty() || rom.size() > 0x20000000 || rom[0x14] > 12)
        throw RomLoadError("NDS : capacité de cartouche non prise en charge");
    Gamecard prepared;
    prepared.cartridge_ = cartridge;
    prepared.mode_ = mode;
    const auto declared = std::uint32_t{0x20000} << rom[0x14];
    while (prepared.capacity_ < rom.size() || prepared.capacity_ < declared) prepared.capacity_ *= 2U;
    const auto size_code = prepared.capacity_ == 0x20000000 ? 0xfeU : (prepared.capacity_ >> 20U) - 1U;
    prepared.chip_id_ = 0xc2U | (size_code << 8U) | (prepared.capacity_ >= 0x8000000 ? 0x80000000U : 0U);
    prepared.reset();
    *this = std::move(prepared);
}

void Gamecard::reset() noexcept {
    command_ = {};
    control_ = mode_ == Mode::main_data ? 0x20000000U : 0U;
    auxiliary_ = mode_ == Mode::main_data ? 0x8000U : 0U;
    data_ = remaining_ = transferred_ = wait_ = base_ = offset_ = 0;
    active_command_ = 0;
    busy_ = ready_ = false;
}

std::uint8_t Gamecard::read_byte(unsigned offset) const noexcept {
    if (offset < 2) return static_cast<std::uint8_t>(auxiliary_ >> (offset * 8U));
    if (offset < 4) return 0;
    if (offset < 8) {
        const auto value = control_ | (busy_ ? 0x80000000U : 0U) | (ready_ ? 0x800000U : 0U);
        return static_cast<std::uint8_t>(value >> ((offset - 4U) * 8U));
    }
    return offset < 16 ? command_[offset - 8U] : 0;
}

std::uint32_t Gamecard::merge(unsigned offset, std::uint32_t value, unsigned width,
                              unsigned first, unsigned count, std::uint32_t previous) const noexcept {
    for (unsigned i = 0; i < width; ++i) if (offset + i >= first && offset + i < first + count) {
        const auto shift = (offset + i - first) * 8U;
        previous = (previous & ~(0xffU << shift)) | (((value >> (i * 8U)) & 0xffU) << shift);
    }
    return previous;
}

void Gamecard::validate_write(unsigned offset, std::uint32_t value, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4) || offset >= 16 || width > 16 - offset || (offset & (width - 1U)))
        throw Error("NDS : accès aux registres cartouche invalide");
    const auto auxiliary = merge(offset,value,width,0,2,auxiliary_) & 0xe043U;
    if (offset <= 2 && offset + width > 2 && (auxiliary & 0xa000U) == 0xa000U)
        throw Error("NDS : SPI des sauvegardes cartouche non pris en charge");
    const auto control = merge(offset,value,width,4,4,control_);
    if (!busy_ && (control & 0x8000U)) throw Error("NDS : application de semence KEY2 non prise en charge");
    const bool start = offset <= 7 && offset + width > 7 && (control & 0x80000000U);
    if (!start || busy_ || (auxiliary & 0xa000U) != 0x8000U) return;
    if (!((control | control_) & 0x20000000U)) throw Error("NDS : cartouche encore maintenue en reset");
    if (control & 0x40000000U) throw Error("NDS : écriture ROM/NAND non prise en charge");
    if (!cartridge_) return; // bus vide : données HIGH-Z modélisées par FF
    const auto command = command_[0];
    if ((mode_ == Mode::raw && command != 0 && command != 0x90 && command != 0x9f) ||
        (mode_ == Mode::main_data && command != 0xb7 && command != 0xb8))
        throw Error("NDS : commande cartouche ou démarrage KEY1 non pris en charge");
}

void Gamecard::write(unsigned offset, std::uint32_t value, unsigned width) {
    validate_write(offset,value,width);
    auxiliary_ = static_cast<std::uint16_t>(merge(offset,value,width,0,2,auxiliary_) & 0xe043U);
    if ((auxiliary_ & 0xa000U) != 0x8000U) { busy_ = ready_ = false; wait_ = remaining_ = 0; }
    if (offset < 8 && offset + width > 4 && !busy_) {
        const auto requested = merge(offset,value,width,4,4,control_);
        control_ = (requested & 0x7f7f7fffU) | (control_ & 0x20000000U);
        if ((requested & 0x80000000U) && offset + width > 7 && (auxiliary_ & 0xa000U) == 0x8000U) start(control_);
    }
    if (!busy_) for (unsigned i = 0; i < width; ++i) if (offset + i >= 8)
        command_[offset + i - 8U] = static_cast<std::uint8_t>(value >> (i * 8U));
}

void Gamecard::start(std::uint32_t control) noexcept {
    active_command_ = command_[0];
    const auto size = (control >> 24U) & 7U;
    remaining_ = size == 0 ? 0 : (size == 7 ? 4U : 0x100U << size);
    transferred_ = 0;
    std::uint32_t address = 0;
    for (unsigned i = 1; i <= 4; ++i) address = (address << 8U) | command_[i];
    address %= capacity_;
    if (address < 0x8000) address = 0x8000U + (address & 0x1ffU);
    base_ = address & ~0xfffU; offset_ = address & 0xfffU;
    const auto divider = control & 0x08000000U ? 8U : 5U;
    wait_ = (8U + (control & 0x1fffU) + (remaining_ ? 4U : 0U)) * divider;
    busy_ = true; ready_ = false;
}

std::uint32_t Gamecard::reply_word() const noexcept {
    if (!cartridge_ || active_command_ == 0x9f) return 0xffffffff;
    if (active_command_ == 0x90 || active_command_ == 0xb8) return chip_id_;
    const auto rom = cartridge_->rom();
    std::uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto address = active_command_ == 0 ? (transferred_ + i) & 0xfffU : base_ | ((offset_ + transferred_ + i) & 0xfffU);
        const auto value = address < rom.size() ? rom[address] : std::uint8_t{0xff};
        word |= static_cast<std::uint32_t>(value) << (i * 8U);
    }
    return word;
}

Gamecard::Events Gamecard::advance_cycles(std::uint64_t cycles) noexcept {
    if (!busy_ || ready_) return {};
    if (cycles < wait_) { wait_ -= static_cast<std::uint32_t>(cycles); return {}; }
    wait_ = 0;
    if (!remaining_) { busy_ = false; return {false,(auxiliary_ & 0x4000U) != 0}; }
    ready_ = true; data_ = reply_word();
    return {true,false};
}

Gamecard::Read Gamecard::read_data() noexcept {
    if (!ready_) return {data_,{}};
    remaining_ -= 4; transferred_ += 4; ready_ = false;
    if (!remaining_) { busy_ = false; return {data_,{false,(auxiliary_ & 0x4000U) != 0}}; }
    const auto divider = control_ & 0x08000000U ? 8U : 5U;
    wait_ = (4U + ((transferred_ & 0x1ffU) == 0 ? (control_ >> 16U) & 0x3fU : 0U)) * divider;
    return {data_,{}};
}

} // namespace ravenemu::nds
