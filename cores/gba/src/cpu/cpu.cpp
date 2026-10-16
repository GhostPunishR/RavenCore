#include "cpu/cpu.hpp"

namespace ravenemu::gba {

int Cpu::execute_arm(std::int32_t instruction) {
    const auto raw = u32(instruction);
    if ((raw & 0x0fff'fff0U) == 0x012f'ff10U) {
        branch_exchange(read_reg(static_cast<int>(raw & 15U)));
        return 0;
    }
    switch ((raw >> 25U) & 7U) {
    case 0:
        if ((raw & 0x0fb0'0ff0U) == 0x0100'0090U) return arm_swap(instruction);
        if ((raw & 0x0fc0'00f0U) == 0x0000'0090U) return arm_multiply(instruction);
        if ((raw & 0x0f80'00f0U) == 0x0080'0090U) return arm_multiply_long(instruction);
        if ((raw & 0x0e00'0090U) == 0x0000'0090U && (raw & 0x60U) != 0) return arm_halfword(instruction);
        return arm_data_or_psr(instruction);
    case 1: return arm_data_or_psr(instruction);
    case 2:
    case 3: return arm_single_transfer(instruction);
    case 4: return arm_block(instruction);
    case 5: return arm_branch(instruction);
    case 7: return ((raw >> 24U) & 1U) != 0 ? arm_swi(instruction) : arm_undefined(instruction);
    default: return arm_undefined(instruction);
    }
}

int Cpu::arm_undefined(std::int32_t instruction) {
    bus.diagnostics.report(DiagnosticEvent::undefined_instruction,
                           "instruction ARM indéfinie " + std::to_string(u32(instruction)));
    return 0;
}

int Cpu::arm_branch(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto offset = sign_extend((raw & 0x00ff'ffffU) << 2U, 26);
    if ((raw & 0x0100'0000U) != 0) write_reg(14, add32(state.regs[15], 4));
    branch_to(add32(read_reg(15), offset));
    return 0;
}

int Cpu::arm_data_or_psr(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto immediate = ((raw >> 25U) & 1U) != 0;
    const auto opcode = static_cast<int>((raw >> 21U) & 15U);
    const auto set_flags = ((raw >> 20U) & 1U) != 0;
    if (!set_flags) {
        if (opcode >= 8 && opcode <= 11) return arm_psr(instruction);
        return arm_data_without_flags(raw, opcode);
    }
    return arm_data(instruction, immediate, opcode, set_flags);
}

// Traitement de données sans S, de loin le plus fréquent : ni retenue ni
// débordement à calculer. Fonction courte, séparée de arm_data pour éviter
// à chaque appel le prologue de la version complète.
int Cpu::arm_data_without_flags(std::uint32_t raw, int opcode) {
    const auto rn = static_cast<int>((raw >> 16U) & 15U);
    const auto rd = static_cast<int>((raw >> 12U) & 15U);
    std::uint32_t y{};
    auto pc_extra = 0;
    if (((raw >> 25U) & 1U) != 0) {
        y = std::rotr(raw & 0xffU, static_cast<int>(((raw >> 8U) & 15U) * 2U));
    } else {
        pc_extra = ((raw >> 4U) & 1U) != 0 ? 4 : 0;
        y = u32(arm_operand2(i32(raw), false, pc_extra));
    }
    const auto x = u32(add32(read_reg(rn), rn == 15 ? pc_extra : 0));
    const auto carry_in = state.carry ? 1U : 0U;
    std::uint32_t value{};
    switch (opcode) {
    case 0: value = x & y; break;
    case 1: value = x ^ y; break;
    case 2: value = x - y; break;
    case 3: value = y - x; break;
    case 4: value = x + y; break;
    case 5: value = x + y + carry_in; break;
    case 6: value = x + ~y + carry_in; break;
    case 7: value = y + ~x + carry_in; break;
    case 12: value = x | y; break;
    case 13: value = y; break;
    case 14: value = x & ~y; break;
    default: value = ~y; break;
    }
    write_reg(rd, i32(value));
    // Décalage par registre : 1I. Écrire PC recharge le pipeline, compté
    // par les lectures de l'instruction suivante.
    return pc_extra != 0 ? 1 : 0;
}

int Cpu::arm_data(std::int32_t instruction, bool immediate, int opcode, bool set_flags) {
    const auto raw = u32(instruction);
    const auto rn = static_cast<int>((raw >> 16U) & 15U);
    const auto rd = static_cast<int>((raw >> 12U) & 15U);
    const auto register_shift = !immediate && ((raw >> 4U) & 1U) != 0;
    const auto pc_extra = register_shift ? 4 : 0;
    const auto a = add32(read_reg(rn), rn == 15 ? pc_extra : 0);
    const auto b = arm_operand2(instruction, immediate, pc_extra);
    const auto writes = opcode < 8 || opcode > 11;
    bool arithmetic{};
    std::int32_t result{};
    switch (opcode) {
    case 0: result = a & b; break;
    case 1: result = a ^ b; break;
    case 2:
        arithmetic = true;
        result = add_with_carry(a, ~b, 1);
        break;
    case 3:
        arithmetic = true;
        result = add_with_carry(b, ~a, 1);
        break;
    case 4:
        arithmetic = true;
        result = add_with_carry(a, b, 0);
        break;
    case 5:
        arithmetic = true;
        result = add_with_carry(a, b, state.carry ? 1 : 0);
        break;
    case 6:
        arithmetic = true;
        result = add_with_carry(a, ~b, state.carry ? 1 : 0);
        break;
    case 7:
        arithmetic = true;
        result = add_with_carry(b, ~a, state.carry ? 1 : 0);
        break;
    case 8: result = a & b; break;
    case 9: result = a ^ b; break;
    case 10:
        arithmetic = true;
        result = add_with_carry(a, ~b, 1);
        break;
    case 11:
        arithmetic = true;
        result = add_with_carry(a, b, 0);
        break;
    case 12: result = a | b; break;
    case 13: result = b; break;
    case 14: result = a & ~b; break;
    default: result = ~b; break;
    }
    // Avec S et Rd = PC, l'ARM7TDMI recopie SPSR dans CPSR, y compris pour
    // TST/TEQ/CMP/CMN qui n'écrivent pas de résultat (forme « P » héritée).
    if (set_flags) {
        if (rd == 15 && state.has_spsr())
            state.set_cpsr(state.spsr(), true);
        else {
            set_nz(result);
            state.carry = arithmetic ? last_carry : shifter_carry;
            if (arithmetic) state.overflow = last_overflow;
        }
    }
    if (writes) write_reg(rd, result);
    return register_shift ? 1 : 0;
}

std::int32_t Cpu::arm_operand2(std::int32_t instruction, bool immediate, int pc_extra) {
    const auto raw = u32(instruction);
    if (immediate) {
        const auto value = raw & 0xffU;
        const auto rotate = static_cast<int>(((raw >> 8U) & 15U) * 2U);
        if (rotate == 0) {
            shifter_carry = state.carry;
            return i32(value);
        }
        const auto result = std::rotr(value, rotate);
        shifter_carry = (result & 0x8000'0000U) != 0;
        return i32(result);
    }
    const auto rm = static_cast<int>(raw & 15U);
    const auto type = static_cast<int>((raw >> 5U) & 3U);
    if (((raw >> 4U) & 1U) != 0) {
        const auto rs = static_cast<int>((raw >> 8U) & 15U);
        return shift_register(add32(read_reg(rm), rm == 15 ? pc_extra : 0), type, read_reg(rs) & 0xff);
    }
    return shift_immediate(read_reg(rm), type, static_cast<int>((raw >> 7U) & 31U));
}

int Cpu::arm_psr(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto use_spsr = ((raw >> 22U) & 1U) != 0;
    if (((raw >> 21U) & 1U) == 0) {
        write_reg(static_cast<int>((raw >> 12U) & 15U), use_spsr ? state.spsr() : state.cpsr());
        return 0;
    }
    std::int32_t operand{};
    if (((raw >> 25U) & 1U) != 0) {
        operand = i32(std::rotr(raw & 0xffU, static_cast<int>(((raw >> 8U) & 15U) * 2U)));
    } else
        operand = read_reg(static_cast<int>(raw & 15U));
    const auto fields = static_cast<int>((raw >> 16U) & 15U);
    std::uint32_t mask{};
    if ((fields & 1) != 0) mask |= 0x000000ffU;
    if ((fields & 2) != 0) mask |= 0x0000ff00U;
    if ((fields & 4) != 0) mask |= 0x00ff0000U;
    if ((fields & 8) != 0) mask |= 0xff000000U;
    if (use_spsr) {
        if (state.has_spsr()) state.set_spsr(i32((u32(state.spsr()) & ~mask) | (u32(operand) & mask)));
    } else {
        if (state.mode == CpuState::mode_user) mask &= 0xff000000U;
        state.set_cpsr(i32((u32(state.cpsr()) & ~mask) | (u32(operand) & mask)), (mask & 0xffU) != 0);
    }
    return 0;
}

int Cpu::arm_single_transfer(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto register_offset = ((raw >> 25U) & 1U) != 0;
    const auto pre = ((raw >> 24U) & 1U) != 0;
    const auto add = ((raw >> 23U) & 1U) != 0;
    const auto byte = ((raw >> 22U) & 1U) != 0;
    const auto write_back = ((raw >> 21U) & 1U) != 0;
    const auto load = ((raw >> 20U) & 1U) != 0;
    const auto rn = static_cast<int>((raw >> 16U) & 15U);
    const auto rd = static_cast<int>((raw >> 12U) & 15U);
    auto offset = i32(raw & 0xfffU);
    if (register_offset)
        offset = shift_immediate(read_reg(static_cast<int>(raw & 15U)), static_cast<int>((raw >> 5U) & 3U),
                                 static_cast<int>((raw >> 7U) & 31U));
    const auto base = read_reg(rn);
    const auto delta = add ? offset : i32(0U - u32(offset));
    const auto address = pre ? add32(base, delta) : base;
    const auto new_base = add32(base, delta);
    if (load) {
        const auto value = byte ? i32(static_cast<std::uint32_t>(bus.read8(address))) : load_word_rotated(address);
        if ((!pre || write_back) && rn != rd) write_reg(rn, new_base);
        write_reg(rd, value);
    } else {
        // Stocker PC donne l'adresse de l'instruction + 12 sur l'ARM7TDMI.
        const auto value = rd == 15 ? add32(read_reg(15), 4) : read_reg(rd);
        if (byte)
            bus.write8(address, value & 0xff);
        else
            bus.write32(address, value);
        if (!pre || write_back) write_reg(rn, new_base);
    }
    // LDR : 1S + 1N + 1I ; STR : 2N. Les accès sont comptés par le bus.
    return load ? 1 : 0;
}

int Cpu::arm_multiply(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto rd = static_cast<int>((raw >> 16U) & 15U);
    const auto multiplier = u32(read_reg(static_cast<int>((raw >> 8U) & 15U)));
    const auto accumulate = ((raw >> 21U) & 1U) != 0;
    auto result = i32(u32(read_reg(static_cast<int>(raw & 15U))) * multiplier);
    if (accumulate) result = add32(result, read_reg(static_cast<int>((raw >> 12U) & 15U)));
    write_reg(rd, result);
    if (((raw >> 20U) & 1U) != 0) set_nz(result);
    // 1S + mI, plus 1I pour MLA.
    return multiply_cycles(multiplier, true) + (accumulate ? 1 : 0);
}

int Cpu::arm_multiply_long(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto hi = static_cast<int>((raw >> 16U) & 15U);
    const auto lo = static_cast<int>((raw >> 12U) & 15U);
    const auto rs = static_cast<int>((raw >> 8U) & 15U);
    const auto rm = static_cast<int>(raw & 15U);
    const auto signed_multiply = ((raw >> 22U) & 1U) != 0;
    const auto multiplier = u32(read_reg(rs));
    std::uint64_t product{};
    if (signed_multiply)
        product = static_cast<std::uint64_t>(static_cast<std::int64_t>(read_reg(rm)) * read_reg(rs));
    else
        product = static_cast<std::uint64_t>(u32(read_reg(rm))) * u32(read_reg(rs));
    if (((raw >> 21U) & 1U) != 0) product += (static_cast<std::uint64_t>(u32(read_reg(hi))) << 32U) | u32(read_reg(lo));
    write_reg(lo, i32(static_cast<std::uint32_t>(product)));
    write_reg(hi, i32(static_cast<std::uint32_t>(product >> 32U)));
    if (((raw >> 20U) & 1U) != 0) {
        state.negative = (product >> 63U) != 0;
        state.zero = product == 0;
    }
    // 1S + (m + 1)I, plus 1I pour UMLAL/SMLAL ; m ne s'arrête sur des 1 qu'en signé.
    const auto accumulate = ((raw >> 21U) & 1U) != 0;
    return 1 + multiply_cycles(multiplier, signed_multiply) + (accumulate ? 1 : 0);
}

int Cpu::arm_swap(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto address = read_reg(static_cast<int>((raw >> 16U) & 15U));
    const auto rd = static_cast<int>((raw >> 12U) & 15U);
    const auto source = read_reg(static_cast<int>(raw & 15U));
    if (((raw >> 22U) & 1U) != 0) {
        const auto old = bus.read8(address);
        bus.write8(address, source);
        write_reg(rd, old);
    } else {
        const auto old = load_word_rotated(address);
        bus.write32(address, source);
        write_reg(rd, old);
    }
    return 1;
}

int Cpu::arm_halfword(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto pre = ((raw >> 24U) & 1U) != 0;
    const auto add = ((raw >> 23U) & 1U) != 0;
    const auto immediate = ((raw >> 22U) & 1U) != 0;
    const auto write_back = ((raw >> 21U) & 1U) != 0;
    const auto load = ((raw >> 20U) & 1U) != 0;
    const auto rn = static_cast<int>((raw >> 16U) & 15U);
    const auto rd = static_cast<int>((raw >> 12U) & 15U);
    const auto sh = static_cast<int>((raw >> 5U) & 3U);
    const auto offset =
        immediate ? i32(((raw >> 8U) & 15U) << 4U | (raw & 15U)) : read_reg(static_cast<int>(raw & 15U));
    const auto base = read_reg(rn);
    const auto delta = add ? offset : i32(0U - u32(offset));
    const auto address = pre ? add32(base, delta) : base;
    const auto new_base = add32(base, delta);
    if (load) {
        std::int32_t value{};
        if (sh == 1)
            value = load_halfword(address);
        else if (sh == 2)
            value = sign_extend(static_cast<unsigned>(bus.read8(address)), 8);
        else
            value = load_signed_halfword(address);
        if ((!pre || write_back) && rn != rd) write_reg(rn, new_base);
        write_reg(rd, value);
    } else {
        bus.write16(address, rd == 15 ? add32(read_reg(15), 4) : read_reg(rd));
        if (!pre || write_back) write_reg(rn, new_base);
    }
    return load ? 1 : 0;
}

int Cpu::arm_block(std::int32_t instruction) {
    const auto raw = u32(instruction);
    const auto pre = ((raw >> 24U) & 1U) != 0;
    const auto add = ((raw >> 23U) & 1U) != 0;
    const auto psr_user = ((raw >> 22U) & 1U) != 0;
    const auto write_back = ((raw >> 21U) & 1U) != 0;
    const auto load = ((raw >> 20U) & 1U) != 0;
    const auto rn = static_cast<int>((raw >> 16U) & 15U);
    // Liste vide : l'ARM7TDMI transfère PC seul et déplace la base de 0x40.
    const auto empty = (raw & 0xffffU) == 0;
    const auto list = empty ? 0x8000U : raw & 0xffffU;
    const auto count = std::popcount(list);
    const auto byte_count = empty ? 0x40 : static_cast<std::int32_t>(count) * 4;
    const auto base = read_reg(rn);
    std::int32_t address{};
    if (add)
        address = add32(base, pre ? 4 : 0);
    else
        address = add32(base, -byte_count + (pre ? 0 : 4));
    const auto write_value = add32(base, add ? byte_count : -byte_count);
    const auto loads_pc = load && (list & 0x8000U) != 0;
    // Bit S sans chargement de PC : ce sont les registres du mode utilisateur
    // qui sont transférés, quel que soit le mode courant.
    const auto previous_mode = state.mode;
    if (psr_user && !loads_pc) state.switch_mode(CpuState::mode_user);
    std::optional<std::int32_t> loaded_pc;
    bool first = true;
    for (int reg = 0; reg < 16; ++reg) {
        if ((list & (1U << static_cast<unsigned>(reg))) == 0) continue;
        if (load) {
            const auto value = bus.read32(address);
            if (reg == 15)
                loaded_pc = value;
            else
                state.regs[static_cast<std::size_t>(reg)] = value;
        } else {
            auto value = reg == 15 ? add32(read_reg(15), 4) : read_reg(reg);
            // La base est réécrite après le premier transfert : seule la
            // première valeur stockée est l'ancienne base.
            if (reg == rn && write_back && !first) value = write_value;
            bus.write32(address, value);
        }
        first = false;
        address = add32(address, 4);
    }
    if (psr_user && !loads_pc) state.switch_mode(previous_mode);
    if (write_back && !(load && (list & (1U << static_cast<unsigned>(rn))) != 0)) write_reg(rn, write_value);
    if (loaded_pc) {
        // CPSR est restauré avant le saut : l'alignement de PC suit l'état Thumb rétabli.
        if (psr_user && state.has_spsr()) state.set_cpsr(state.spsr(), true);
        branch_to(*loaded_pc);
    }
    // LDM : nS + 1N + 1I ; STM : (n-1)S + 2N.
    return load ? 1 : 0;
}

int Cpu::arm_swi(std::int32_t instruction) {
    execute_swi(static_cast<int>((u32(instruction) >> 16U) & 0xffU), add32(state.regs[15], 4));
    return 0;
}

void Cpu::thumb_arithmetic_flags(std::int32_t value) noexcept {
    set_nz(value);
    state.carry = last_carry;
    state.overflow = last_overflow;
}

int Cpu::thumb_undefined(int instruction) {
    bus.diagnostics.report(DiagnosticEvent::undefined_instruction,
                           "instruction Thumb indéfinie " + std::to_string(instruction));
    return 0;
}

int Cpu::execute_thumb(int instruction) {
    const auto instr = static_cast<unsigned>(instruction) & 0xffffU;
    // Les formats sont testés dans l'ordre croissant des opcodes : on entre
    // directement dans la chaîne au premier format possible pour les trois
    // bits de poids fort, au lieu de tester tous les formats précédents.
    switch (instr >> 13U) {
    case 0:
        if ((instr & 0xf800U) == 0x1800U) {
            const auto immediate = ((instr >> 10U) & 1U) != 0;
            const auto subtract = ((instr >> 9U) & 1U) != 0;
            const auto operand = static_cast<int>((instr >> 6U) & 7U);
            const auto rs = static_cast<int>((instr >> 3U) & 7U);
            const auto rd = static_cast<int>(instr & 7U);
            const auto b = immediate ? operand : read_reg(operand);
            const auto result = subtract ? add_with_carry(read_reg(rs), ~b, 1) : add_with_carry(read_reg(rs), b, 0);
            write_reg(rd, result);
            thumb_arithmetic_flags(result);
            return 0;
        }
        if ((instr & 0xe000U) == 0) {
            const auto type = static_cast<int>((instr >> 11U) & 3U);
            const auto amount = static_cast<int>((instr >> 6U) & 31U);
            const auto rs = static_cast<int>((instr >> 3U) & 7U);
            const auto rd = static_cast<int>(instr & 7U);
            const auto result = shift_immediate(read_reg(rs), type, amount);
            write_reg(rd, result);
            set_nz(result);
            state.carry = shifter_carry;
            return 0;
        }
        [[fallthrough]];
    case 1:
        if ((instr & 0xe000U) == 0x2000U) {
            const auto opcode = static_cast<int>((instr >> 11U) & 3U);
            const auto rd = static_cast<int>((instr >> 8U) & 7U);
            const auto value = i32(instr & 0xffU);
            std::int32_t result{};
            if (opcode == 0) {
                write_reg(rd, value);
                set_nz(value);
            } else if (opcode == 1) {
                result = add_with_carry(read_reg(rd), ~value, 1);
                thumb_arithmetic_flags(result);
            } else if (opcode == 2) {
                result = add_with_carry(read_reg(rd), value, 0);
                write_reg(rd, result);
                thumb_arithmetic_flags(result);
            } else {
                result = add_with_carry(read_reg(rd), ~value, 1);
                write_reg(rd, result);
                thumb_arithmetic_flags(result);
            }
            return 0;
        }
        [[fallthrough]];
    case 2:
        if ((instr & 0xfc00U) == 0x4000U) {
            const auto opcode = static_cast<int>((instr >> 6U) & 15U);
            const auto rs = static_cast<int>((instr >> 3U) & 7U);
            const auto rd = static_cast<int>(instr & 7U);
            const auto a = read_reg(rd);
            const auto b = read_reg(rs);
            std::int32_t result{};
            bool write = true;
            bool arithmetic = false;
            bool shift = false;
            switch (opcode) {
            case 0: result = a & b; break;
            case 1: result = a ^ b; break;
            case 2:
                result = shift_register(a, 0, b);
                shift = true;
                break;
            case 3:
                result = shift_register(a, 1, b);
                shift = true;
                break;
            case 4:
                result = shift_register(a, 2, b);
                shift = true;
                break;
            case 5:
                result = add_with_carry(a, b, state.carry ? 1 : 0);
                arithmetic = true;
                break;
            case 6:
                result = add_with_carry(a, ~b, state.carry ? 1 : 0);
                arithmetic = true;
                break;
            case 7:
                result = shift_register(a, 3, b);
                shift = true;
                break;
            case 8:
                result = a & b;
                write = false;
                break;
            case 9:
                result = add_with_carry(0, ~b, 1);
                arithmetic = true;
                break;
            case 10:
                result = add_with_carry(a, ~b, 1);
                arithmetic = true;
                write = false;
                break;
            case 11:
                result = add_with_carry(a, b, 0);
                arithmetic = true;
                write = false;
                break;
            case 12: result = a | b; break;
            case 13: result = i32(u32(a) * u32(b)); break;
            case 14: result = a & ~b; break;
            default: result = ~b; break;
            }
            if (write) write_reg(rd, result);
            set_nz(result);
            if (arithmetic) {
                state.carry = last_carry;
                state.overflow = last_overflow;
            } else if (shift)
                state.carry = shifter_carry;
            // MUL : 1S + mI (m selon Rd d'origine) ; décalage par registre : 1S + 1I.
            if (opcode == 13) return multiply_cycles(u32(a), true);
            return shift ? 1 : 0;
        }
        if ((instr & 0xfc00U) == 0x4400U) {
            const auto opcode = static_cast<int>((instr >> 8U) & 3U);
            const auto rd = static_cast<int>((instr & 7U) | (((instr >> 7U) & 1U) << 3U));
            const auto rs = static_cast<int>(((instr >> 3U) & 7U) | (((instr >> 6U) & 1U) << 3U));
            if (opcode == 0)
                write_reg(rd, add32(read_reg(rd), read_reg(rs)));
            else if (opcode == 1)
                thumb_arithmetic_flags(add_with_carry(read_reg(rd), ~read_reg(rs), 1));
            else if (opcode == 2)
                write_reg(rd, read_reg(rs));
            else
                branch_exchange(read_reg(rs));
            return 0;
        }
        if ((instr & 0xf800U) == 0x4800U) {
            const auto rd = static_cast<int>((instr >> 8U) & 7U);
            write_reg(rd, bus.read32(add32(i32(u32(read_reg(15)) & ~2U), i32((instr & 0xffU) << 2U))));
            return 1;
        }
        if ((instr & 0xf200U) == 0x5000U) {
            const auto load = ((instr >> 11U) & 1U) != 0;
            const auto byte = ((instr >> 10U) & 1U) != 0;
            const auto address =
                add32(read_reg(static_cast<int>((instr >> 3U) & 7U)), read_reg(static_cast<int>((instr >> 6U) & 7U)));
            const auto rd = static_cast<int>(instr & 7U);
            if (load)
                write_reg(rd, byte ? bus.read8(address) : load_word_rotated(address));
            else if (byte)
                bus.write8(address, read_reg(rd));
            else
                bus.write32(address, read_reg(rd));
            return load ? 1 : 0;
        }
        if ((instr & 0xf200U) == 0x5200U) {
            const auto h = ((instr >> 11U) & 1U) != 0;
            const auto s = ((instr >> 10U) & 1U) != 0;
            const auto address =
                add32(read_reg(static_cast<int>((instr >> 3U) & 7U)), read_reg(static_cast<int>((instr >> 6U) & 7U)));
            const auto rd = static_cast<int>(instr & 7U);
            if (!s && !h)
                bus.write16(address, read_reg(rd));
            else if (!s)
                write_reg(rd, load_halfword(address));
            else if (!h)
                write_reg(rd, sign_extend(static_cast<unsigned>(bus.read8(address)), 8));
            else
                write_reg(rd, load_signed_halfword(address));
            return !s && !h ? 0 : 1;
        }
        [[fallthrough]];
    case 3:
        if ((instr & 0xe000U) == 0x6000U) {
            const auto byte = ((instr >> 12U) & 1U) != 0;
            const auto load = ((instr >> 11U) & 1U) != 0;
            const auto offset = static_cast<int>((instr >> 6U) & 31U);
            const auto rb = static_cast<int>((instr >> 3U) & 7U);
            const auto rd = static_cast<int>(instr & 7U);
            const auto address = add32(read_reg(rb), byte ? offset : offset << 2);
            if (load)
                write_reg(rd, byte ? bus.read8(address) : load_word_rotated(address));
            else if (byte)
                bus.write8(address, read_reg(rd));
            else
                bus.write32(address, read_reg(rd));
            return load ? 1 : 0;
        }
        [[fallthrough]];
    case 4:
        if ((instr & 0xf000U) == 0x8000U) {
            const auto load = ((instr >> 11U) & 1U) != 0;
            const auto rb = static_cast<int>((instr >> 3U) & 7U);
            const auto rd = static_cast<int>(instr & 7U);
            const auto address = add32(read_reg(rb), static_cast<int>((instr >> 5U) & 0x3eU));
            if (load)
                write_reg(rd, load_halfword(address));
            else
                bus.write16(address, read_reg(rd));
            return load ? 1 : 0;
        }
        if ((instr & 0xf000U) == 0x9000U) {
            const auto load = ((instr >> 11U) & 1U) != 0;
            const auto rd = static_cast<int>((instr >> 8U) & 7U);
            const auto address = add32(read_reg(13), static_cast<int>((instr & 0xffU) << 2U));
            if (load)
                write_reg(rd, load_word_rotated(address));
            else
                bus.write32(address, read_reg(rd));
            return load ? 1 : 0;
        }
        [[fallthrough]];
    case 5:
        if ((instr & 0xf000U) == 0xa000U) {
            const auto rd = static_cast<int>((instr >> 8U) & 7U);
            const auto base = ((instr >> 11U) & 1U) != 0 ? read_reg(13) : i32(u32(read_reg(15)) & ~2U);
            write_reg(rd, add32(base, static_cast<int>((instr & 0xffU) << 2U)));
            return 0;
        }
        if ((instr & 0xff00U) == 0xb000U) {
            const auto offset = static_cast<std::int32_t>((instr & 0x7fU) << 2U);
            write_reg(13, add32(read_reg(13), ((instr >> 7U) & 1U) != 0 ? -offset : offset));
            return 0;
        }
        if ((instr & 0xf600U) == 0xb400U) {
            const auto load = ((instr >> 11U) & 1U) != 0;
            const auto extra = ((instr >> 8U) & 1U) != 0;
            const auto list = instr & 0xffU;
            const auto count = static_cast<int>(std::popcount(list)) + (extra ? 1 : 0);
            if (load) {
                auto address = read_reg(13);
                for (int reg = 0; reg < 8; ++reg)
                    if ((list & (1U << reg)) != 0) {
                        write_reg(reg, bus.read32(address));
                        address = add32(address, 4);
                    }
                if (extra) {
                    write_reg(15, bus.read32(address));
                    address = add32(address, 4);
                }
                write_reg(13, address);
            } else {
                auto address = add32(read_reg(13), -count * 4);
                write_reg(13, address);
                for (int reg = 0; reg < 8; ++reg)
                    if ((list & (1U << reg)) != 0) {
                        bus.write32(address, read_reg(reg));
                        address = add32(address, 4);
                    }
                if (extra) bus.write32(address, read_reg(14));
            }
            // POP : nS + 1N + 1I ; PUSH : (n-1)S + 2N.
            return load ? 1 : 0;
        }
        [[fallthrough]];
    case 6:
        if ((instr & 0xf000U) == 0xc000U) {
            const auto load = ((instr >> 11U) & 1U) != 0;
            const auto rb = static_cast<int>((instr >> 8U) & 7U);
            const auto list = instr & 0xffU;
            const auto base = read_reg(rb);
            // Liste vide : PC seul est transféré (stocké à l'adresse + 6) et la
            // base avance de 0x40, comme en ARM.
            if (list == 0) {
                if (load)
                    branch_to(bus.read32(base));
                else
                    bus.write32(base, add32(read_reg(15), 2));
                write_reg(rb, add32(base, 0x40));
                return load ? 1 : 0;
            }
            const auto final_base = add32(base, static_cast<std::int32_t>(std::popcount(list)) * 4);
            auto address = base;
            bool first = true;
            for (int reg = 0; reg < 8; ++reg)
                if ((list & (1U << reg)) != 0) {
                    if (load) write_reg(reg, bus.read32(address));
                    // La base est réécrite après le premier transfert : seule la
                    // première valeur stockée est l'ancienne base.
                    else
                        bus.write32(address, reg == rb && !first ? final_base : read_reg(reg));
                    first = false;
                    address = add32(address, 4);
                }
            if (!(load && (list & (1U << rb)) != 0)) write_reg(rb, final_base);
            return load ? 1 : 0;
        }
        if ((instr & 0xff00U) == 0xdf00U) {
            execute_swi(static_cast<int>(instr & 0xffU), add32(state.regs[15], 2));
            return 0;
        }
        if ((instr & 0xf000U) == 0xd000U) {
            const auto condition = static_cast<int>((instr >> 8U) & 15U);
            if (condition >= 14 || !check_condition(condition)) return 0;
            branch_to(add32(read_reg(15), sign_extend((instr & 0xffU) << 1U, 9)));
            return 0;
        }
        [[fallthrough]];
    case 7:
        if ((instr & 0xf800U) == 0xe000U) {
            branch_to(add32(read_reg(15), sign_extend((instr & 0x7ffU) << 1U, 12)));
            return 0;
        }
        if ((instr & 0xf800U) == 0xf000U) {
            write_reg(14, add32(read_reg(15), sign_extend((instr & 0x7ffU) << 12U, 23)));
            return 0;
        }
        if ((instr & 0xf800U) == 0xf800U) {
            const auto target = add32(read_reg(14), static_cast<int>((instr & 0x7ffU) << 1U));
            write_reg(14, i32(u32(add32(state.regs[15], 2)) | 1U));
            branch_to(target);
            return 0;
        }
    default: break;
    }
    return thumb_undefined(instruction);
}

}  // namespace ravenemu::gba
