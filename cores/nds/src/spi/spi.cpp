#include <ravenemu/nds/spi.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {

std::uint8_t Spi::read_byte(unsigned offset) const noexcept {
    if (offset < 2) {
        const auto status = control_ | (remaining_ ? 0x80U : 0U);
        return static_cast<std::uint8_t>(status >> (offset * 8U));
    }
    return offset == 2 ? data_ : 0;
}

std::uint16_t Spi::proposed_control(unsigned offset, std::uint32_t value, unsigned width) const noexcept {
    auto control = static_cast<unsigned>(control_);
    for (unsigned i = 0; i < width && offset + i < 2; ++i) {
        const auto shift = (offset + i) * 8U;
        control = (control & ~(0xffU << shift)) | (((value >> (i * 8U)) & 0xffU) << shift);
    }
    control &= 0xcf03U; // busy et positions inutilisées en lecture seule
    // Les écritures pendant busy sont ignorées, sauf la désactivation du bus.
    if (remaining_ && (control & 0x8000U)) return control_;
    return static_cast<std::uint16_t>(control);
}

void Spi::validate_write(unsigned offset, std::uint32_t value, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4) || offset >= 4 || width > 4 - offset || (offset & (width - 1U)))
        throw Error("NDS : accès SPI invalide");
    const auto control = proposed_control(offset, value, width);
    const bool strobe = offset <= 2 && offset + width > 2;
    if (!strobe || !(control & 0x8000U) || remaining_) return;
    if ((control & 0x300U) != 0x200U) throw Error("NDS : périphérique SPI firmware/Powerman/réservé non pris en charge");
    if (control & 0x400U) throw Error("NDS : transfert SPI 16 bits non pris en charge");
}

void Spi::write(unsigned offset, std::uint32_t value, unsigned width, const InputState& input) {
    validate_write(offset, value, width);
    const auto next = proposed_control(offset, value, width);
    if (!(next & 0x8000U) || ((next ^ control_) & 0x300U)) {
        touchscreen_.deselect();
        remaining_ = 0; // annulation sans IRQ
    }
    control_ = next;
    if (offset > 2 || offset + width <= 2 || !(control_ & 0x8000U) || remaining_) return;
    pending_touchscreen_ = touchscreen_;
    received_ = pending_touchscreen_.exchange(static_cast<std::uint8_t>(value >> ((2U - offset) * 8U)), input);
    // ARM7/8, /16, /32, /64 ; huit horloges série par octet.
    remaining_ = 64U << (control_ & 3U);
}

bool Spi::advance_cycles(std::uint64_t cycles) noexcept {
    if (!remaining_) return false;
    if (cycles < remaining_) { remaining_ -= static_cast<std::uint32_t>(cycles); return false; }
    remaining_ = 0;
    data_ = received_;
    touchscreen_ = pending_touchscreen_;
    if (!(control_ & 0x800U)) touchscreen_.deselect();
    return (control_ & 0x4000U) != 0;
}

} // namespace ravenemu::nds
