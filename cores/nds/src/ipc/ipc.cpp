#include <ravenemu/nds/ipc.hpp>

namespace ravenemu::nds {
namespace {
constexpr Processor other(Processor cpu) noexcept {
    return cpu == Processor::arm9 ? Processor::arm7 : Processor::arm9;
}
} // namespace

Ipc::Endpoint& Ipc::endpoint(Processor cpu) noexcept { return endpoints_[cpu == Processor::arm9 ? 0 : 1]; }
const Ipc::Endpoint& Ipc::endpoint(Processor cpu) const noexcept { return endpoints_[cpu == Processor::arm9 ? 0 : 1]; }
void Ipc::reset() noexcept { endpoints_ = {}; }

std::uint16_t Ipc::sync(Processor cpu) const noexcept {
    return static_cast<std::uint16_t>(endpoint(cpu).sync | ((endpoint(other(cpu)).sync >> 8U) & 15U));
}

Ipc::Interrupts Ipc::write_sync(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept {
    auto& local = endpoint(cpu);
    const auto writable = mask & 0x4f00U;
    local.sync = static_cast<std::uint16_t>((local.sync & ~writable) | (value & writable));
    Interrupts result;
    if ((value & mask & 0x2000U) && (endpoint(other(cpu)).sync & 0x4000U)) {
        if (cpu == Processor::arm9) result.arm7 = 1U << 16U;
        else result.arm9 = 1U << 16U;
    }
    return result;
}

std::uint16_t Ipc::control(Processor cpu) const noexcept {
    const auto& local = endpoint(cpu);
    const auto& incoming = endpoint(other(cpu)).outgoing;
    return static_cast<std::uint16_t>(local.control | (local.error ? 0x4000U : 0U) |
        (local.outgoing.size == 0 ? 1U : 0U) | (local.outgoing.size == 16 ? 2U : 0U) |
        (incoming.size == 0 ? 0x100U : 0U) | (incoming.size == 16 ? 0x200U : 0U));
}

Ipc::Interrupts Ipc::conditions() const noexcept {
    const auto sources = [this](Processor cpu) {
        const auto status = control(cpu);
        return ((status & 5U) == 5U ? 1U << 17U : 0U) |
            ((status & 0x500U) == 0x400U ? 1U << 18U : 0U);
    };
    return {sources(Processor::arm9), sources(Processor::arm7)};
}

Ipc::Interrupts Ipc::rising(Interrupts before) const noexcept {
    const auto after = conditions();
    return {after.arm9 & ~before.arm9, after.arm7 & ~before.arm7};
}

Ipc::Interrupts Ipc::write_control(Processor cpu, std::uint16_t value, std::uint16_t mask) noexcept {
    const auto before = conditions();
    auto& local = endpoint(cpu);
    const auto writable = mask & 0x8404U;
    local.control = static_cast<std::uint16_t>((local.control & ~writable) | (value & writable));
    if (value & mask & 0x4000U) local.error = false;
    if (value & mask & 8U) local.outgoing = {}; // efface aussi le dernier mot reçu à distance
    return rising(before);
}

Ipc::Interrupts Ipc::send(Processor cpu, std::uint32_t value) noexcept {
    auto& local = endpoint(cpu);
    if (!(local.control & 0x8000U)) return {};
    if (local.outgoing.size == 16) { local.error = true; return {}; }
    const auto before = conditions();
    auto& fifo = local.outgoing;
    fifo.words[(fifo.head + fifo.size) % 16U] = value;
    ++fifo.size;
    return rising(before);
}

std::uint32_t Ipc::peek(Processor cpu) const noexcept {
    const auto& fifo = endpoint(other(cpu)).outgoing;
    return fifo.size == 0 ? fifo.last : fifo.words[fifo.head];
}

Ipc::Received Ipc::receive(Processor cpu) noexcept {
    auto& local = endpoint(cpu);
    auto& fifo = endpoint(other(cpu)).outgoing;
    const auto value = peek(cpu);
    if (fifo.size == 0) { local.error = true; return {value, {}}; }
    if (!(local.control & 0x8000U)) return {value, {}};
    const auto before = conditions();
    fifo.last = value;
    fifo.head = (fifo.head + 1U) % 16U;
    --fifo.size;
    return {value, rising(before)};
}

} // namespace ravenemu::nds
