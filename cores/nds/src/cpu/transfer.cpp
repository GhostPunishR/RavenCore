#include "interpreter.hpp"

namespace ravenemu::nds::detail {

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
    for (unsigned r = 0; r < 16; ++r) {
        if ((list & (1U << r)) == 0) continue;
        values[index] = load ? ram.read32(cpu, start + index * 4U) : reg(r);
        ++index;
    }
    if (load) {
        index = 0;
        for (unsigned r = 0; r < 16; ++r) {
            if ((list & (1U << r)) == 0) continue;
            if (r == 15) loaded_pc = values[index];
            else s.registers[r] = values[index];
            ++index;
        }
    } else ram.write_words(cpu, start, std::span<const std::uint32_t>{values}.first(count));
    if (writeback) s.registers[rn] = up ? base + bytes : base - bytes;
    if (loaded_pc) {
        if (saved) { s.set_cpsr(*saved); branch(*loaded_pc); }
        else load_pc(*loaded_pc);
    }
}

} // namespace ravenemu::nds::detail
