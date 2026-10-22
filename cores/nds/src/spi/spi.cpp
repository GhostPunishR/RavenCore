#include <ravenemu/nds/spi.hpp>
#include <ravenemu/core.hpp>

#include <utility>
#include <limits>

namespace ravenemu::nds {

void Spi::reset() noexcept {
    reset_bus();
    power_management_.reset();
}

void Spi::reset_bus() noexcept {
    control_ = 0;
    remaining_ = 0;
    data_ = received_ = 0;
    touchscreen_.reset();
    pending_touchscreen_.reset();
    firmware_session_ = {};
    pending_firmware_session_ = {};
    power_session_ = pending_power_session_ = {};
    firmware_.reset_serial(); // une opération déjà lancée reste autonome
}

void Spi::load_firmware(std::span<const std::uint8_t> bytes) {
    Firmware prepared{bytes}; // valider et allouer avant de toucher au bus actif
    // Un acquittement appartenant à l'ancienne image ne doit pas nettoyer la nouvelle.
    prepared.generation_ = prepared.saved_generation_ = firmware_.generation_ + 1U;
    firmware_ = std::move(prepared);
    reset_bus();
}

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
    if (control & 0x400U) throw Error("NDS : transfert SPI 16 bits non pris en charge");
    const auto device = control & 0x300U;
    if (device == 0 || device == 0x200U) return;
    if (device != 0x100U) throw Error("NDS : périphérique SPI réservé non pris en charge");
    auto session = firmware_session_;
    if ((control ^ control_) & 0x300U) session = {};
    static_cast<void>(firmware_.exchange(session, static_cast<std::uint8_t>(value >> ((2U - offset) * 8U))));
}

void Spi::write(unsigned offset, std::uint32_t value, unsigned width, const InputState& input) {
    validate_write(offset, value, width);
    if (power_management_.powered_off()) return;
    const auto next = proposed_control(offset, value, width);
    if (!(next & 0x8000U) || ((next ^ control_) & 0x300U)) {
        touchscreen_.deselect();
        power_session_ = pending_power_session_ = {};
        if (remaining_) firmware_session_ = {}; // byte incomplet : abandon du paquet
        else firmware_.deselect(firmware_session_);
        remaining_ = 0; // annulation sans IRQ
    }
    control_ = next;
    if (offset > 2 || offset + width <= 2 || !(control_ & 0x8000U) || remaining_) return;
    const auto output = static_cast<std::uint8_t>(value >> ((2U - offset) * 8U));
    if ((control_ & 0x300U) == 0) {
        pending_power_session_ = power_session_;
        received_ = power_management_.exchange(pending_power_session_, output);
    } else if ((control_ & 0x300U) == 0x100U) {
        pending_firmware_session_ = firmware_session_;
        received_ = firmware_.exchange(pending_firmware_session_, output);
    } else {
        pending_touchscreen_ = touchscreen_;
        received_ = pending_touchscreen_.exchange(output, input);
    }
    // ARM7/8, /16, /32, /64 ; huit horloges série par octet.
    remaining_ = 64U << (control_ & 3U);
}

std::uint64_t Spi::cycles_until_power_off() const noexcept {
    if (power_management_.powered_off()) return 0;
    return remaining_ && !(control_ & 0x300U) && PowerManagement::requests_power_off(pending_power_session_) ?
        remaining_ : std::numeric_limits<std::uint64_t>::max();
}

bool Spi::advance_cycles(std::uint64_t cycles) noexcept {
    const auto limit = cycles_until_power_off();
    if (!limit) return false;
    if (cycles > limit) cycles = limit;
    if (!remaining_) { firmware_.advance_cycles(cycles); return false; }
    if (cycles < remaining_) {
        firmware_.advance_cycles(cycles);
        remaining_ -= static_cast<std::uint32_t>(cycles);
        return false;
    }
    const auto elapsed = remaining_;
    firmware_.advance_cycles(elapsed);
    remaining_ = 0;
    data_ = received_;
    if ((control_ & 0x300U) == 0) {
        power_session_ = pending_power_session_;
        power_management_.complete_byte(power_session_);
        if (!(control_ & 0x800U)) power_session_ = {};
    } else if ((control_ & 0x300U) == 0x100U) {
        firmware_session_ = pending_firmware_session_;
        if (!(control_ & 0x800U)) firmware_.deselect(firmware_session_);
    } else {
        touchscreen_ = pending_touchscreen_;
        if (!(control_ & 0x800U)) touchscreen_.deselect();
    }
    // Une écriture démarre seulement à la fin du dernier octet et de CS.
    firmware_.advance_cycles(cycles - elapsed);
    return (control_ & 0x4000U) != 0;
}

} // namespace ravenemu::nds
