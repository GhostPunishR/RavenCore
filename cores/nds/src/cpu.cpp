#include <ravenemu/nds/cpu.hpp>

#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ravenemu::nds {
namespace {

std::string fault_message(Processor cpu, std::uint32_t address, bool thumb,
                          std::optional<std::uint32_t> instruction, const std::string& reason) {
    std::ostringstream out;
    out << "NDS " << (cpu == Processor::arm9 ? "ARM9" : "ARM7") << (thumb ? " Thumb" : " ARM")
        << " PC=0x" << std::hex << std::setw(8) << std::setfill('0') << address;
    if (instruction) out << " instruction=0x" << std::setw(thumb ? 4 : 8) << *instruction;
    out << " : " << reason;
    return out.str();
}

std::uint32_t sign_extend(std::uint32_t value, unsigned bits) {
    const auto sign = std::uint32_t{1} << (bits - 1U);
    return (value ^ sign) - sign;
}

struct Result {
    std::uint32_t value;
    bool carry;
    bool overflow;
};

Result add(std::uint32_t x, std::uint32_t y, bool carry) {
    const auto sum = static_cast<std::uint64_t>(x) + y + (carry ? 1U : 0U);
    const auto signed_sum = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(x)) +
                            std::bit_cast<std::int32_t>(y) + (carry ? 1 : 0);
    return {static_cast<std::uint32_t>(sum), (sum >> 32U) != 0,
            signed_sum < std::numeric_limits<std::int32_t>::min() ||
            signed_sum > std::numeric_limits<std::int32_t>::max()};
}

bool condition(const CpuState& s, unsigned code) {
    switch (code) {
    case 0: return s.zero;
    case 1: return !s.zero;
    case 2: return s.carry;
    case 3: return !s.carry;
    case 4: return s.negative;
    case 5: return !s.negative;
    case 6: return s.overflow;
    case 7: return !s.overflow;
    case 8: return s.carry && !s.zero;
    case 9: return !s.carry || s.zero;
    case 10: return s.negative == s.overflow;
    case 11: return s.negative != s.overflow;
    case 12: return !s.zero && s.negative == s.overflow;
    case 13: return s.zero || s.negative != s.overflow;
    case 14: return true;
    default: return false;
    }
}

struct Shifted {
    std::uint32_t value;
    bool carry;
};

Shifted shift(std::uint32_t value, unsigned type, unsigned amount, bool carry, bool by_register = false) {
    if (by_register) {
        amount &= 0xffU;
        if (amount == 0) return {value, carry};
    } else if (amount == 0) {
        if (type == 0) return {value, carry};
        if (type == 3) return {(value >> 1U) | (carry ? 0x80000000U : 0U), (value & 1U) != 0};
        amount = 32;
    }
    switch (type) {
    case 0:
        if (amount > 32) return {0, false};
        return {amount == 32 ? 0U : value << amount, ((value >> (32U - amount)) & 1U) != 0};
    case 1:
        if (amount > 32) return {0, false};
        return {amount == 32 ? 0U : value >> amount, ((value >> (amount - 1U)) & 1U) != 0};
    case 2: {
        const bool negative = (value & 0x80000000U) != 0;
        if (amount >= 32) return {negative ? 0xffffffffU : 0U, negative};
        const auto fill = negative ? 0xffffffffU << (32U - amount) : 0U;
        return {(value >> amount) | fill, ((value >> (amount - 1U)) & 1U) != 0};
    }
    default: {
        const auto rotated = std::rotr(value, static_cast<int>(amount & 31U));
        return {rotated, (rotated >> 31U) != 0};
    }
    }
}

// Pas de référence persistante au bus : copier/déplacer une Machine ne laisse
// aucun CPU lié à la mémoire de l'ancienne instance.
class Interpreter {
public:
    Interpreter(CpuState state, Processor processor, Memory& memory)
        : s(state), cpu(processor), ram(memory), pc(state.registers[15]) {}

    CpuState s;

    void execute(std::uint32_t raw) {
        s.registers[15] = pc + (s.thumb ? 2U : 4U);
        if (s.thumb) execute_thumb(raw);
        else execute_arm(raw);
        ++s.instructions;
    }

private:
    Processor cpu;
    Memory& ram;
    std::uint32_t pc;

    [[noreturn]] static void unsupported() { throw Error("instruction ou variante non implémentée"); }
    [[nodiscard]] std::uint32_t reg(unsigned r) const {
        return r == 15 ? pc + (s.thumb ? 4U : 8U) : s.registers[r];
    }
    void branch(std::uint32_t address) { s.registers[15] = address & (s.thumb ? ~1U : ~3U); }
    void exchange(std::uint32_t address) {
        s.thumb = (address & 1U) != 0;
        branch(address);
    }
    void load_pc(std::uint32_t address) {
        // ARMv5 : LDR/LDM/POP vers PC prennent aussi le bit T de l'adresse.
        // ARMv4T : ils conservent le mode courant ; seul BX l'échange.
        if (cpu == Processor::arm9) exchange(address);
        else branch(address);
    }
    void nz(std::uint32_t value) {
        s.negative = (value >> 31U) != 0;
        s.zero = value == 0;
    }
    std::uint32_t arithmetic(Result result) {
        nz(result.value);
        s.carry = result.carry;
        s.overflow = result.overflow;
        return result.value;
    }
    void write_reg(unsigned r, std::uint32_t value) {
        if (r == 15) branch(value);
        else s.registers[r] = value;
    }

    void execute_arm(std::uint32_t raw) {
        const auto cond = raw >> 28U;
        if (cond == 15) {
            if (cpu != Processor::arm9 || (raw & 0xfe000000U) != 0xfa000000U) unsupported();
            // BLX immédiat ARMv5 : le bit H fournit le bit 1 de la cible.
            s.registers[14] = pc + 4U;
            exchange((pc + 8U + sign_extend((raw & 0x00ffffffU) << 2U, 26) +
                      ((raw >> 23U) & 2U)) | 1U);
            return;
        }
        if (!condition(s, cond)) return;
        if ((raw & 0x0ffffff0U) == 0x012fff10U) {
            exchange(reg(raw & 15U));
        } else if ((raw & 0x0ffffff0U) == 0x012fff30U) {
            if (cpu != Processor::arm9 || (raw & 15U) == 15) unsupported();
            const auto target = reg(raw & 15U);
            s.registers[14] = pc + 4U;
            exchange(target);
        } else if ((raw & 0x0e000000U) == 0x0a000000U) {
            if ((raw & 0x01000000U) != 0) s.registers[14] = pc + 4U;
            branch(pc + 8U + sign_extend((raw & 0x00ffffffU) << 2U, 26));
        } else if ((raw & 0x0c000000U) == 0x04000000U) {
            arm_transfer(raw);
        } else if ((raw & 0x0e000000U) == 0x08000000U) {
            if ((raw & 0x00400000U) != 0) unsupported(); // banques utilisateur / restauration SPSR
            multiple((raw >> 16U) & 15U, raw & 0xffffU, (raw & 0x00100000U) != 0,
                     (raw & 0x01000000U) != 0, (raw & 0x00800000U) != 0, (raw & 0x00200000U) != 0);
        } else if ((raw & 0x0c000000U) == 0) {
            arm_data(raw);
        } else unsupported();
    }

    void arm_data(std::uint32_t raw) {
        const bool immediate = (raw & 0x02000000U) != 0;
        // Bit 4 : décalage par registre, multiplications et encodages spéciaux.
        if (!immediate && (raw & 0x10U) != 0) unsupported();
        const auto opcode = (raw >> 21U) & 15U;
        const bool flags = (raw & 0x00100000U) != 0;
        const bool test = opcode >= 8 && opcode <= 11;
        const auto rd = (raw >> 12U) & 15U;
        if ((test && (!flags || rd != 0)) || (rd == 15 && flags)) unsupported();
        const auto rn = reg((raw >> 16U) & 15U);
        Shifted operand{};
        if (immediate) {
            const auto rotate = ((raw >> 8U) & 15U) * 2U;
            const auto value = std::rotr(raw & 0xffU, static_cast<int>(rotate));
            operand = {value, rotate == 0 ? s.carry : (value >> 31U) != 0};
        } else operand = shift(reg(raw & 15U), (raw >> 5U) & 3U, (raw >> 7U) & 31U, s.carry);

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
        if (sum) result = flags ? arithmetic(*sum) : sum->value;
        else if (flags) { nz(result); s.carry = operand.carry; }
        if (!test) write_reg(rd, result);
    }

    void arm_transfer(std::uint32_t raw) {
        const bool pre = (raw & 0x01000000U) != 0;
        const bool up = (raw & 0x00800000U) != 0;
        const bool byte = (raw & 0x00400000U) != 0;
        const bool writeback = (raw & 0x00200000U) != 0;
        const bool load = (raw & 0x00100000U) != 0;
        const auto rn = (raw >> 16U) & 15U;
        const auto rd = (raw >> 12U) & 15U;
        if ((raw & 0x02000000U) != 0 || (!pre && writeback) || (rd == 15 && (!load || byte)) ||
            ((!pre || writeback) && (rn == 15 || rn == rd))) unsupported();
        const auto base = reg(rn);
        const auto offset = raw & 0xfffU;
        const auto adjusted = up ? base + offset : base - offset;
        const auto address = pre ? adjusted : base;
        if (load) {
            const auto value = byte ? ram.read8(cpu, address) : ram.read32(cpu, address);
            if (rd == 15) load_pc(value);
            else s.registers[rd] = value;
        }
        else if (byte) ram.write8(cpu, address, static_cast<std::uint8_t>(reg(rd)));
        else ram.write32(cpu, address, reg(rd));
        if (!pre || writeback) s.registers[rn] = adjusted;
    }

    void multiple(unsigned rn, std::uint32_t list, bool load, bool pre, bool up, bool writeback) {
        if (rn == 15 || list == 0 || (!load && (list & 0x8000U) != 0) ||
            (writeback && (list & (1U << rn)) != 0)) unsupported();
        const auto count = static_cast<unsigned>(std::popcount(list));
        const auto bytes = count * 4U;
        const auto base = reg(rn);
        const auto start = up ? base + (pre ? 4U : 0U) : base - bytes + (pre ? 0U : 4U);
        std::array<std::uint32_t, 16> values{};
        unsigned index{};
        // Lectures et écritures sont préparées avant toute modification : pas
        // de pile à moitié écrite ni de registres à moitié chargés sur erreur.
        for (unsigned r = 0; r < 16; ++r) {
            if ((list & (1U << r)) == 0) continue;
            values[index] = load ? ram.read32(cpu, start + index * 4U) : reg(r);
            ++index;
        }
        if (load) {
            index = 0;
            for (unsigned r = 0; r < 16; ++r) {
                if ((list & (1U << r)) == 0) continue;
                if (r == 15) load_pc(values[index]);
                else s.registers[r] = values[index];
                ++index;
            }
        } else ram.write_words(cpu, start, std::span<const std::uint32_t>{values}.first(count));
        if (writeback) s.registers[rn] = up ? base + bytes : base - bytes;
    }

    void execute_thumb(std::uint32_t raw) {
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
            s.registers[(raw >> 8U) & 7U] = ram.read32(cpu, ((pc + 4U) & ~3U) + (raw & 0xffU) * 4U);
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
            if (cond >= 14) unsupported(); // instruction indéfinie / SWI
            if (condition(s, cond)) branch(pc + 4U + sign_extend((raw & 0xffU) << 1U, 9));
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

    void thumb_transfer(unsigned rd, std::uint32_t address, unsigned width, bool load) {
        if (load) {
            if (width == 1) s.registers[rd] = ram.read8(cpu, address);
            else if (width == 2) s.registers[rd] = ram.read16(cpu, address);
            else s.registers[rd] = ram.read32(cpu, address);
        } else {
            if (width == 1) ram.write8(cpu, address, static_cast<std::uint8_t>(reg(rd)));
            else if (width == 2) ram.write16(cpu, address, static_cast<std::uint16_t>(reg(rd)));
            else ram.write32(cpu, address, reg(rd));
        }
    }

    void thumb_alu(std::uint32_t raw) {
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
};

} // namespace

CpuExecutionError::CpuExecutionError(Processor processor, std::uint32_t address, bool thumb,
                                     std::optional<std::uint32_t> instruction, const std::string& reason)
    : Error(fault_message(processor, address, thumb, instruction, reason)), processor_(processor),
      address_(address), thumb_(thumb), instruction_(instruction) {}

void Cpu::reset(std::uint32_t address, bool thumb) {
    if ((address & (thumb ? 1U : 3U)) != 0) throw std::invalid_argument("NDS : entrée CPU non alignée");
    state_ = {};
    state_.registers[15] = address;
    state_.thumb = thumb;
}

void Cpu::set_register(std::size_t index, std::uint32_t value) {
    if (index >= 16) throw std::out_of_range("NDS : numéro de registre CPU");
    if (index == 15) value &= state_.thumb ? ~1U : ~3U;
    state_.registers[index] = value;
}

void Cpu::step(Memory& memory) {
    const auto pc = state_.registers[15];
    std::optional<std::uint32_t> instruction;
    try {
        instruction = state_.thumb ? memory.read16(processor_, pc) : memory.read32(processor_, pc);
        Interpreter interpreter{state_, processor_, memory};
        interpreter.execute(*instruction);
        state_ = interpreter.s;
    } catch (const Error& error) {
        throw CpuExecutionError(processor_, pc, state_.thumb, instruction, error.what());
    }
}

} // namespace ravenemu::nds
