#pragma once

#include "cpu/cpu_state.hpp"
#include "memory/bus.hpp"

namespace ravenemu::gba {

class Cpu {
public:
    explicit Cpu(Bus& bus) : bus(bus) {}
    void reset(std::int32_t entry) {
        state.reset();
        state.switch_mode(CpuState::mode_supervisor);
        state.regs[13] = i32(0x03007fe0U);
        state.switch_mode(CpuState::mode_irq);
        state.regs[13] = i32(0x03007fa0U);
        state.switch_mode(CpuState::mode_system);
        state.regs[13] = i32(0x03007f00U);
        state.thumb = false;
        state.irq_disabled = false;
        state.regs[15] = entry;
        pipeline_valid_ = false;
    }
    [[nodiscard]] std::int32_t read_reg(int index) const noexcept {
        if (index != 15) return state.regs[static_cast<std::size_t>(index)];
        return add32(state.regs[15], state.thumb ? 4 : 8);
    }
    void write_reg(int index, std::int32_t value) noexcept {
        if (index == 15)
            branch_to(value);
        else
            state.regs[static_cast<std::size_t>(index)] = value;
    }
    void branch_to(std::int32_t address) noexcept {
        state.regs[15] = i32(u32(address) & (state.thumb ? ~1U : ~3U));
        branched_ = true;
        pipeline_valid_ = false;
    }
    void branch_exchange(std::int32_t address) noexcept {
        state.thumb = (u32(address) & 1U) != 0;
        state.regs[15] = i32(u32(address) & (state.thumb ? ~1U : ~3U));
        branched_ = true;
        pipeline_valid_ = false;
    }
    void execute_swi(int number, std::int32_t return_address) {
        // Le vrai BIOS saute puis revient : le pipeline est rechargé, et voit
        // donc le code qu'un SWI (CpuSet, décompression) vient d'écrire.
        if (swi_handler) {
            swi_handler(number);
            pipeline_valid_ = false;
        } else
            raise_exception(CpuState::mode_supervisor, 0x08, return_address);
    }
    void raise_exception(int mode, std::int32_t vector, std::int32_t return_address) {
        const auto saved = state.cpsr();
        state.switch_mode(mode);
        state.set_spsr(saved);
        state.regs[14] = return_address;
        state.irq_disabled = true;
        state.thumb = false;
        state.regs[15] = vector;
        branched_ = true;
        pipeline_valid_ = false;
    }
    /**
     * Pipeline de l'ARM7TDMI : quand une instruction s'exécute, les deux
     * suivantes sont déjà lues. L'instruction courante précharge PC + 2
     * instructions avant d'agir, si bien qu'une écriture sur ces adresses ne
     * change pas ce qui s'exécute ensuite (code automodifiant). Un saut vide
     * le pipeline, rechargé (avec ses attentes mémoire) à l'instruction suivante.
     */
    int step() {
        branched_ = false;
        bus.take_wait_cycles();
        if (!pipeline_valid_ || pipeline_thumb_ != state.thumb) refill_pipeline();
        const auto width = state.thumb ? 2 : 4;
        const auto pc = state.regs[15];
        const auto instruction = pipeline_[0];
        const auto prefetched = fetch(add32(pc, 2 * width));
        // Le bus compte les cycles de chaque accès, attentes comprises ;
        // l'instruction ne rend que ses cycles internes (I), qui suivent ses accès.
        int internal{};
        if (state.thumb)
            internal = execute_thumb(static_cast<int>(u32(instruction) & 0xffffU));
        else
            internal = check_condition(static_cast<int>(u32(instruction) >> 28U)) ? execute_arm(instruction) : 0;
        if (internal > 0) {
            bus.idle(internal);
            // Préchargement désactivé : après les cycles internes d'une
            // instruction en ROM, la lecture suivante redevient non séquentielle
            // (GBATEK, « Prefetch Disable Bug »).
            const auto region = u32(pc) >> 24U;
            if (!bus.timing.prefetch_enabled() && region >= 0x08U && region <= 0x0dU) bus.break_access_sequence();
        }
        if (branched_) {
            bus.break_access_sequence();
        } else {
            state.regs[15] = add32(pc, width);
            pipeline_[0] = pipeline_[1];
            pipeline_[1] = prefetched;
        }
        return bus.take_wait_cycles();
    }
    // Contenu du pipeline, conservé dans les états instantanés.
    struct Pipeline {
        std::array<std::int32_t, 2> opcodes{};
        bool valid{};
        bool thumb{};
    };
    [[nodiscard]] Pipeline pipeline() const noexcept { return {pipeline_, pipeline_valid_, pipeline_thumb_}; }
    void restore_pipeline(const Pipeline& value) noexcept {
        pipeline_ = value.opcodes;
        pipeline_valid_ = value.valid;
        pipeline_thumb_ = value.thumb;
    }
    [[nodiscard]] bool check_condition(int condition) const noexcept {
        if (condition == 14) return true;  // AL, de loin la plus fréquente
        switch (condition & 15) {
        case 0: return state.zero;
        case 1: return !state.zero;
        case 2: return state.carry;
        case 3: return !state.carry;
        case 4: return state.negative;
        case 5: return !state.negative;
        case 6: return state.overflow;
        case 7: return !state.overflow;
        case 8: return state.carry && !state.zero;
        case 9: return !state.carry || state.zero;
        case 10: return state.negative == state.overflow;
        case 11: return state.negative != state.overflow;
        case 12: return !state.zero && state.negative == state.overflow;
        case 13: return state.zero || state.negative != state.overflow;
        case 14: return true;
        default: return false;
        }
    }
    std::int32_t shift_immediate(std::int32_t value, int type, int amount) noexcept {
        const auto raw = u32(value);
        switch (type & 3) {
        case 0:
            if (amount == 0) {
                shifter_carry = state.carry;
                return value;
            }
            shifter_carry = ((raw >> (32U - static_cast<unsigned>(amount))) & 1U) != 0;
            return i32(raw << static_cast<unsigned>(amount));
        case 1: {
            const auto n = amount == 0 ? 32U : static_cast<unsigned>(amount);
            shifter_carry = ((raw >> (n - 1U)) & 1U) != 0;
            return n == 32U ? 0 : i32(raw >> n);
        }
        case 2: {
            const auto n = amount == 0 ? 32U : static_cast<unsigned>(amount);
            if (n >= 32U) {
                shifter_carry = value < 0;
                return value < 0 ? -1 : 0;
            }
            shifter_carry = ((raw >> (n - 1U)) & 1U) != 0;
            return value >> n;
        }
        default:
            if (amount == 0) {
                shifter_carry = (raw & 1U) != 0;
                return i32((raw >> 1U) | (state.carry ? 0x80000000U : 0U));
            }
            shifter_carry = ((raw >> (static_cast<unsigned>(amount) - 1U)) & 1U) != 0;
            return i32(std::rotr(raw, amount));
        }
    }
    std::int32_t shift_register(std::int32_t value, int type, int amount) noexcept {
        const auto n = static_cast<unsigned>(amount) & 0xffU;
        const auto raw = u32(value);
        if (n == 0) {
            shifter_carry = state.carry;
            return value;
        }
        switch (type & 3) {
        case 0:
            if (n < 32U) {
                shifter_carry = ((raw >> (32U - n)) & 1U) != 0;
                return i32(raw << n);
            }
            shifter_carry = n == 32U && (raw & 1U) != 0;
            return 0;
        case 1:
            if (n < 32U) {
                shifter_carry = ((raw >> (n - 1U)) & 1U) != 0;
                return i32(raw >> n);
            }
            shifter_carry = n == 32U && value < 0;
            return 0;
        case 2:
            if (n >= 32U) {
                shifter_carry = value < 0;
                return value < 0 ? -1 : 0;
            }
            shifter_carry = ((raw >> (n - 1U)) & 1U) != 0;
            return value >> n;
        default: {
            const auto rotate = n & 31U;
            if (rotate == 0) {
                shifter_carry = value < 0;
                return value;
            }
            shifter_carry = ((raw >> (rotate - 1U)) & 1U) != 0;
            return i32(std::rotr(raw, static_cast<int>(rotate)));
        }
        }
    }
    std::int32_t add_with_carry(std::int32_t a, std::int32_t b, int carry_in) noexcept {
        const auto sum = static_cast<std::uint64_t>(u32(a)) + u32(b) + static_cast<unsigned>(carry_in);
        const auto result = i32(static_cast<std::uint32_t>(sum));
        last_carry = sum > 0xffff'ffffULL;
        last_overflow = ((a ^ result) & (b ^ result)) < 0;
        return result;
    }
    void set_nz(std::int32_t value) noexcept {
        state.negative = value < 0;
        state.zero = value == 0;
    }
    // Cycles internes m d'une multiplication sur l'ARM7TDMI : le multiplieur
    // s'arrête dès que les octets de poids fort de l'opérande sont tous à 0
    // (ou tous à 1 pour une multiplication signée).
    static int multiply_cycles(std::uint32_t operand, bool sign_extended) noexcept {
        auto cycles = 1;
        for (unsigned shift = 8; shift < 32; shift += 8, ++cycles) {
            const auto top = operand >> shift;
            if (top == 0 || (sign_extended && top == (0xffff'ffffU >> shift))) return cycles;
        }
        return 4;
    }
    // Le bus aligne lui-même l'adresse, sauf la SRAM qui lit l'octet visé.
    std::int32_t load_word_rotated(std::int32_t address) {
        const auto word = bus.read32(address);
        return i32(std::rotr(u32(word), static_cast<int>((u32(address) & 3U) * 8U)));
    }
    // Demi-mot non aligné : l'ARM7TDMI lit le demi-mot aligné puis le fait
    // tourner de 8 bits ; en signé, il ne charge que l'octet impair.
    std::int32_t load_halfword(std::int32_t address) {
        const auto half = u32(bus.read16(address)) & 0xffffU;
        return i32((u32(address) & 1U) != 0 ? std::rotr(half, 8) : half);
    }
    std::int32_t load_signed_halfword(std::int32_t address) {
        if ((u32(address) & 1U) != 0) return sign_extend(static_cast<unsigned>(bus.read8(address)), 8);
        return sign_extend(static_cast<unsigned>(bus.read16(address)), 16);
    }

    Bus& bus;
    CpuState state;
    std::function<void(int)> swi_handler;
    bool shifter_carry{};
    bool last_carry{};
    bool last_overflow{};

private:
    int execute_arm(std::int32_t instruction);
    int arm_undefined(std::int32_t instruction);
    int arm_branch(std::int32_t instruction);
    int arm_data_or_psr(std::int32_t instruction);
    int arm_data(std::int32_t instruction, bool immediate, int opcode, bool set_flags);
    int arm_data_without_flags(std::uint32_t raw, int opcode);
    std::int32_t arm_operand2(std::int32_t instruction, bool immediate, int pc_extra);
    int arm_psr(std::int32_t instruction);
    int arm_single_transfer(std::int32_t instruction);
    int arm_multiply(std::int32_t instruction);
    int arm_multiply_long(std::int32_t instruction);
    int arm_swap(std::int32_t instruction);
    int arm_halfword(std::int32_t instruction);
    int arm_block(std::int32_t instruction);
    int arm_swi(std::int32_t instruction);
    int execute_thumb(int instruction);
    int thumb_undefined(int instruction);
    void thumb_arithmetic_flags(std::int32_t value) noexcept;
    std::int32_t fetch(std::int32_t address) {
        return state.thumb ? i32(u32(bus.fetch16(address)) & 0xffffU) : bus.fetch32(address);
    }
    void refill_pipeline() {
        pipeline_thumb_ = state.thumb;
        const auto pc = state.regs[15];
        pipeline_[0] = fetch(pc);
        pipeline_[1] = fetch(add32(pc, state.thumb ? 2 : 4));
        pipeline_valid_ = true;
    }
    bool branched_{};
    std::array<std::int32_t, 2> pipeline_{};
    bool pipeline_valid_{};
    bool pipeline_thumb_{};
};

}  // namespace ravenemu::gba
