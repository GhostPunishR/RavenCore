#include <ravenemu/nds/touchscreen.hpp>

namespace ravenemu::nds {

void Touchscreen::deselect() noexcept {
    reply_ = 0;
    reply_bits_ = command_bits_ = 0;
    command_ = 0;
    // CS ne modifie pas le mode d'alimentation sélectionné par la commande.
}

void Touchscreen::convert(std::uint8_t command, const InputState& input) noexcept {
    const auto channel = (command >> 4U) & 7U;
    unsigned sample = 0;
    switch (channel) {
    case 1: sample = input.pen_down ? input.touch.y : 0xfffU; break;
    case 3: sample = input.pen_down ? input.touch.z1 : 0U; break;
    case 4: sample = input.pen_down ? input.touch.z2 : 0xfffU; break;
    case 5: sample = input.pen_down ? input.touch.x : 0U; break;
    default: break; // batterie à GND ; température/microphone non modélisés
    }
    sample &= 0xfffU;
    const bool eight_bits = (command & 8U) != 0;
    if (eight_bits) sample &= 0xff0U;
    reply_ = static_cast<std::uint16_t>(sample << 3U);
    reply_bits_ = eight_bits ? 9U : 13U; // bit muet inclus
    power_mode_ = command & 3U;
}

std::uint8_t Touchscreen::exchange(std::uint8_t output, const InputState& input) noexcept {
    std::uint8_t received = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        // Une commande peut commencer dès que seuls six bits ADC restent.
        const bool can_start = reply_bits_ <= 6;
        received = static_cast<std::uint8_t>((static_cast<unsigned>(received) << 1U) | (static_cast<unsigned>(reply_) >> 15U));
        reply_ = static_cast<std::uint16_t>(reply_ << 1U);
        if (reply_bits_) --reply_bits_;
        const auto next = (static_cast<unsigned>(output) >> (7U - bit)) & 1U;
        if (command_bits_) {
            command_ = static_cast<std::uint8_t>((static_cast<unsigned>(command_) << 1U) | next);
            if (++command_bits_ == 8) {
                convert(command_, input);
                command_bits_ = 0;
            }
        } else if (next && can_start) {
            command_ = 1;
            command_bits_ = 1;
        }
    }
    return received;
}

} // namespace ravenemu::nds
