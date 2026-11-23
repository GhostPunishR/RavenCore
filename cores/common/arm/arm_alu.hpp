#pragma once

// Traitement de données et accès aux PSR (GBATEK, « ARM Opcodes: Data
// Processing (ALU) » et « PSR Transfer (MRS, MSR) »). Inclus par cpu.hpp.
namespace ravenemu::arm {

template <Architecture Arch, Bus BusType>
auto Cpu<Arch, BusType>::shift_by_immediate(std::uint32_t value, std::uint32_t type, std::uint32_t amount) const noexcept
    -> Shifted {
    switch (type & 3U) {
    case 0:  // LSL#0 : opérande inchangé, retenue inchangée
        if (amount == 0) return {value, regs.c};
        return {value << amount, ((value >> (32U - amount)) & 1U) != 0};
    case 1:  // LSR#0 vaut LSR#32
        if (amount == 0) return {0, (value >> 31U) != 0};
        return {value >> amount, ((value >> (amount - 1U)) & 1U) != 0};
    case 2:  // ASR#0 vaut ASR#32
        if (amount == 0) {
            const auto fill = (value >> 31U) != 0;
            return {fill ? 0xffff'ffffU : 0U, fill};
        }
        return {detail::sign_extend(value >> amount, 32U - amount), ((value >> (amount - 1U)) & 1U) != 0};
    default:  // ROR#0 vaut RRX
        if (amount == 0) return {(value >> 1U) | (regs.c ? 0x8000'0000U : 0U), (value & 1U) != 0};
        return {std::rotr(value, static_cast<int>(amount)), ((value >> (amount - 1U)) & 1U) != 0};
    }
}

// Décalage par registre : seuls les 8 bits bas comptent ; 0 laisse l'opérande
// et la retenue inchangés.
template <Architecture Arch, Bus BusType>
auto Cpu<Arch, BusType>::shift_by_register(std::uint32_t value, std::uint32_t type, std::uint32_t amount) const noexcept
    -> Shifted {
    amount &= 0xffU;
    if (amount == 0) return {value, regs.c};
    switch (type & 3U) {
    case 0:
        if (amount < 32) return {value << amount, ((value >> (32U - amount)) & 1U) != 0};
        return {0, amount == 32 && (value & 1U) != 0};
    case 1:
        if (amount < 32) return {value >> amount, ((value >> (amount - 1U)) & 1U) != 0};
        return {0, amount == 32 && (value >> 31U) != 0};
    case 2:
        if (amount < 32) return {detail::sign_extend(value >> amount, 32U - amount), ((value >> (amount - 1U)) & 1U) != 0};
        return {(value >> 31U) != 0 ? 0xffff'ffffU : 0U, (value >> 31U) != 0};
    default: {
        const auto rotate = amount & 31U;
        if (rotate == 0) return {value, (value >> 31U) != 0};
        return {std::rotr(value, static_cast<int>(rotate)), ((value >> (rotate - 1U)) & 1U) != 0};
    }
    }
}

// Addition à 32 bits avec retenue ; règle N, Z, C et V si `set_flags`.
// Une soustraction a - b s'écrit a + ~b + 1.
template <Architecture Arch, Bus BusType>
std::uint32_t Cpu<Arch, BusType>::add_with_flags(std::uint32_t left, std::uint32_t right, bool carry_in,
                                                 bool set_flags) noexcept {
    const auto sum = static_cast<std::uint64_t>(left) + right + (carry_in ? 1U : 0U);
    const auto result = static_cast<std::uint32_t>(sum);
    if (set_flags) {
        set_nz(result);
        regs.c = (sum >> 32U) != 0;
        regs.v = (((left ^ result) & (right ^ result)) >> 31U) != 0;
    }
    return result;
}

template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_data_immediate(std::uint32_t opcode) {
    const auto rotate = ((opcode >> 8U) & 15U) * 2U;
    const auto value = std::rotr(opcode & 0xffU, static_cast<int>(rotate));
    arm_data(opcode, {value, rotate == 0 ? regs.c : (value >> 31U) != 0}, false);
}

template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_data_shift_immediate(std::uint32_t opcode) {
    const auto rm = regs.r[opcode & 15U];
    arm_data(opcode, shift_by_immediate(rm, opcode >> 5U, (opcode >> 7U) & 31U), false);
}

// Décalage par registre : un cycle interne, et R15 lu comme PC + 12.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_data_shift_register(std::uint32_t opcode) {
    bus_.internal(1);
    const auto rm_index = opcode & 15U;
    const auto rm = rm_index == 15 ? regs.r[15] + 4 : regs.r[rm_index];
    const auto rs = regs.r[(opcode >> 8U) & 15U];
    arm_data(opcode, shift_by_register(rm, opcode >> 5U, rs), true);
}

template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_data(std::uint32_t opcode, Shifted operand, bool pc_plus_12) {
    const auto rn_index = (opcode >> 16U) & 15U;
    const auto rd = (opcode >> 12U) & 15U;
    const auto set_flags = ((opcode >> 20U) & 1U) != 0;
    const auto rn = rn_index == 15 && pc_plus_12 ? regs.r[15] + 4 : regs.r[rn_index];
    const auto op2 = operand.value;
    const auto code = (opcode >> 21U) & 15U;
    const auto writes = code < 8 || code > 11;  // TST, TEQ, CMP et CMN n'écrivent pas Rd
    // Avec Rd = R15 et S, le CPSR reprend le SPSR au lieu des drapeaux.
    const auto restores_cpsr = set_flags && writes && rd == 15;
    const auto flags = set_flags && !restores_cpsr;
    std::uint32_t result{};
    bool logical = true;
    switch (code) {
    case 0x0: result = rn & op2; break;
    case 0x1: result = rn ^ op2; break;
    case 0x2: result = add_with_flags(rn, ~op2, true, flags); logical = false; break;
    case 0x3: result = add_with_flags(op2, ~rn, true, flags); logical = false; break;
    case 0x4: result = add_with_flags(rn, op2, false, flags); logical = false; break;
    case 0x5: result = add_with_flags(rn, op2, regs.c, flags); logical = false; break;
    case 0x6: result = add_with_flags(rn, ~op2, regs.c, flags); logical = false; break;
    case 0x7: result = add_with_flags(op2, ~rn, regs.c, flags); logical = false; break;
    case 0x8: result = rn & op2; break;
    case 0x9: result = rn ^ op2; break;
    case 0xa: result = add_with_flags(rn, ~op2, true, flags); logical = false; break;
    case 0xb: result = add_with_flags(rn, op2, false, flags); logical = false; break;
    case 0xc: result = rn | op2; break;
    case 0xd: result = op2; break;
    case 0xe: result = rn & ~op2; break;
    default: result = ~op2; break;
    }
    // Opérations logiques : V inchangé, C vient du décalage.
    if (logical && flags) {
        set_nz(result);
        regs.c = operand.carry;
    }
    if (!writes) return;
    if (restores_cpsr) regs.set_cpsr(regs.spsr(), 0xffff'ffffU);
    write_register(rd, result);
}

template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_status_read(std::uint32_t opcode) {
    const auto spsr = ((opcode >> 22U) & 1U) != 0;
    regs.r[(opcode >> 12U) & 15U] = spsr ? regs.spsr() : regs.cpsr();
}

// MSR : seuls les champs choisis s'écrivent ; en mode utilisateur, seuls les
// drapeaux. Le bit T ne change pas par MSR. Q n'existe qu'en ARMv5TE.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_status_write(std::uint32_t opcode, std::uint32_t value) {
    std::uint32_t fields{};
    if ((opcode & (1U << 19U)) != 0) fields |= 0xff00'0000U;
    if ((opcode & (1U << 18U)) != 0) fields |= 0x00ff'0000U;
    if ((opcode & (1U << 17U)) != 0) fields |= 0x0000'ff00U;
    if ((opcode & (1U << 16U)) != 0) fields |= 0x0000'00ffU;
    constexpr auto flags = armv5 ? 0xf800'0000U : 0xf000'0000U;
    if (((opcode >> 22U) & 1U) != 0) {
        regs.set_spsr(value, fields & (flags | 0xffU));
        return;
    }
    const auto control = regs.mode == mode::user ? 0U : 0xffU & ~psr::thumb;
    regs.set_cpsr(value, fields & (flags | control));
}

} // namespace ravenemu::arm
