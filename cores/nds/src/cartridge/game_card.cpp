#include "cartridge/game_card.hpp"

#include <bit>
#include <utility>

namespace ravenemu::nds {

namespace {

constexpr std::uint32_t busy = 0x8000'0000U;        // ROMCTRL bit 31
constexpr std::uint32_t data_ready = 0x0080'0000U;  // ROMCTRL bit 23
constexpr std::uint32_t reset_released = 0x2000'0000U;
constexpr std::uint32_t slot_enabled = 0x8000U;  // AUXSPICNT bit 15
constexpr std::uint32_t spi_mode = 0x2000U;      // AUXSPICNT bit 13
constexpr std::uint32_t done_irq = 0x4000U;      // AUXSPICNT bit 14
constexpr std::uint32_t spi_hold = 0x0040U;      // AUXSPICNT bit 6
constexpr std::uint32_t spi_busy = 0x0080U;      // AUXSPICNT bit 7

constexpr std::uint32_t merge(std::uint32_t old, std::uint32_t value, std::uint32_t mask) noexcept {
    return (old & ~mask) | (value & mask);
}

} // namespace

std::uint32_t cartridge_chip_id(const CartridgeHeader& header) noexcept {
    const auto megabytes = header.capacity >= 3 && header.capacity < 13 ? 1U << (header.capacity - 3U) : 1U;
    return 0xc2U | ((megabytes - 1U) << 8U);
}

void GameCard::insert(std::vector<std::uint8_t> rom, std::uint32_t chip_id) {
    rom_ = std::move(rom);
    chip_id_ = chip_id;
    save_.set_type(SaveType::automatic);
    const auto megabytes = ((chip_id >> 8U) & 0x7fU) + 1U;
    chip_mask_ = std::bit_ceil(megabytes << 20U) - 1U;
    // État laissé par le BIOS : ROMCTRL des commandes normales (en-tête,
    // 060h), sans bits d'état, et le reset de la cartouche relâché.
    std::uint32_t header_control = 0;
    for (unsigned byte = 0; byte < 4 && 0x60U + byte < rom_.size(); ++byte)
        header_control |= std::uint32_t{rom_[0x60U + byte]} << (8U * byte);
    rom_control_ = (header_control & 0x5f7f'7fffU) | reset_released;
}

std::uint32_t GameCard::read(std::uint32_t offset, Cycles now) const noexcept {
    switch (offset) {
    case 0x00: return spi_control_ | (now < spi_busy_until_ ? spi_busy : 0U) | (std::uint32_t{spi_data_} << 16U);
    case 0x04: return rom_control_;
    default: return 0;  // commande et graines : en écriture seule
    }
}

void GameCard::write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now, Scheduler& scheduler,
                     std::uint32_t instruction) {
    switch (offset) {
    case 0x00:
        write_spi_control(value, mask);
        if ((mask & 0x00ff'0000U) != 0) spi_transfer(static_cast<std::uint8_t>(value >> 16U), now, instruction);
        break;
    case 0x04:
        // Bits 15 (graine) et 31 (départ) en écriture seule, 23 en lecture
        // seule ; le reset (bit 29) ne se remet pas.
        rom_control_ = merge(rom_control_, value, mask & 0x5f7f'7fffU);
        if ((value & mask & reset_released) != 0) rom_control_ |= reset_released;
        if ((value & mask & busy) != 0 && (rom_control_ & busy) == 0) start(now, scheduler);
        break;
    case 0x08:
    case 0x0c:
        for (unsigned byte = 0; byte < 4; ++byte) {
            if (((mask >> (8U * byte)) & 0xffU) != 0) command_[offset - 0x08U + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
        }
        break;
    default: break;
    }
}

// AUXSPICNT : bit 7 (occupé) en lecture seule. Quitter le mode SPI, ou
// couper le slot, relâche la puce.
void GameCard::write_spi_control(std::uint32_t value, std::uint32_t mask) {
    spi_control_ = merge(spi_control_, value, mask & 0xe043U);
    if ((spi_control_ & (slot_enabled | spi_mode)) != (slot_enabled | spi_mode)) save_.deselect();
}

// AUXSPIDATA : un octet échangé avec la puce, en mode SPI seulement. Sans
// AUXSPICNT bit 6, la puce est relâchée après l'octet.
void GameCard::spi_transfer(std::uint8_t value, Cycles now, std::uint32_t instruction) {
    if ((spi_control_ & (slot_enabled | spi_mode)) != (slot_enabled | spi_mode)) return;
    spi_data_ = save_.transfer(value, instruction);
    if ((spi_control_ & spi_hold) == 0) save_.deselect();
    spi_busy_until_ = now + (Cycles{64} << (spi_control_ & 3U));
}

void GameCard::start(Cycles now, Scheduler& scheduler) {
    if ((spi_control_ & slot_enabled) == 0 || (spi_control_ & spi_mode) != 0) return;
    const auto block = (rom_control_ >> 24U) & 7U;
    length_ = block == 0 ? 0U : (block == 7 ? 4U : 0x100U << block);
    position_ = 0;
    rom_control_ = (rom_control_ | busy) & ~data_ready;
    const auto clocks = 8U + (rom_control_ & 0x1fffU) + (length_ != 0 ? 4U : 0U);
    scheduler.schedule(Event::card, now + clocks * clock());
}

bool GameCard::on_event(InterruptController& irq) {
    if ((rom_control_ & busy) == 0) return false;
    if (length_ == 0) {
        finish(irq);
        return false;
    }
    data_ = 0;
    for (unsigned byte = 0; byte < 4; ++byte) data_ |= std::uint32_t{stream_byte(position_ + byte)} << (8U * byte);
    position_ += 4;
    rom_control_ |= data_ready;
    return true;
}

std::uint32_t GameCard::read_data(Cycles now, Scheduler& scheduler, InterruptController& irq) {
    if ((rom_control_ & data_ready) == 0) return data_;
    rom_control_ &= ~data_ready;
    if (position_ >= length_) {
        finish(irq);
    } else {
        auto clocks = Cycles{4};
        if (position_ % 0x200U == 0) clocks += (rom_control_ >> 16U) & 0x3fU;
        scheduler.schedule(Event::card, now + clocks * clock());
    }
    return data_;
}

void GameCard::finish(InterruptController& irq) noexcept {
    rom_control_ &= ~(busy | data_ready);
    if ((spi_control_ & done_irq) != 0) irq.request(irq::card_done);
}

std::uint8_t GameCard::stream_byte(std::uint32_t index) const noexcept {
    switch (command_[0]) {
    case 0xb7: {
        const auto start = (std::uint32_t{command_[1]} << 24U) | (std::uint32_t{command_[2]} << 16U) |
                           (std::uint32_t{command_[3]} << 8U) | command_[4];
        auto address = (start & ~0xfffU) | ((start + index) & 0xfffU);  // flux bouclé dans son bloc de 4 Kio
        address &= chip_mask_;                                          // au-delà de la puce : miroir
        if (address < 0x8000U) address = 0x8000U + (address & 0x1ffU);  // en-tête et zone sécurisée
        return address < rom_.size() ? rom_[address] : 0xffU;
    }
    case 0xb8: return static_cast<std::uint8_t>(chip_id_ >> ((index & 3U) * 8U));
    default: return 0;
    }
}

} // namespace ravenemu::nds
