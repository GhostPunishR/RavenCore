#include <ravenemu/nds/firmware.hpp>
#include <ravenemu/core.hpp>

#include <algorithm>
#include <stdexcept>

namespace ravenemu::nds {
namespace {
std::uint16_t read16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<unsigned>(bytes[offset + 1]) << 8U));
}

std::uint16_t settings_crc(std::span<const std::uint8_t> bytes) noexcept {
    unsigned crc = 0xffff;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xa001U : 0U);
    }
    return static_cast<std::uint16_t>(crc);
}

std::optional<FirmwareSettings> read_settings(std::span<const std::uint8_t> bytes, std::uint32_t offset) noexcept {
    const auto block = bytes.subspan(offset, 0x100);
    const auto counter = read16(block, 0x70);
    if (read16(block, 0) != 5 || counter > 0x7f || read16(block, 0x72) != settings_crc(block.first(0x70))) return std::nullopt;
    FirmwareSettings settings;
    std::copy_n(block.begin(), settings.data.size(), settings.data.begin());
    settings.offset = offset;
    settings.counter = counter;
    const TouchCalibration calibration{
        {read16(block, 0x58), read16(block, 0x5a), block[0x5c], block[0x5d]},
        {read16(block, 0x5e), read16(block, 0x60), block[0x62], block[0x63]}
    };
    if (calibration.valid()) settings.touch_calibration = calibration;
    return settings;
}
} // namespace

bool TouchCalibration::valid() const noexcept {
    const auto valid_point = [](const TouchCalibrationPoint& point) {
        return point.adc_x <= 0xfff && point.adc_y <= 0xfff && point.screen_x != 0 &&
               point.screen_y != 0 && point.screen_y <= 192;
    };
    return valid_point(first) && valid_point(second) && first.adc_x != second.adc_x &&
           first.adc_y != second.adc_y && first.screen_x != second.screen_x && first.screen_y != second.screen_y;
}

TouchSample TouchCalibration::sample_at(std::uint16_t x, std::uint16_t y) const {
    if (x >= 256 || y >= 192) throw std::invalid_argument("NDS : position tactile hors écran");
    if (!valid()) throw Error("NDS : étalonnage tactile invalide");
    const auto convert = [](int pixel, int screen1, int screen2, int adc1, int adc2) {
        auto numerator = (pixel + 1 - screen1) * (adc2 - adc1);
        auto denominator = screen2 - screen1;
        if (denominator < 0) { numerator = -numerator; denominator = -denominator; }
        const auto delta = numerator < 0 ? -((-numerator + denominator / 2) / denominator) :
                                           (numerator + denominator / 2) / denominator;
        return static_cast<std::uint16_t>(std::clamp(adc1 + delta, 0, 0xfff));
    };
    return {convert(x, first.screen_x, second.screen_x, first.adc_x, second.adc_x),
            convert(y, first.screen_y, second.screen_y, first.adc_y, second.adc_y)};
}

Firmware::Firmware(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 0x40000 && bytes.size() != 0x80000)
        throw RomLoadError("NDS : firmware DS attendu de 256 ou 512 Kio");
    bytes_.assign(bytes.begin(), bytes.end());
    refresh_settings();
}

void Firmware::refresh_settings() noexcept {
    settings_.reset();
    if (bytes_.empty()) return;
    const std::span<const std::uint8_t> bytes = bytes_;
    const auto offset = static_cast<std::uint32_t>(read16(bytes, 0x20)) * 8U;
    if (offset < 0x200 || offset > bytes.size() - 0x200) return;
    auto first = read_settings(bytes, offset);
    auto second = read_settings(bytes, offset + 0x100);
    if (!first || (second && second->counter == ((first->counter + 1U) & 0x7fU))) settings_ = second;
    else settings_ = first;
}

FirmwareSnapshot Firmware::snapshot() const {
    if (bytes_.empty()) throw Error("NDS : aucun firmware à sauvegarder");
    if (busy()) throw Error("NDS : opération flash en cours, sauvegarde firmware différée");
    return {bytes_, generation_};
}

} // namespace ravenemu::nds
