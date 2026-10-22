#include "interpreter.hpp"

namespace ravenemu::nds::detail {

void Interpreter::arm_swap(std::uint32_t raw) {
    const auto rn = (raw >> 16U) & 15U, rd = (raw >> 12U) & 15U, rm = raw & 15U;
    if (rn == 15 || rd == 15 || rm == 15) unsupported();
    const auto address = reg(rn), value = reg(rm);
    s.registers[rd] = bus.swap(address,value,(raw & 0x00400000U) != 0);
}

void Interpreter::arm_doubleword(std::uint32_t raw) {
    const bool pre = (raw & 0x01000000U) != 0, up = (raw & 0x00800000U) != 0;
    const bool immediate = (raw & 0x00400000U) != 0, writeback = (raw & 0x00200000U) != 0;
    const bool load = (raw & 0x20U) == 0; // L=0 : type 2=LDRD, type 3=STRD
    const auto rn = (raw >> 16U) & 15U, rd = (raw >> 12U) & 15U, rm = raw & 15U;
    if (cpu != Processor::arm9 || (rd & 1U) || rd >= 14 || (!pre && writeback) ||
        ((!pre || writeback) && (rn == 15 || rn == rd || rn == rd + 1U)) ||
        (!immediate && ((raw & 0xf00U) || rm == 15 || (!load && (rm == rd || rm == rd + 1U))))) unsupported();
    const auto base = reg(rn);
    const auto offset = immediate ? ((raw >> 4U) & 0xf0U) | (raw & 15U) : reg(rm);
    const auto adjusted = up ? base + offset : base - offset;
    const auto address = pre ? adjusted : base;
    std::array<std::uint32_t,2> values{reg(rd),reg(rd + 1U)};
    // Comme LDM/STM, validation de la paire complète avant tout effet ;
    // les deux accès ARM946 restent alignés sur 32 bits dans ce modèle.
    if (load) {
        bus.read_words(address,values);
        s.registers[rd] = values[0]; s.registers[rd + 1U] = values[1];
    } else bus.write_words(address,values);
    if (!pre || writeback) s.registers[rn] = adjusted;
}

void Interpreter::multiple(unsigned rn, std::uint32_t list, bool load, bool pre, bool up, bool writeback,
                           bool restore_status) {
    if (rn == 15 || list == 0 || (!load && (list & 0x8000U) != 0) ||
        (writeback && (list & (1U << rn)) != 0)) unsupported();
    const auto count = static_cast<unsigned>(std::popcount(list));
    const auto bytes = count * 4U;
    const auto base = reg(rn);
    const auto start = up ? base + (pre ? 4U : 0U) : base - bytes + (pre ? 0U : 4U);
    std::optional<std::uint32_t> saved;
    if (restore_status) {
        saved = s.spsr();
        auto validated = s;
        validated.set_cpsr(*saved); // valider le mode avant tout effet
    }
    std::array<std::uint32_t, 16> values{};
    std::optional<std::uint32_t> loaded_pc;
    unsigned index{};
    // Lectures et écritures sont préparées avant toute modification : pas
    // de pile à moitié écrite ni de registres à moitié chargés sur erreur.
    if (load) bus.read_words(start, std::span<std::uint32_t>{values}.first(count));
    else for (unsigned r = 0; r < 16; ++r) {
        if ((list & (1U << r)) == 0) continue;
        values[index++] = reg(r);
    }
    if (load) {
        index = 0;
        for (unsigned r = 0; r < 16; ++r) {
            if ((list & (1U << r)) == 0) continue;
            if (r == 15) loaded_pc = values[index];
            else s.registers[r] = values[index];
            ++index;
        }
    } else bus.write_words(start, std::span<const std::uint32_t>{values}.first(count));
    if (writeback) s.registers[rn] = up ? base + bytes : base - bytes;
    if (loaded_pc) {
        if (saved) { s.set_cpsr(*saved); branch(*loaded_pc); }
        else load_pc(*loaded_pc);
    }
}

} // namespace ravenemu::nds::detail
