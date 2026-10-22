#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace ravenemu::nds {

enum class CpuMode : std::uint8_t {
    user = 0x10, fiq = 0x11, irq = 0x12, supervisor = 0x13,
    abort = 0x17, undefined = 0x1b, system = 0x1f,
};

struct CpuState {
    // r15 est l'adresse de la prochaine instruction à lire. Les lectures
    // architecturales du PC ajoutent 8 en ARM, 4 en Thumb.
    std::array<std::uint32_t, 16> registers{};
    bool negative{};
    bool zero{};
    bool carry{};
    bool overflow{};
    bool sticky_overflow{}; // Q ARM9 ; saturation et accumulation DSP, effacement explicite par MSR
    bool thumb{};
    bool irq_disabled{true};
    bool fiq_disabled{true};
    bool waiting_for_interrupt{}; // WFI ARM9 ou HALT ARM7 ; distinct de CPSR et du compteur
    CpuMode mode{CpuMode::system}; // état de départ du chargement direct
    std::uint64_t instructions{};

    [[nodiscard]] std::uint32_t cpsr() const noexcept;
    [[nodiscard]] bool has_spsr() const noexcept;
    [[nodiscard]] std::uint32_t spsr() const;
    // Mutateurs de la copie de travail de l'interpréteur. Aucun accès mutable
    // à l'état vivant d'un Cpu n'est exposé par Cpu::state().
    void set_cpsr(std::uint32_t value);
    void set_spsr(std::uint32_t value);
    bool operator==(const CpuState&) const = default;

private:
    static std::size_t bank(CpuMode mode) noexcept;
    void switch_mode(CpuMode next) noexcept;
    std::array<std::array<std::uint32_t, 2>, 6> stack_banks_{};
    std::array<std::uint32_t, 5> user_r8_r12_{};
    std::array<std::uint32_t, 5> fiq_r8_r12_{};
    std::array<std::uint32_t, 6> saved_status_{};
};

} // namespace ravenemu::nds
