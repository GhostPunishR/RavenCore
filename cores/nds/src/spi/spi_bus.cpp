#include "spi/spi_bus.hpp"

#include "spi/firmware_image.hpp"

namespace ravenemu::nds {

namespace {

constexpr std::uint32_t busy = 0x0080U;
constexpr std::uint32_t hold = 0x0800U;
constexpr std::uint32_t irq_enable = 0x4000U;
constexpr std::uint32_t enabled = 0x8000U;

} // namespace

SpiBus::SpiBus() {
    firmware_.set_type(SaveType::flash_256k);
    static_cast<void>(firmware_.load(firmware_image(UserSettings{})));
}

std::uint32_t SpiBus::read(Cycles now) const noexcept {
    return control_ | (now < busy_until_ ? busy : 0U) | (std::uint32_t{data_} << 16U);
}

void SpiBus::write(std::uint32_t value, std::uint32_t mask, Cycles now, Scheduler& scheduler) {
    if ((mask & 0xffffU) != 0) {
        const auto old_device = device();
        // Bits 0-1, 8-11, 14, 15 ; le bit 7 (occupé) en lecture seule.
        control_ = (control_ & ~(mask & 0xcf03U)) | (value & mask & 0xcf03U);
        if (device() != old_device || (control_ & enabled) == 0) deselect();
    }
    if ((mask & 0x00ff'0000U) != 0) transfer(static_cast<std::uint8_t>(value >> 16U), now, scheduler);
}

void SpiBus::transfer(std::uint8_t value, Cycles now, Scheduler& scheduler) {
    if ((control_ & enabled) == 0) return;
    switch (device()) {
    case 0: data_ = power_.transfer(value); break;
    case 1: data_ = firmware_.transfer(value); break;
    case 2: data_ = touch_.transfer(value); break;
    default: data_ = 0; break;
    }
    if ((control_ & hold) == 0) deselect();
    busy_until_ = now + (Cycles{64} << (control_ & 3U));
    if ((control_ & irq_enable) != 0) scheduler.schedule(Event::spi, busy_until_);
}

void SpiBus::on_event(InterruptController& irq) const noexcept {
    if ((control_ & irq_enable) != 0) irq.request(irq::spi);
}

void SpiBus::deselect() noexcept {
    power_.deselect();
    firmware_.deselect();
    touch_.deselect();
}

} // namespace ravenemu::nds
