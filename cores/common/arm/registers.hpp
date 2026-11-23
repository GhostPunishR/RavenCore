#pragma once

#include <array>
#include <cstdint>

// Registres d'un processeur ARM 32 bits (ARMv4T et ARMv5TE) : les seize
// registres visibles, le CPSR et les banques de chaque mode. Écrit d'après
// GBATEK, « ARM CPU Register Set » et « ARM CPU Flags & Condition Field ».
namespace ravenemu::arm {

namespace mode {
inline constexpr std::uint32_t user = 0x10;
inline constexpr std::uint32_t fiq = 0x11;
inline constexpr std::uint32_t irq = 0x12;
inline constexpr std::uint32_t supervisor = 0x13;
inline constexpr std::uint32_t abort = 0x17;
inline constexpr std::uint32_t undefined = 0x1b;
inline constexpr std::uint32_t system = 0x1f;
} // namespace mode

// Bits du CPSR et des SPSR.
namespace psr {
inline constexpr std::uint32_t negative = 1U << 31U;
inline constexpr std::uint32_t zero = 1U << 30U;
inline constexpr std::uint32_t carry = 1U << 29U;
inline constexpr std::uint32_t overflow = 1U << 28U;
inline constexpr std::uint32_t sticky_overflow = 1U << 27U;  // Q, ARMv5TE
inline constexpr std::uint32_t irq_disable = 1U << 7U;
inline constexpr std::uint32_t fiq_disable = 1U << 6U;
inline constexpr std::uint32_t thumb = 1U << 5U;
inline constexpr std::uint32_t mode_bits = 0x1fU;
} // namespace psr

class Registers {
public:
    // R15 vaut, pendant l'exécution d'une instruction, son adresse + 8 (ARM)
    // ou + 4 (Thumb), comme le voit le programme.
    std::array<std::uint32_t, 16> r{};
    bool n{};
    bool z{};
    bool c{};
    bool v{};
    bool q{};
    bool irq_disabled{true};
    bool fiq_disabled{true};
    bool thumb{};
    std::uint32_t mode{mode::supervisor};

    [[nodiscard]] std::uint32_t cpsr() const noexcept {
        auto value = mode & psr::mode_bits;
        if (thumb) value |= psr::thumb;
        if (fiq_disabled) value |= psr::fiq_disable;
        if (irq_disabled) value |= psr::irq_disable;
        if (q) value |= psr::sticky_overflow;
        if (v) value |= psr::overflow;
        if (c) value |= psr::carry;
        if (z) value |= psr::zero;
        if (n) value |= psr::negative;
        return value;
    }

    // Écrit les bits du CPSR choisis par `mask` ; un changement de mode
    // échange aussitôt les banques.
    void set_cpsr(std::uint32_t value, std::uint32_t mask) noexcept {
        const auto merged = (cpsr() & ~mask) | (value & mask);
        n = (merged & psr::negative) != 0;
        z = (merged & psr::zero) != 0;
        c = (merged & psr::carry) != 0;
        v = (merged & psr::overflow) != 0;
        q = (merged & psr::sticky_overflow) != 0;
        irq_disabled = (merged & psr::irq_disable) != 0;
        fiq_disabled = (merged & psr::fiq_disable) != 0;
        thumb = (merged & psr::thumb) != 0;
        switch_mode(merged & psr::mode_bits);
    }

    // Les modes utilisateur et système n'ont pas de SPSR.
    [[nodiscard]] bool has_spsr() const noexcept { return bank(mode) != 0; }
    [[nodiscard]] std::uint32_t spsr() const noexcept { return has_spsr() ? spsr_[bank(mode)] : cpsr(); }
    void set_spsr(std::uint32_t value, std::uint32_t mask) noexcept {
        if (!has_spsr()) return;
        auto& target = spsr_[bank(mode)];
        target = (target & ~mask) | (value & mask);
    }

    void switch_mode(std::uint32_t next) noexcept {
        next &= psr::mode_bits;
        if (next == mode) return;
        store_bank(mode);
        load_bank(next);
        mode = next;
    }

    // Registres de la banque utilisateur, que LDM et STM avec « ^ » visent
    // depuis un mode privilégié.
    [[nodiscard]] std::uint32_t user_register(unsigned index) const noexcept {
        if (index < 8 || index == 15) return r[index];
        if (index <= 12) return mode == mode::fiq ? user_r8_r12_[index - 8] : r[index];
        return bank(mode) == 0 ? r[index] : (index == 13 ? r13_[0] : r14_[0]);
    }
    void set_user_register(unsigned index, std::uint32_t value) noexcept {
        if (index < 8 || index == 15) {
            r[index] = value;
        } else if (index <= 12) {
            if (mode == mode::fiq)
                user_r8_r12_[index - 8] = value;
            else
                r[index] = value;
        } else if (bank(mode) == 0) {
            r[index] = value;
        } else {
            (index == 13 ? r13_ : r14_)[0] = value;
        }
    }

    // Banque de chaque mode : 0 pour utilisateur et système (et tout mode
    // invalide), puis FIQ, IRQ, superviseur, abandon, indéfini.
    static constexpr unsigned bank(std::uint32_t value) noexcept {
        switch (value & psr::mode_bits) {
        case mode::fiq: return 1;
        case mode::irq: return 2;
        case mode::supervisor: return 3;
        case mode::abort: return 4;
        case mode::undefined: return 5;
        default: return 0;
        }
    }

private:
    void store_bank(std::uint32_t from) noexcept {
        const auto slot = bank(from);
        r13_[slot] = r[13];
        r14_[slot] = r[14];
        auto& high = from == mode::fiq ? fiq_r8_r12_ : user_r8_r12_;
        for (unsigned i = 0; i < 5; ++i) high[i] = r[8 + i];
    }
    void load_bank(std::uint32_t to) noexcept {
        const auto slot = bank(to);
        r[13] = r13_[slot];
        r[14] = r14_[slot];
        const auto& high = to == mode::fiq ? fiq_r8_r12_ : user_r8_r12_;
        for (unsigned i = 0; i < 5; ++i) r[8 + i] = high[i];
    }

    std::array<std::uint32_t, 6> r13_{};
    std::array<std::uint32_t, 6> r14_{};
    std::array<std::uint32_t, 6> spsr_{};
    std::array<std::uint32_t, 5> user_r8_r12_{};
    std::array<std::uint32_t, 5> fiq_r8_r12_{};
};

} // namespace ravenemu::arm
