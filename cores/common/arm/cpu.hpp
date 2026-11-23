#pragma once

#include "arm/registers.hpp"

#include <bit>
#include <concepts>
#include <cstdint>
#include <optional>

// Interpréteur ARM commun aux cœurs RavenCore : ARMv4T (ARM7TDMI) et ARMv5TE
// (ARM946E-S), en ARM comme en Thumb. Écrit pour RavenCore d'après GBATEK
// (« ARM CPU Reference », « THUMB Instruction Set ») ; aucun code d'un autre
// émulateur.
//
// Le CPU ne connaît que son bus : lectures du code et des données, cycles
// internes, coprocesseurs et appels au BIOS. Le bus compte lui-même le coût
// de chaque accès ; le CPU lui signale les cycles internes (I) de GBATEK.
// Les cycles propres à l'ARM9 (recouvrement des accès, verrous du pipeline)
// restent à mesurer : ses instructions comptent pour l'instant ceux de l'ARM7.
namespace ravenemu::arm {

enum class Architecture { armv4t, armv5te };

// Accès MRC ou MCR à un coprocesseur : MCR/MRC Pn,opcode1,Rd,Cn,Cm,opcode2.
struct CoprocessorAccess {
    unsigned coprocessor{};
    unsigned opcode1{};
    unsigned crn{};
    unsigned crm{};
    unsigned opcode2{};
};

// Contrat du bus. Les adresses des lectures et écritures de 16 et 32 bits
// arrivent déjà alignées ; le CPU fait lui-même les rotations de GBATEK.
// `coprocessor_read` rend std::nullopt et `coprocessor_write` false pour un
// coprocesseur absent : le CPU lève alors l'exception d'instruction indéfinie.
// `software_interrupt` rend true quand l'hôte a exécuté lui-même la fonction
// du BIOS (BIOS HLE) ; sinon le CPU saute au vecteur SWI.
template <typename B>
concept Bus = requires(B& bus, std::uint32_t address, std::uint32_t value, int cycles, CoprocessorAccess access,
                       bool thumb) {
    { bus.code16(address) } -> std::convertible_to<std::uint32_t>;
    { bus.code32(address) } -> std::convertible_to<std::uint32_t>;
    { bus.read8(address) } -> std::convertible_to<std::uint32_t>;
    { bus.read16(address) } -> std::convertible_to<std::uint32_t>;
    { bus.read32(address) } -> std::convertible_to<std::uint32_t>;
    bus.write8(address, value);
    bus.write16(address, value);
    bus.write32(address, value);
    bus.internal(cycles);
    { bus.coprocessor_read(access) } -> std::same_as<std::optional<std::uint32_t>>;
    { bus.coprocessor_write(access, value) } -> std::same_as<bool>;
    { bus.software_interrupt(value, thumb) } -> std::same_as<bool>;
};

template <Architecture Arch, Bus BusType>
class Cpu {
public:
    static constexpr bool armv5 = Arch == Architecture::armv5te;

    explicit Cpu(BusType& bus) noexcept : bus_(bus) {}

    // Mise sous tension : superviseur, état ARM, IRQ et FIQ masquées, PC au
    // vecteur de reset (GBATEK, « ARM CPU Exceptions »).
    void reset() noexcept {
        regs = Registers{};
        branch_target_ = vector_base;
        irq_line_ = false;
    }

    // Reprend l'exécution à `address`, en Thumb si `thumb` (démarrage direct).
    void jump(std::uint32_t address, bool thumb) noexcept {
        regs.thumb = thumb;
        branch_target_ = address & (thumb ? ~1U : ~3U);
    }

    // Exécute une instruction, ou entre dans le gestionnaire d'IRQ si la ligne
    // est levée et les IRQ permises.
    void step() {
        if (irq_line_ && !regs.irq_disabled) {
            enter_exception(mode::irq, 0x18, branch_target_ + 4, false);
            return;
        }
        current_ = branch_target_;
        branched_ = false;
        if (regs.thumb) {
            const auto opcode = static_cast<std::uint32_t>(bus_.code16(current_)) & 0xffffU;
            regs.r[15] = current_ + 4;
            execute_thumb(opcode);
            if (!branched_) branch_target_ = current_ + 2;
        } else {
            const auto opcode = static_cast<std::uint32_t>(bus_.code32(current_));
            regs.r[15] = current_ + 8;
            if (condition(opcode >> 28U)) execute_arm(opcode);
            else if ((opcode >> 28U) == 15U) execute_unconditional(opcode);
            if (!branched_) branch_target_ = current_ + 4;
        }
    }

    // Ligne IRQ du processeur, tenue par le contrôleur d'interruptions.
    void set_irq_line(bool asserted) noexcept { irq_line_ = asserted; }
    [[nodiscard]] bool irq_line() const noexcept { return irq_line_; }

    // Adresse de la prochaine instruction.
    [[nodiscard]] std::uint32_t pc() const noexcept { return branch_target_; }

    Registers regs;
    // ARMv5 : 0xFFFF0000 quand CP15 place les vecteurs en haut (bit 13).
    std::uint32_t vector_base{};
    // ARMv5 : LDR, LDM et POP vers PC changent d'état selon le bit 0 de la
    // valeur lue, sauf en mode « pré-ARMv5 » de CP15 (bit 15).
    bool load_interworking{armv5};

private:
    struct Shifted {
        std::uint32_t value;
        bool carry;
    };

    [[nodiscard]] bool condition(std::uint32_t code) const noexcept {
        switch (code & 15U) {
        case 0: return regs.z;
        case 1: return !regs.z;
        case 2: return regs.c;
        case 3: return !regs.c;
        case 4: return regs.n;
        case 5: return !regs.n;
        case 6: return regs.v;
        case 7: return !regs.v;
        case 8: return regs.c && !regs.z;
        case 9: return !regs.c || regs.z;
        case 10: return regs.n == regs.v;
        case 11: return regs.n != regs.v;
        case 12: return !regs.z && regs.n == regs.v;
        case 13: return regs.z || regs.n != regs.v;
        case 14: return true;
        default: return false;  // NV : réservé à l'ARMv5 (instructions inconditionnelles)
        }
    }

    // Saut : la prochaine instruction est lue à `target`, aligné selon l'état.
    void branch(std::uint32_t target) noexcept {
        branch_target_ = target & (regs.thumb ? ~1U : ~3U);
        branched_ = true;
    }
    // Saut qui choisit l'état d'après le bit 0 (BX, BLX).
    void branch_exchange(std::uint32_t target) noexcept {
        regs.thumb = (target & 1U) != 0;
        branch(target);
    }
    // PC chargé depuis la mémoire (LDR, LDM, POP).
    void load_pc(std::uint32_t value) noexcept {
        if (armv5 && load_interworking) branch_exchange(value);
        else branch(value);
    }
    void write_register(unsigned index, std::uint32_t value) noexcept {
        if (index == 15) branch(value);
        else regs.r[index] = value;
    }
    void set_nz(std::uint32_t value) noexcept {
        regs.n = (value >> 31U) != 0;
        regs.z = value == 0;
    }

    void enter_exception(std::uint32_t next_mode, std::uint32_t offset, std::uint32_t return_address,
                         bool disable_fiq) noexcept {
        const auto saved = regs.cpsr();
        regs.switch_mode(next_mode);
        regs.set_spsr(saved, 0xffff'ffffU);
        regs.r[14] = return_address;
        regs.thumb = false;
        regs.irq_disabled = true;
        if (disable_fiq) regs.fiq_disabled = true;
        branch(vector_base + offset);
    }
    void undefined_instruction() noexcept {
        enter_exception(mode::undefined, 0x04, current_ + (regs.thumb ? 2U : 4U), false);
    }
    void breakpoint() noexcept { enter_exception(mode::abort, 0x0c, current_ + 4, false); }
    void software_interrupt(std::uint32_t comment) {
        if (bus_.software_interrupt(comment, regs.thumb)) return;
        enter_exception(mode::supervisor, 0x08, current_ + (regs.thumb ? 2U : 4U), false);
    }

    // Décalages du second opérande (GBATEK, « Data Processing »).
    [[nodiscard]] Shifted shift_by_immediate(std::uint32_t value, std::uint32_t type, std::uint32_t amount) const noexcept;
    [[nodiscard]] Shifted shift_by_register(std::uint32_t value, std::uint32_t type, std::uint32_t amount) const noexcept;
    std::uint32_t add_with_flags(std::uint32_t left, std::uint32_t right, bool carry_in, bool set_flags) noexcept;
    static int multiply_cycles(std::uint32_t operand, bool signed_operand) noexcept;

    // ARM
    void execute_arm(std::uint32_t opcode);
    void execute_unconditional(std::uint32_t opcode);
    void arm_data(std::uint32_t opcode, Shifted operand, bool pc_plus_12);
    void arm_data_immediate(std::uint32_t opcode);
    void arm_data_shift_immediate(std::uint32_t opcode);
    void arm_data_shift_register(std::uint32_t opcode);
    void arm_status_read(std::uint32_t opcode);
    void arm_status_write(std::uint32_t opcode, std::uint32_t value);
    void arm_multiply(std::uint32_t opcode);
    void arm_multiply_long(std::uint32_t opcode);
    void arm_signed_multiply(std::uint32_t opcode);
    void arm_saturating(std::uint32_t opcode);
    void arm_single_transfer(std::uint32_t opcode, std::uint32_t offset);
    void arm_halfword_transfer(std::uint32_t opcode);
    void arm_doubleword_transfer(std::uint32_t opcode, std::uint32_t address);
    void arm_swap(std::uint32_t opcode);
    void arm_block_transfer(std::uint32_t opcode);
    void arm_block_store(std::uint32_t opcode, std::uint32_t list, std::uint32_t address, std::uint32_t new_base);
    void arm_block_load(std::uint32_t opcode, std::uint32_t list, std::uint32_t address, std::uint32_t new_base);
    void arm_branch(std::uint32_t opcode);
    void arm_coprocessor_register(std::uint32_t opcode);

    // Chargements communs à l'ARM et au Thumb, avec les règles d'alignement
    // propres à chaque architecture (GBATEK, « ARM CPU Memory Alignments »).
    std::uint32_t load_word(std::uint32_t address);
    std::uint32_t load_halfword(std::uint32_t address);
    std::uint32_t load_signed_halfword(std::uint32_t address);
    std::uint32_t load_signed_byte(std::uint32_t address);

    // Thumb
    void execute_thumb(std::uint32_t opcode);
    void thumb_shift_or_add(std::uint32_t opcode);
    void thumb_immediate(std::uint32_t opcode);
    void thumb_alu(std::uint32_t opcode);
    void thumb_high_register(std::uint32_t opcode);
    void thumb_load_store(std::uint32_t opcode);
    void thumb_load_store_immediate(std::uint32_t opcode);
    void thumb_address(std::uint32_t opcode);
    void thumb_push_pop(std::uint32_t opcode);
    void thumb_multiple(std::uint32_t opcode);
    void thumb_branch(std::uint32_t opcode);
    void thumb_long_branch(std::uint32_t opcode);

    BusType& bus_;
    std::uint32_t current_{};        // adresse de l'instruction en cours
    std::uint32_t branch_target_{};  // adresse de la suivante
    bool branched_{};
    bool irq_line_{};
};

namespace detail {
constexpr std::uint32_t sign_extend(std::uint32_t value, unsigned bits) noexcept {
    const auto shift = 32U - bits;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value << shift) >> shift);
}
} // namespace detail

} // namespace ravenemu::arm

#include "arm/arm_alu.hpp"
#include "arm/arm_multiply.hpp"
#include "arm/arm_memory.hpp"
#include "arm/arm_control.hpp"
#include "arm/arm_decoder.hpp"
#include "arm/thumb.hpp"
