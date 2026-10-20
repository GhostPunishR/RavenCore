#include <ravenemu/nds/dma.hpp>

namespace ravenemu::nds {

void DmaController::reset() noexcept { channels_ = {}; }

std::uint32_t DmaController::address_mask(unsigned channel, bool source) const noexcept {
    if (processor_ == Processor::arm7 && (source ? channel == 0 : channel != 3)) return 0x07fffffeU;
    return 0x0ffffffeU;
}

std::uint32_t DmaController::count_mask(unsigned channel) const noexcept {
    return processor_ == Processor::arm9 ? 0x1fffffU : (channel == 3 ? 0xffffU : 0x3fffU);
}

unsigned DmaController::timing(std::uint32_t control) const noexcept {
    return processor_ == Processor::arm9 ? (control >> 27U) & 7U : (control >> 28U) & 3U;
}

std::uint8_t DmaController::read_byte(unsigned offset) const noexcept {
    if (offset >= 0x30) return 0;
    return static_cast<std::uint8_t>(channels_[offset / 12U].registers[(offset % 12U) / 4U] >> ((offset & 3U) * 8U));
}

void DmaController::write_byte(unsigned offset, std::uint8_t value) noexcept {
    if (offset >= 0x30) return;
    const auto number = offset / 12U;
    const auto reg = (offset % 12U) / 4U;
    auto& channel = channels_[number];
    const auto before = channel.registers[2];
    const auto shift = (offset & 3U) * 8U;
    const auto mask = reg < 2 ? address_mask(number, reg == 0) :
        (processor_ == Processor::arm9 ? 0xffffffffU : 0xf7e00000U | count_mask(number));
    channel.registers[reg] = ((channel.registers[reg] & ~(0xffU << shift)) |
        (static_cast<std::uint32_t>(value) << shift)) & mask;
    if (reg != 2) return;
    const auto control = channel.registers[2];
    if (!(control & 0x80000000U)) { channel.pending = channel.reload = false; return; }
    if (before & 0x80000000U) return; // pas de rechargement si déjà activé
    const auto width = (control & 0x04000000U) ? 4U : 2U;
    const auto count = control & count_mask(number);
    channel.transfer = {number, channel.registers[0] & ~(width - 1U), channel.registers[1] & ~(width - 1U),
                        count == 0 ? count_mask(number) + 1U : count, control};
    channel.reload = false;
    channel.pending = timing(control) == 0;
}

unsigned DmaController::event_mode(Event event) noexcept {
    return event == Event::vblank ? 1U : (event == Event::hblank ? 2U : 4U);
}

bool DmaController::listening(Event event) const noexcept {
    if (event != Event::vblank && processor_ == Processor::arm7) return false;
    for (const auto& channel : channels_)
        if ((channel.registers[2] & 0x80000000U) && timing(channel.transfer.control) == event_mode(event)) return true;
    return false;
}

void DmaController::pause(Event event) noexcept {
    if (event != Event::vblank && processor_ == Processor::arm7) return;
    for (auto& channel : channels_)
        if (timing(channel.transfer.control) == event_mode(event)) channel.pending = false;
}

void DmaController::trigger(Event event) noexcept {
    if (event != Event::vblank && processor_ == Processor::arm7) return;
    const auto mode = event_mode(event);
    for (unsigned i = 0; i < channels_.size(); ++i) {
        auto& channel = channels_[i];
        if (!(channel.registers[2] & 0x80000000U) || channel.pending || timing(channel.transfer.control) != mode) continue;
        if (channel.reload) {
            const auto count = channel.registers[2] & count_mask(i);
            channel.transfer.remaining = count == 0 ? count_mask(i) + 1U : count;
            if (((channel.transfer.control >> 21U) & 3U) == 3U) {
                const auto width = (channel.transfer.control & 0x04000000U) ? 4U : 2U;
                channel.transfer.destination = channel.registers[1] & ~(width - 1U);
            }
            channel.reload = false;
        }
        channel.burst_remaining = event == Event::display_fifo ? 4U : 0U;
        channel.pending = true;
    }
}

std::optional<DmaController::Transfer> DmaController::next() const noexcept {
    for (const auto& channel : channels_) if (channel.pending) return channel.transfer;
    return std::nullopt;
}

std::uint32_t DmaController::complete_unit() noexcept {
    for (unsigned i = 0; i < channels_.size(); ++i) {
        auto& channel = channels_[i];
        if (!channel.pending) continue;
        auto& transfer = channel.transfer;
        const auto width = (transfer.control & 0x04000000U) ? 4U : 2U;
        const auto source_mode = (transfer.control >> 23U) & 3U;
        const auto destination_mode = (transfer.control >> 21U) & 3U;
        if (source_mode < 2) transfer.source = (source_mode == 0 ? transfer.source + width : transfer.source - width) & address_mask(i, true);
        if (destination_mode != 2) transfer.destination = (destination_mode == 1 ? transfer.destination - width : transfer.destination + width) & address_mask(i, false);
        if (--transfer.remaining != 0) {
            if (processor_ == Processor::arm9 && timing(transfer.control) == 4 && --channel.burst_remaining == 0)
                channel.pending = false;
            return 0;
        }
        channel.pending = false;
        channel.reload = (transfer.control & 0x02000000U) && timing(transfer.control) != 0;
        if (!channel.reload) channel.registers[2] &= ~0x80000000U;
        return (transfer.control & 0x40000000U) ? 1U << (8U + i) : 0U;
    }
    return 0;
}

} // namespace ravenemu::nds
