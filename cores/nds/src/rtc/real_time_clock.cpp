#include "rtc/real_time_clock.hpp"

namespace ravenemu::nds {

namespace {

constexpr std::int64_t seconds_per_day = 86'400;
constexpr std::int64_t days_per_century = 36'525;  // 2000-2099 : une année sur quatre bissextile

constexpr bool leap(unsigned year) noexcept { return year % 4U == 0; }

constexpr unsigned days_in_month(unsigned year, unsigned month) noexcept {
    constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap(year) ? 29U : days[(month - 1U) % 12U];
}

constexpr std::uint8_t to_bcd(unsigned value) noexcept { return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U)); }
constexpr unsigned from_bcd(std::uint8_t value) noexcept { return (value >> 4U) * 10U + (value & 15U); }

std::int64_t to_seconds(const DateTime& date) noexcept {
    std::int64_t days = 0;
    for (unsigned year = 2000; year < date.year; ++year) days += leap(year) ? 366 : 365;
    for (unsigned month = 1; month < date.month; ++month) days += days_in_month(date.year, month);
    days += static_cast<std::int64_t>(date.day) - 1;
    return days * seconds_per_day + date.hour * 3600LL + date.minute * 60LL + date.second;
}

DateTime from_seconds(std::int64_t seconds) noexcept {
    auto days = seconds / seconds_per_day % days_per_century;
    auto rest = seconds % seconds_per_day;
    DateTime date;
    date.hour = static_cast<unsigned>(rest / 3600);
    date.minute = static_cast<unsigned>(rest / 60 % 60);
    date.second = static_cast<unsigned>(rest % 60);
    while (days >= (leap(date.year) ? 366 : 365)) days -= leap(date.year++) ? 366 : 365;
    while (days >= days_in_month(date.year, date.month)) days -= days_in_month(date.year, date.month++);
    date.day = static_cast<unsigned>(days) + 1U;
    return date;
}

std::uint8_t reverse_bits(std::uint8_t value) noexcept {
    std::uint8_t reversed = 0;
    for (unsigned bit = 0; bit < 8; ++bit) reversed = static_cast<std::uint8_t>(reversed | (((std::uint32_t{value} >> bit) & 1U) << (7U - bit)));
    return reversed;
}

} // namespace

std::int64_t RealTimeClock::seconds(Cycles now) const noexcept {
    return base_seconds_ + static_cast<std::int64_t>((now - base_cycles_) / system_clock_hz);
}

void RealTimeClock::set_date_time(const DateTime& date_time, Cycles now) noexcept {
    base_seconds_ = to_seconds(date_time);
    base_cycles_ = now;
}

DateTime RealTimeClock::date_time(Cycles now) const noexcept { return from_seconds(seconds(now)); }

// Le 1er janvier 2000 était un samedi : 5 si 0 = lundi.
unsigned RealTimeClock::weekday(Cycles now) const noexcept {
    return static_cast<unsigned>((seconds(now) / seconds_per_day + 5 + weekday_offset_) % 7);
}

std::uint32_t RealTimeClock::read() const noexcept {
    const auto input = (lines_ & 0x10U) == 0;  // bit 4 à 0 : données lues de la puce
    return input ? (lines_ & ~1U) | (output_ ? 1U : 0U) : lines_;
}

void RealTimeClock::write(std::uint32_t value, std::uint32_t mask, Cycles now) noexcept {
    const auto old = lines_;
    lines_ = (lines_ & ~mask & 0xffffU) | (value & mask & 0xffffU);
    const auto selected = (lines_ & 4U) != 0;
    if ((old & 4U) == 0 && selected) {  // CS levé : nouvelle commande
        has_command_ = false;
        shift_ = 0;
        bit_ = 0;
    }
    if (!selected) {
        has_command_ = false;
        return;
    }
    const auto clock_was = (old & 2U) != 0;
    const auto clock_is = (lines_ & 2U) != 0;
    if (clock_was && !clock_is) on_falling();
    if (!clock_was && clock_is) on_rising((lines_ & 1U) != 0, now);
}

// Front montant : la puce lit un bit de la commande ou d'un paramètre écrit.
void RealTimeClock::on_rising(bool data, Cycles now) noexcept {
    if (has_command_ && reading_) return;
    shift_ = static_cast<std::uint8_t>(shift_ | ((data ? 1U : 0U) << bit_));
    if (++bit_ < 8) return;
    const auto byte = shift_;
    shift_ = 0;
    bit_ = 0;
    if (!has_command_) command_received(byte, now);
    else parameter_received(byte, now);
}

// Front descendant : en lecture, la puce sort le bit suivant.
void RealTimeClock::on_falling() noexcept {
    if (!has_command_ || !reading_) return;
    output_ = ((std::uint32_t{buffer_[index_ % buffer_.size()]} >> bit_) & 1U) != 0;
    if (++bit_ < 8) return;
    bit_ = 0;
    if (index_ + 1 < parameter_length()) ++index_;
}

unsigned RealTimeClock::parameter_length() const noexcept {
    switch (register_) {
    case 2: return 7;
    case 6:
    case 5: return 3;
    case 1: return (status2_ & 4U) != 0 ? 3U : 1U;
    default: return 1;
    }
}

std::uint8_t RealTimeClock::encode_hour(unsigned hour) const noexcept {
    const auto afternoon = hour >= 12 ? 0x40U : 0U;
    const auto shown = (status1_ & 2U) != 0 ? hour : hour % 12U;
    return static_cast<std::uint8_t>(to_bcd(shown) | afternoon);
}

unsigned RealTimeClock::decode_hour(std::uint8_t value) const noexcept {
    const auto hour = from_bcd(value & 0x3fU);
    if ((status1_ & 2U) != 0) return hour % 24U;
    return hour % 12U + ((value & 0x40U) != 0 ? 12U : 0U);
}

void RealTimeClock::command_received(std::uint8_t command, Cycles now) noexcept {
    if ((command & 0x0fU) != 0x06U) {
        if ((command >> 4U) != 0x06U) return;  // code fixe absent : commande ignorée
        command = reverse_bits(command);
    }
    has_command_ = true;
    reading_ = (command & 0x80U) != 0;
    register_ = (command >> 4U) & 7U;
    index_ = 0;
    buffer_.fill(0);
    if (!reading_) return;
    const auto date = date_time(now);
    switch (register_) {
    case 0:
        buffer_[0] = status1_;
        status1_ &= 0x0f;  // drapeaux effacés à la lecture
        break;
    case 4: buffer_[0] = status2_; break;
    case 2:
        buffer_ = {to_bcd(date.year - 2000U), to_bcd(date.month), to_bcd(date.day), static_cast<std::uint8_t>(weekday(now)),
                   encode_hour(date.hour), to_bcd(date.minute), to_bcd(date.second)};
        break;
    case 6: buffer_ = {encode_hour(date.hour), to_bcd(date.minute), to_bcd(date.second)}; break;
    case 1:
        if ((status2_ & 4U) != 0) buffer_ = {alarm1_[0], alarm1_[1], alarm1_[2]};
        else buffer_[0] = alarm1_[2];
        break;
    case 5: buffer_ = {alarm2_[0], alarm2_[1], alarm2_[2]}; break;
    case 3: buffer_[0] = adjust_; break;
    default: buffer_[0] = free_; break;
    }
}

void RealTimeClock::parameter_received(std::uint8_t value, Cycles now) noexcept {
    if (index_ >= parameter_length()) return;
    buffer_[index_++] = value;
    if (index_ == parameter_length()) apply_write(now);
}

void RealTimeClock::apply_write(Cycles now) noexcept {
    switch (register_) {
    case 0:
        if ((buffer_[0] & 1U) != 0) return reset(now);
        status1_ = static_cast<std::uint8_t>((status1_ & 0xf0U) | (buffer_[0] & 0x0eU));
        break;
    case 4: status2_ = buffer_[0]; break;
    case 2:
    case 6: {
        auto date = date_time(now);
        const auto time = register_ == 2 ? 4U : 0U;
        if (register_ == 2) {
            date.year = 2000U + from_bcd(buffer_[0]) % 100U;
            date.month = from_bcd(buffer_[1] & 0x1fU);
            date.day = from_bcd(buffer_[2] & 0x3fU);
        }
        date.hour = decode_hour(buffer_[time]);
        date.minute = from_bcd(buffer_[time + 1] & 0x7fU) % 60U;
        date.second = from_bcd(buffer_[time + 2] & 0x7fU) % 60U;
        if (date.month < 1 || date.month > 12) date.month = 1;
        if (date.day < 1 || date.day > days_in_month(date.year, date.month)) date.day = 1;
        set_date_time(date, now);
        if (register_ == 2) weekday_offset_ = (weekday_offset_ + 7U + (buffer_[3] & 7U) % 7U - weekday(now)) % 7U;
        break;
    }
    case 1:
        if ((status2_ & 4U) != 0) alarm1_ = {buffer_[0], buffer_[1], buffer_[2]};
        else alarm1_[2] = buffer_[0];  // fréquence, partagée avec la minute de l'alarme 1
        break;
    case 5: alarm2_ = {buffer_[0], buffer_[1], buffer_[2]}; break;
    case 3: adjust_ = buffer_[0]; break;
    default: free_ = buffer_[0]; break;
    }
}

// Remise à zéro (état 1 bit 0) : registres à 0, 1er janvier 2000 à 0 h.
void RealTimeClock::reset(Cycles now) noexcept {
    status1_ = status2_ = adjust_ = free_ = 0;
    alarm1_ = {};
    alarm2_ = {};
    weekday_offset_ = 0;
    set_date_time(DateTime{}, now);
}

} // namespace ravenemu::nds
