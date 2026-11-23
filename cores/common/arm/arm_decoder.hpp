#pragma once

#include <array>

// Décodage des instructions ARM : une table de 4 096 entrées, indexée par les
// bits 27-20 et 7-4 de l'instruction, calculée à la compilation pour chaque
// architecture. Classement d'après GBATEK, « ARM Instruction Summary ».
// Inclus par cpu.hpp.
namespace ravenemu::arm {

enum class ArmOp : std::uint8_t {
    data_immediate,
    data_shift_immediate,
    data_shift_register,
    status_read,
    status_write_register,
    status_write_immediate,
    branch_exchange,
    branch_link_exchange,  // ARMv5
    count_leading_zeros,   // ARMv5
    saturating,            // ARMv5TE
    breakpoint,            // ARMv5
    signed_multiply,       // ARMv5TE
    multiply,
    multiply_long,
    swap,
    halfword_transfer,
    single_transfer_immediate,
    single_transfer_register,
    block_transfer,
    branch,
    coprocessor_register,
    software_interrupt,
    undefined,
};

namespace detail {

// Espace « divers » : TST, TEQ, CMP et CMN sans S (bits 27-20 = 0001 0xx0).
constexpr ArmOp decode_arm_misc(std::uint32_t high, std::uint32_t low, bool armv5) noexcept {
    const auto op = (high >> 1U) & 3U;  // bits 22-21
    if (low == 0x0) return (op & 1U) != 0 ? ArmOp::status_write_register : ArmOp::status_read;
    if (low == 0x1 && op == 1) return ArmOp::branch_exchange;
    if (!armv5) return ArmOp::undefined;
    if (low == 0x1 && op == 3) return ArmOp::count_leading_zeros;
    if (low == 0x3 && op == 1) return ArmOp::branch_link_exchange;
    if (low == 0x5) return ArmOp::saturating;
    if (low == 0x7 && op == 1) return ArmOp::breakpoint;
    if ((low & 0x9U) == 0x8U) return ArmOp::signed_multiply;
    return ArmOp::undefined;
}

constexpr ArmOp decode_arm_group0(std::uint32_t high, std::uint32_t low, bool armv5) noexcept {
    if (low == 0x9) {
        if ((high & 0xfcU) == 0x00U) return ArmOp::multiply;
        if ((high & 0xf8U) == 0x08U) return ArmOp::multiply_long;
        if ((high & 0xfbU) == 0x10U) return ArmOp::swap;
        return ArmOp::undefined;
    }
    if ((low & 0x9U) == 0x9U) return ArmOp::halfword_transfer;
    if ((high & 0xf9U) == 0x10U) return decode_arm_misc(high, low, armv5);
    return (low & 1U) != 0 ? ArmOp::data_shift_register : ArmOp::data_shift_immediate;
}

// `high` : bits 27-20 ; `low` : bits 7-4.
constexpr ArmOp decode_arm(std::uint32_t high, std::uint32_t low, bool armv5) noexcept {
    switch (high >> 5U) {
    case 0: return decode_arm_group0(high, low, armv5);
    case 1:
        if ((high & 0xfbU) == 0x32U) return ArmOp::status_write_immediate;
        if ((high & 0xfbU) == 0x30U) return ArmOp::undefined;
        return ArmOp::data_immediate;
    case 2: return ArmOp::single_transfer_immediate;
    case 3: return (low & 1U) != 0 ? ArmOp::undefined : ArmOp::single_transfer_register;
    case 4: return ArmOp::block_transfer;
    case 5: return ArmOp::branch;
    case 6: return ArmOp::undefined;  // LDC, STC : aucun coprocesseur ne les prend
    default:
        if ((high & 0x10U) != 0) return ArmOp::software_interrupt;
        return (low & 1U) != 0 ? ArmOp::coprocessor_register : ArmOp::undefined;  // MRC/MCR, CDP
    }
}

template <bool Armv5>
inline constexpr auto arm_table = [] {
    std::array<ArmOp, 4096> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) table[index] = decode_arm(index >> 4U, index & 15U, Armv5);
    return table;
}();

} // namespace detail

template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::execute_arm(std::uint32_t opcode) {
    const auto index = ((opcode >> 16U) & 0xff0U) | ((opcode >> 4U) & 15U);
    switch (detail::arm_table<armv5>[index]) {
    case ArmOp::data_immediate: arm_data_immediate(opcode); break;
    case ArmOp::data_shift_immediate: arm_data_shift_immediate(opcode); break;
    case ArmOp::data_shift_register: arm_data_shift_register(opcode); break;
    case ArmOp::status_read: arm_status_read(opcode); break;
    case ArmOp::status_write_register: arm_status_write(opcode, regs.r[opcode & 15U]); break;
    case ArmOp::status_write_immediate:
        arm_status_write(opcode, std::rotr(opcode & 0xffU, static_cast<int>(((opcode >> 8U) & 15U) * 2U)));
        break;
    case ArmOp::branch_exchange: branch_exchange(regs.r[opcode & 15U]); break;
    case ArmOp::branch_link_exchange: {
        const auto target = regs.r[opcode & 15U];
        regs.r[14] = current_ + 4;
        branch_exchange(target);
        break;
    }
    case ArmOp::count_leading_zeros:
        write_register((opcode >> 12U) & 15U, static_cast<std::uint32_t>(std::countl_zero(regs.r[opcode & 15U])));
        break;
    case ArmOp::saturating: arm_saturating(opcode); break;
    case ArmOp::breakpoint: breakpoint(); break;
    case ArmOp::signed_multiply: arm_signed_multiply(opcode); break;
    case ArmOp::multiply: arm_multiply(opcode); break;
    case ArmOp::multiply_long: arm_multiply_long(opcode); break;
    case ArmOp::swap: arm_swap(opcode); break;
    case ArmOp::halfword_transfer: arm_halfword_transfer(opcode); break;
    case ArmOp::single_transfer_immediate: arm_single_transfer(opcode, opcode & 0xfffU); break;
    case ArmOp::single_transfer_register:
        arm_single_transfer(opcode, shift_by_immediate(regs.r[opcode & 15U], opcode >> 5U, (opcode >> 7U) & 31U).value);
        break;
    case ArmOp::block_transfer: arm_block_transfer(opcode); break;
    case ArmOp::branch: arm_branch(opcode); break;
    case ArmOp::coprocessor_register: arm_coprocessor_register(opcode); break;
    case ArmOp::software_interrupt: software_interrupt(opcode & 0x00ff'ffffU); break;
    case ArmOp::undefined: undefined_instruction(); break;
    }
}

} // namespace ravenemu::arm
