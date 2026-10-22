#include "interpreter.hpp"

namespace ravenemu::nds::detail {

void Interpreter::execute_arm(std::uint32_t raw) {
    const auto cond = raw >> 28U;
    if (cond == 15) {
        if (cpu == Processor::arm9 && (raw & 0xfd70f000U) == 0xf550f000U) {
            // PLD : indication de préchargement ARMv5TE, sans effet architectural
            // ni accès mémoire dans notre modèle sans cache physique.
            if ((raw & 0x02000000U) && ((raw & 0x10U) || (raw & 15U) == 15)) unsupported();
            return;
        }
        if (cpu != Processor::arm9 || (raw & 0xfe000000U) != 0xfa000000U) unsupported();
        // BLX immédiat ARMv5 : le bit H fournit le bit 1 de la cible.
        s.registers[14] = pc + 4U;
        exchange((pc + 8U + sign_extend((raw & 0x00ffffffU) << 2U, 26) +
                  ((raw >> 23U) & 2U)) | 1U);
        return;
    }
    if (!condition(s, cond)) return;
    if ((raw & 0x0fbf0fffU) == 0x010f0000U) {
        arm_psr(raw, true);
    } else if ((raw & 0x0fb0fff0U) == 0x0120f000U || (raw & 0x0fb0f000U) == 0x0320f000U) {
        arm_psr(raw, false);
    } else if ((raw & 0x0f000000U) == 0x0f000000U) {
        enter_exception(s, ram.exception_base(cpu), Exception::software_interrupt, pc + 4U);
    } else if ((raw & 0x0ff000f0U) == 0x07f000f0U) {
        enter_exception(s, ram.exception_base(cpu), Exception::undefined_instruction, pc + 4U);
    } else if ((raw & 0x0ffffff0U) == 0x012fff10U) {
        exchange(reg(raw & 15U));
    } else if ((raw & 0x0ffffff0U) == 0x012fff30U) {
        if (cpu != Processor::arm9 || (raw & 15U) == 15) unsupported();
        const auto target = reg(raw & 15U);
        s.registers[14] = pc + 4U;
        exchange(target);
    } else if ((raw & 0x0fff0ff0U) == 0x016f0f10U) {
        const auto rd = (raw >> 12U) & 15U, rm = raw & 15U;
        if (cpu != Processor::arm9 || rd == 15 || rm == 15) unsupported();
        s.registers[rd] = static_cast<std::uint32_t>(std::countl_zero(reg(rm)));
    } else if ((raw & 0x0f900ff0U) == 0x01000050U) {
        arm_saturating(raw);
    } else if ((raw & 0x0f900090U) == 0x01000080U) {
        arm_dsp_multiply(raw);
    } else if ((raw & 0x0fb00ff0U) == 0x01000090U) {
        arm_swap(raw);
    } else if ((raw & 0x0e000000U) == 0x0a000000U) {
        if ((raw & 0x01000000U) != 0) s.registers[14] = pc + 4U;
        branch(pc + 8U + sign_extend((raw & 0x00ffffffU) << 2U, 26));
    } else if ((raw & 0x0c000000U) == 0x04000000U) {
        arm_transfer(raw);
    } else if ((raw & 0x0e000000U) == 0x08000000U) {
        const bool restore = (raw & 0x00400000U) != 0;
        if (restore && ((raw & 0x00108000U) != 0x00108000U)) unsupported(); // banques utilisateur absentes
        multiple((raw >> 16U) & 15U, raw & 0xffffU, (raw & 0x00100000U) != 0,
                 (raw & 0x01000000U) != 0, (raw & 0x00800000U) != 0, (raw & 0x00200000U) != 0, restore);
    } else if ((raw & 0x0fc000f0U) == 0x00000090U) {
        arm_multiply(raw, false);
    } else if ((raw & 0x0f8000f0U) == 0x00800090U) {
        arm_multiply(raw, true);
    } else if ((raw & 0x0e000090U) == 0x00000090U && (raw & 0x60U) != 0) {
        arm_halfword(raw);
    } else if ((raw & 0x0f000010U) == 0x0e000010U && ((raw >> 8U) & 15U) == 15) {
        arm_cp15(raw);
    } else if ((raw & 0x0c000000U) == 0) {
        arm_data(raw);
    } else unsupported();
}

void Interpreter::arm_data(std::uint32_t raw) {
    const bool immediate = (raw & 0x02000000U) != 0;
    const bool by_register = !immediate && (raw & 0x10U) != 0;
    if (by_register && (raw & 0x80U) != 0) unsupported(); // encodage spécial hors ALU
    const auto opcode = (raw >> 21U) & 15U;
    const bool flags = (raw & 0x00100000U) != 0;
    const bool test = opcode >= 8 && opcode <= 11;
    const auto rd = (raw >> 12U) & 15U;
    if (test && (!flags || rd != 0)) unsupported();
    const bool restore = rd == 15 && flags;
    if (restore && !s.has_spsr()) unsupported();
    const bool update_flags = flags && !restore;
    if (by_register && (rd == 15 || ((raw >> 16U) & 15U) == 15 ||
                        ((raw >> 8U) & 15U) == 15 || (raw & 15U) == 15))
        unsupported(); // les variantes avec PC demandent des règles de pipeline distinctes
    const auto rn = reg((raw >> 16U) & 15U);
    Shifted operand{};
    if (immediate) {
        const auto rotate = ((raw >> 8U) & 15U) * 2U;
        const auto value = std::rotr(raw & 0xffU, static_cast<int>(rotate));
        operand = {value, rotate == 0 ? s.carry : (value >> 31U) != 0};
    } else {
        const auto amount = by_register ? reg((raw >> 8U) & 15U) : (raw >> 7U) & 31U;
        operand = shift(reg(raw & 15U), (raw >> 5U) & 3U, amount, s.carry, by_register);
    }

    std::uint32_t result{};
    std::optional<Result> sum;
    switch (opcode) {
    case 0: case 8: result = rn & operand.value; break;
    case 1: case 9: result = rn ^ operand.value; break;
    case 2: case 10: sum = add(rn, ~operand.value, true); break;
    case 3: sum = add(operand.value, ~rn, true); break;
    case 4: case 11: sum = add(rn, operand.value, false); break;
    case 5: sum = add(rn, operand.value, s.carry); break;
    case 6: sum = add(rn, ~operand.value, s.carry); break;
    case 7: sum = add(operand.value, ~rn, s.carry); break;
    case 12: result = rn | operand.value; break;
    case 13: result = operand.value; break;
    case 14: result = rn & ~operand.value; break;
    case 15: result = ~operand.value; break;
    }
    if (sum) result = update_flags ? arithmetic(*sum) : sum->value;
    else if (update_flags) { nz(result); s.carry = operand.carry; }
    if (!test) {
        if (restore) { s.set_cpsr(s.spsr()); branch(result); }
        else write_reg(rd, result);
    }
}

void Interpreter::arm_transfer(std::uint32_t raw) {
    const bool pre = (raw & 0x01000000U) != 0;
    const bool up = (raw & 0x00800000U) != 0;
    const bool byte = (raw & 0x00400000U) != 0;
    const bool writeback = (raw & 0x00200000U) != 0;
    const bool load = (raw & 0x00100000U) != 0;
    const auto rn = (raw >> 16U) & 15U;
    const auto rd = (raw >> 12U) & 15U;
    const bool register_offset = (raw & 0x02000000U) != 0;
    if ((!pre && writeback) || (rd == 15 && (!load || byte)) ||
        ((!pre || writeback) && (rn == 15 || rn == rd))) unsupported();
    if (register_offset && ((raw & 0x10U) != 0 || (raw & 15U) == 15)) unsupported();
    const auto base = reg(rn);
    const auto offset = register_offset
        ? shift(reg(raw & 15U), (raw >> 5U) & 3U, (raw >> 7U) & 31U, s.carry).value
        : raw & 0xfffU;
    const auto adjusted = up ? base + offset : base - offset;
    const auto address = pre ? adjusted : base;
    if (load) {
        const auto value = byte ? bus.read8(address) : bus.read32(address);
        if (rd == 15) load_pc(value);
        else s.registers[rd] = value;
    }
    else if (byte) bus.write8(address, static_cast<std::uint8_t>(reg(rd)));
    else bus.write32(address, reg(rd));
    if (!pre || writeback) s.registers[rn] = adjusted;
}

void Interpreter::arm_halfword(std::uint32_t raw) {
    const bool pre = (raw & 0x01000000U) != 0;
    const bool up = (raw & 0x00800000U) != 0;
    const bool immediate = (raw & 0x00400000U) != 0;
    const bool writeback = (raw & 0x00200000U) != 0;
    const bool load = (raw & 0x00100000U) != 0;
    const auto rn = (raw >> 16U) & 15U;
    const auto rd = (raw >> 12U) & 15U;
    const auto type = (raw >> 5U) & 3U;
    if (!load && type >= 2) { arm_doubleword(raw); return; }
    if (rd == 15 || (!load && type != 1) || (!pre && writeback) ||
        ((!pre || writeback) && (rn == 15 || rn == rd)) ||
        (!immediate && ((raw & 0xf00U) != 0 || (raw & 15U) == 15))) unsupported();
    const auto offset = immediate ? ((raw >> 4U) & 0xf0U) | (raw & 15U) : reg(raw & 15U);
    const auto base = reg(rn);
    const auto adjusted = up ? base + offset : base - offset;
    const auto address = pre ? adjusted : base;
    if (!load) bus.write16(address, static_cast<std::uint16_t>(reg(rd)));
    else if (type == 1) s.registers[rd] = bus.read16(address);
    else if (type == 2) s.registers[rd] = sign_extend(bus.read8(address), 8);
    else s.registers[rd] = sign_extend(bus.read16(address), 16);
    if (!pre || writeback) s.registers[rn] = adjusted;
}

void Interpreter::arm_multiply(std::uint32_t raw, bool wide) {
    const auto hi = (raw >> 16U) & 15U;
    const auto lo = (raw >> 12U) & 15U;
    const auto rs = (raw >> 8U) & 15U;
    const auto rm = raw & 15U;
    const bool accumulate = (raw & 0x00200000U) != 0;
    const bool flags = (raw & 0x00100000U) != 0;
    if (hi == 15 || rs == 15 || rm == 15 || (wide && (lo == 15 || lo == hi)) ||
        (!wide && ((accumulate && lo == 15) || (!accumulate && lo != 0))) ||
        (cpu == Processor::arm7 && (hi == rm || (wide && lo == rm)))) unsupported();
    if (!wide) {
        const auto value = reg(rm) * reg(rs) + (accumulate ? reg(lo) : 0U);
        s.registers[hi] = value;
        if (flags) nz(value);
    } else {
        const bool signed_product = (raw & 0x00400000U) != 0;
        std::uint64_t value;
        if (signed_product) {
            const auto x = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(reg(rm)));
            const auto y = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(reg(rs)));
            value = static_cast<std::uint64_t>(x * y);
        } else value = static_cast<std::uint64_t>(reg(rm)) * reg(rs);
        if (accumulate) value += (static_cast<std::uint64_t>(reg(hi)) << 32U) | reg(lo);
        s.registers[lo] = static_cast<std::uint32_t>(value);
        s.registers[hi] = static_cast<std::uint32_t>(value >> 32U);
        if (flags) { s.negative = (value >> 63U) != 0; s.zero = value == 0; }
    }
    // C/V conservés dans ce modèle. Pas d'émulation des retenues
    // indéterminées du multiplieur ARM7, ni de ses cycles internes.
}

} // namespace ravenemu::nds::detail
