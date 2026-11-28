#include "gpu/geometry/geometry_engine.hpp"

#include <algorithm>
#include <span>

namespace ravenemu::nds::geometry {

namespace {

constexpr std::uint8_t swap_buffers = 0x50;
constexpr Cycles swap_after_vblank = 392;

// Durées de la table de GBATEK (« DS 3D I/O Map »), en cycles de 33,51 MHz.
// NORMAL : 9 cycles, sans compter encore les lumières actives.
constexpr Cycles base_duration(unsigned command) noexcept {
    switch (command) {
    case 0x10: return 1;
    case 0x11: return 17;
    case 0x12: return 36;
    case 0x13: return 17;
    case 0x14: return 36;
    case 0x15: return 19;
    case 0x16: return 34;
    case 0x17: return 30;
    case 0x18: return 35;
    case 0x19: return 31;
    case 0x1a: return 28;
    case 0x1b:
    case 0x1c: return 22;
    case 0x21:
    case 0x23: return 9;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28: return 8;
    case 0x30:
    case 0x31: return 4;
    case 0x32: return 6;
    case 0x34: return 32;
    case 0x70: return 103;
    case 0x71: return 9;
    case 0x72: return 5;
    default: return 1;  // COLOR, TEXCOORD, attributs, BEGIN/END, VIEWPORT
    }
}

constexpr std::int32_t sign_extend(std::uint32_t value, unsigned bits) noexcept {
    const auto shift = 32U - bits;
    return static_cast<std::int32_t>(value << shift) >> shift;
}

} // namespace

Cycles GeometryEngine::duration(std::uint8_t command) const noexcept {
    const auto extra = matrices_.mode() == 2 && (command == 0x18 || command == 0x19 || command == 0x1a || command == 0x1c);
    return base_duration(command) + (extra ? 30U : 0U);
}

void GeometryEngine::run(Cycles now) {
    while (true) {
        if (busy_until_ > time_) {
            if (busy_until_ > now) break;
            time_ = busy_until_;
        }
        executing_ = 0;
        if (waiting_vblank_ || fifo_.empty()) break;
        const auto command = fifo_.front().command;
        const auto count = entries_of(command);
        if (fifo_.total() < count) break;  // paramètres encore attendus
        std::array<std::uint32_t, 32> parameters{};
        for (std::size_t index = 0; index < count; ++index) {
            parameters[index] = fifo_.front().parameter;
            fifo_.pop();
        }
        executing_ = command;
        busy_until_ = time_ + duration(command);
        execute(command, parameters);
    }
    time_ = std::max(time_, now);
}

void GeometryEngine::on_vblank(Cycles now) {
    run(now);
    if (!waiting_vblank_) return;
    waiting_vblank_ = false;
    executing_ = swap_buffers;
    busy_until_ = now + swap_after_vblank;
}

void GeometryEngine::execute(std::uint8_t command, const std::array<std::uint32_t, 32>& parameters) {
    const std::span<const std::uint32_t> p{parameters};
    switch (command) {
    case 0x10: matrices_.set_mode(p[0]); break;
    case 0x11: matrices_.push(); break;
    case 0x12: matrices_.pop(p[0]); break;
    case 0x13: matrices_.store(p[0]); break;
    case 0x14: matrices_.restore(p[0]); break;
    case 0x15: matrices_.load(identity()); break;
    case 0x16: matrices_.load(from_4x4(p)); break;
    case 0x17: matrices_.load(from_4x3(p)); break;
    case 0x18: matrices_.multiply(from_4x4(p), false); break;
    case 0x19: matrices_.multiply(from_4x3(p), false); break;
    case 0x1a: matrices_.multiply(from_3x3(p), false); break;
    case 0x1b: matrices_.multiply(scale(p), true); break;
    case 0x1c: matrices_.multiply(translation(p), false); break;
    case swap_buffers:
        waiting_vblank_ = true;
        busy_until_ = time_;
        break;
    case 0x70: box_result_ = true; break;
    case 0x71:  // POS_TEST : (x, y, z, 1) × matrice de découpage
        position_result_ = transform({sign_extend(p[0], 16), sign_extend(p[0] >> 16U, 16), sign_extend(p[1], 16), one},
                                     matrices_.clip());
        break;
    case 0x72: {  // VEC_TEST : (x, y, z, 0) × matrice des vecteurs ; 4 bits de signe, 12 de fraction
        const auto x = sign_extend(p[0], 10) * 8;
        const auto y = sign_extend(p[0] >> 10U, 10) * 8;
        const auto z = sign_extend(p[0] >> 20U, 10) * 8;
        const auto result = transform({x, y, z, 0}, matrices_.vector());
        for (unsigned axis = 0; axis < 3; ++axis)
            vector_result_[axis] = static_cast<std::uint16_t>(sign_extend(static_cast<std::uint32_t>(result[axis]), 13));
        break;
    }
    default: break;  // pas encore d'effet
    }
}

void GeometryEngine::push(Entry entry, Cycles now) {
    run(now);
    fifo_.push(entry);
    run(now);
}

Cycles GeometryEngine::write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now) {
    run(now);
    if (offset == 0x200) {  // GXSTAT
        if ((mask & value & 0x8000U) != 0) matrices_.acknowledge_error();
        irq_mode_ = ((irq_mode_ & ~mask) | (value & mask)) & 0xc000'0000U;
        return now;
    }
    if (offset >= 0x200) return now;  // DISP_1DOT_DEPTH, résultats : pas d'écriture
    // Le FIFO plein bloque le CPU jusqu'à ce qu'une commande le libère.
    auto resume = now;
    while (fifo_.full() && busy_until_ > time_) {
        resume = busy_until_;
        run(resume);
    }
    const auto data = value & mask;
    if (offset < 0x40) packed_.write(data, [&](Entry entry) { push(entry, resume); });
    else if (parameter_count(offset / 4U) >= 0) push(Entry{static_cast<std::uint8_t>(offset / 4U), data}, resume);
    return resume;
}

std::uint32_t GeometryEngine::read(std::uint32_t offset, Cycles now) {
    run(now);
    if (offset == 0x200) return status();
    if (offset == 0x204) return 0;  // RAM_COUNT : pas encore de polygones
    if (offset >= 0x220 && offset < 0x230) return static_cast<std::uint32_t>(position_result_[(offset - 0x220) / 4]);
    if (offset >= 0x230 && offset < 0x236) {
        const auto index = (offset - 0x230) / 2;
        const auto next = index + 1 < 3 ? vector_result_[index + 1] : std::uint16_t{0};
        return vector_result_[index] | (std::uint32_t{next} << 16U);
    }
    if (offset >= 0x240 && offset < 0x280) return static_cast<std::uint32_t>(matrices_.clip()[(offset - 0x240) / 4]);
    if (offset >= 0x280 && offset < 0x2a4) {
        const auto index = (offset - 0x280) / 4;  // 3 × 3 : lignes et colonnes 0 à 2
        return static_cast<std::uint32_t>(matrices_.vector()[(index / 3) * 4 + index % 3]);
    }
    return 0;
}

std::uint32_t GeometryEngine::status() const noexcept {
    const auto count = static_cast<std::uint32_t>(fifo_.fifo_count());
    const auto testing = executing_ >= 0x70 && executing_ <= 0x72 && busy_until_ > time_;
    const auto stacking = (executing_ == 0x11 || executing_ == 0x12) && busy_until_ > time_;
    std::uint32_t value = (testing ? 1U : 0U) | (box_result_ ? 2U : 0U);
    value |= (matrices_.coordinate_level() & 31U) << 8U;
    value |= matrices_.projection_level() << 13U;
    value |= (stacking ? 1U : 0U) << 14U;
    value |= (matrices_.error() ? 1U : 0U) << 15U;
    value |= count << 16U;
    value |= (count < CommandFifo::fifo_capacity / 2 ? 1U : 0U) << 25U;
    value |= (count == 0 ? 1U : 0U) << 26U;
    value |= (busy() ? 1U : 0U) << 27U;
    return value | irq_mode_;
}

bool GeometryEngine::irq_condition() const noexcept {
    const auto count = fifo_.fifo_count();
    switch (irq_mode_ >> 30U) {
    case 1: return count < CommandFifo::fifo_capacity / 2;
    case 2: return count == 0;
    default: return false;
    }
}

} // namespace ravenemu::nds::geometry
