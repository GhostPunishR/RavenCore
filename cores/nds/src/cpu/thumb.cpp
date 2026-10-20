#include "interpreter.hpp"

namespace ravenemu::nds::detail {

void Interpreter::execute_thumb(std::uint32_t raw) {
    const auto rd = raw & 7U;
    const auto rs = (raw >> 3U) & 7U;
    if ((raw & 0xf800U) == 0x1800U) {
        const auto y = (raw & 0x0400U) != 0 ? (raw >> 6U) & 7U : reg((raw >> 6U) & 7U);
        const auto x = reg(rs);
        s.registers[rd] = arithmetic((raw & 0x0200U) != 0 ? add(x, ~y, true) : add(x, y, false));
    } else if ((raw & 0xe000U) == 0) {
        const auto result = shift(reg(rs), (raw >> 11U) & 3U, (raw >> 6U) & 31U, s.carry);
        s.registers[rd] = result.value;
        nz(result.value);
        s.carry = result.carry;
    } else if ((raw & 0xe000U) == 0x2000U) {
        const auto r = (raw >> 8U) & 7U;
        const auto value = raw & 0xffU;
        switch ((raw >> 11U) & 3U) {
        case 0: s.registers[r] = value; nz(value); break;
        case 1: arithmetic(add(reg(r), ~value, true)); break;
        case 2: s.registers[r] = arithmetic(add(reg(r), value, false)); break;
        case 3: s.registers[r] = arithmetic(add(reg(r), ~value, true)); break;
        }
    } else if ((raw & 0xfc00U) == 0x4000U) {
        thumb_alu(raw);
    } else if ((raw & 0xfc00U) == 0x4400U) {
        const auto dest = rd | ((raw >> 4U) & 8U);
        const auto source = reg((raw >> 3U) & 15U);
        switch ((raw >> 8U) & 3U) {
        case 0: write_reg(dest, reg(dest) + source); break;
        case 1: arithmetic(add(reg(dest), ~source, true)); break;
        case 2: write_reg(dest, source); break;
        case 3:
            if ((raw & 7U) != 0) unsupported();
            if ((raw & 0x80U) != 0) {
                if (cpu != Processor::arm9 || ((raw >> 3U) & 15U) == 15) unsupported();
                s.registers[14] = (pc + 2U) | 1U;
            }
            exchange(source);
            break;
        }
    } else if ((raw & 0xf800U) == 0x4800U) {
        s.registers[(raw >> 8U) & 7U] = bus.read32(((pc + 4U) & ~3U) + (raw & 0xffU) * 4U);
    } else if ((raw & 0xf000U) == 0x5000U) {
        const auto address = reg(rs) + reg((raw >> 6U) & 7U);
        const auto type = (raw >> 9U) & 7U;
        switch (type) {
        case 0: thumb_transfer(rd, address, 4, false); break;
        case 1: thumb_transfer(rd, address, 2, false); break;
        case 2: thumb_transfer(rd, address, 1, false); break;
        case 3: s.registers[rd] = sign_extend(bus.read8(address), 8); break;
        case 4: thumb_transfer(rd, address, 4, true); break;
        case 5: thumb_transfer(rd, address, 2, true); break;
        case 6: thumb_transfer(rd, address, 1, true); break;
        case 7: s.registers[rd] = sign_extend(bus.read16(address), 16); break;
        }
    } else if ((raw & 0xe000U) == 0x6000U || (raw & 0xf000U) == 0x8000U) {
        const auto width = (raw & 0xf000U) == 0x8000U ? 2U : ((raw & 0x1000U) != 0 ? 1U : 4U);
        const auto address = reg(rs) + ((raw >> 6U) & 31U) * width;
        thumb_transfer(rd, address, width, (raw & 0x0800U) != 0);
    } else if ((raw & 0xf000U) == 0x9000U) {
        thumb_transfer((raw >> 8U) & 7U, reg(13) + (raw & 0xffU) * 4U, 4, (raw & 0x0800U) != 0);
    } else if ((raw & 0xf000U) == 0xa000U) {
        const auto base = (raw & 0x0800U) != 0 ? reg(13) : (pc + 4U) & ~3U;
        s.registers[(raw >> 8U) & 7U] = base + (raw & 0xffU) * 4U;
    } else if ((raw & 0xff00U) == 0xb000U) {
        const auto offset = (raw & 0x7fU) * 4U;
        s.registers[13] = (raw & 0x80U) != 0 ? reg(13) - offset : reg(13) + offset;
    } else if ((raw & 0xf600U) == 0xb400U) {
        const bool load = (raw & 0x0800U) != 0;
        const auto extra = (raw & 0x0100U) != 0 ? (load ? 0x8000U : 0x4000U) : 0U;
        multiple(13, (raw & 0xffU) | extra, load, !load, load, true);
    } else if ((raw & 0xf000U) == 0xc000U) {
        const auto rn = (raw >> 8U) & 7U;
        const auto list = raw & 0xffU;
        const bool load = (raw & 0x0800U) != 0;
        // LDMIA Thumb avec la base dans la liste : garder la base chargée.
        const bool writeback = !(load && (list & (1U << rn)) != 0);
        multiple(rn, list, load, false, true, writeback);
    } else if ((raw & 0xf000U) == 0xd000U) {
        const auto cond = (raw >> 8U) & 15U;
        if (cond == 15) enter_exception(s, ram.exception_base(cpu), Exception::software_interrupt, pc + 2U);
        else if (cond == 14) enter_exception(s, ram.exception_base(cpu), Exception::undefined_instruction, pc + 2U);
        else if (condition(s, cond)) branch(pc + 4U + sign_extend((raw & 0xffU) << 1U, 9));
    } else if ((raw & 0xf800U) == 0xe000U) {
        branch(pc + 4U + sign_extend((raw & 0x7ffU) << 1U, 12));
    } else if ((raw & 0xf800U) == 0xf000U) {
        // Les deux demi-instructions BL sont des pas distincts : après
        // le préfixe LR contient la partie haute signée de la cible.
        s.registers[14] = pc + 4U + sign_extend((raw & 0x7ffU) << 12U, 23);
    } else if ((raw & 0xf800U) == 0xf800U || (raw & 0xf800U) == 0xe800U) {
        const bool exchange_to_arm = (raw & 0xf800U) == 0xe800U;
        if (exchange_to_arm && (cpu != Processor::arm9 || (raw & 1U) != 0)) unsupported();
        const auto target = reg(14) + (raw & 0x7ffU) * 2U;
        s.registers[14] = (pc + 2U) | 1U;
        s.thumb = !exchange_to_arm;
        branch(target);
    } else unsupported();
}

void Interpreter::thumb_transfer(unsigned rd, std::uint32_t address, unsigned width, bool load) {
    if (load) {
        if (width == 1) s.registers[rd] = bus.read8(address);
        else if (width == 2) s.registers[rd] = bus.read16(address);
        else s.registers[rd] = bus.read32(address);
    } else {
        if (width == 1) bus.write8(address, static_cast<std::uint8_t>(reg(rd)));
        else if (width == 2) bus.write16(address, static_cast<std::uint16_t>(reg(rd)));
        else bus.write32(address, reg(rd));
    }
}

void Interpreter::thumb_alu(std::uint32_t raw) {
    const auto rd = raw & 7U;
    const auto opcode = (raw >> 6U) & 15U;
    const auto x = reg(rd);
    const auto y = reg((raw >> 3U) & 7U);
    std::uint32_t result{};
    switch (opcode) {
    case 0: case 8: result = x & y; break;
    case 1: result = x ^ y; break;
    case 2: case 3: case 4: case 7: {
        const auto type = opcode == 7 ? 3U : opcode - 2U;
        const auto shifted = shift(x, type, y, s.carry, true);
        result = shifted.value;
        s.carry = shifted.carry;
        break;
    }
    case 5: result = arithmetic(add(x, y, s.carry)); break;
    case 6: result = arithmetic(add(x, ~y, s.carry)); break;
    case 9: result = arithmetic(add(0, ~y, true)); break;
    case 10: result = arithmetic(add(x, ~y, true)); break;
    case 11: result = arithmetic(add(x, y, false)); break;
    case 12: result = x | y; break;
    case 13: result = x * y; break;
    case 14: result = x & ~y; break;
    case 15: result = ~y; break;
    }
    nz(result);
    if (opcode != 8 && opcode != 10 && opcode != 11) s.registers[rd] = result;
}

} // namespace ravenemu::nds::detail
