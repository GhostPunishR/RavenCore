#pragma once

// Sauts, coprocesseurs et instructions inconditionnelles de l'ARMv5 (GBATEK,
// « Branch and Branch with Link », « Coprocessor Instructions »). Inclus par
// cpu.hpp.
namespace ravenemu::arm {

// B, BL : PC = PC + 8 + décalage × 4 ; BL range l'adresse suivante dans LR.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_branch(std::uint32_t opcode) {
    const auto offset = detail::sign_extend((opcode & 0x00ff'ffffU) << 2U, 26);
    if (((opcode >> 24U) & 1U) != 0) regs.r[14] = current_ + 4;
    branch(regs.r[15] + offset);
}

// MRC, MCR. MRC vers R15 ne règle que N, Z, C, V ; MCR depuis R15 envoie
// PC + 12. Un coprocesseur absent rend l'instruction indéfinie.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_coprocessor_register(std::uint32_t opcode) {
    const CoprocessorAccess access{(opcode >> 8U) & 15U, (opcode >> 21U) & 7U, (opcode >> 16U) & 15U, opcode & 15U,
                                   (opcode >> 5U) & 7U};
    const auto rd = (opcode >> 12U) & 15U;
    if (((opcode >> 20U) & 1U) != 0) {
        const auto value = bus_.coprocessor_read(access);
        if (!value) {
            undefined_instruction();
            return;
        }
        bus_.internal(1);
        if (rd == 15) regs.set_cpsr(*value, 0xf000'0000U);
        else regs.r[rd] = *value;
        return;
    }
    if (!bus_.coprocessor_write(access, rd == 15 ? regs.r[15] + 4 : regs.r[rd])) undefined_instruction();
}

// Condition NV. L'ARMv4 ne les exécute pas ; l'ARMv5 y place BLX (adresse
// immédiate, passe en Thumb) et PLD (simple indication de cache, sans effet
// ici). Les variantes MCR2, MRC2, LDC2, STC2 et CDP2 ne visent aucun
// coprocesseur du DS : indéfinies.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::execute_unconditional(std::uint32_t opcode) {
    if constexpr (!armv5) {
        static_cast<void>(opcode);
    } else {
        if ((opcode & 0x0e00'0000U) == 0x0a00'0000U) {
            const auto offset = detail::sign_extend((opcode & 0x00ff'ffffU) << 2U, 26);
            regs.r[14] = current_ + 4;
            regs.thumb = true;
            branch(regs.r[15] + offset + (((opcode >> 24U) & 1U) << 1U));
            return;
        }
        if ((opcode & 0x0d70'f000U) == 0x0550'f000U) return;  // PLD
        undefined_instruction();
    }
}

} // namespace ravenemu::arm
