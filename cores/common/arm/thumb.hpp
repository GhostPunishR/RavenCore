#pragma once

// Jeu d'instructions Thumb, formats 1 à 19 de GBATEK (« THUMB Instruction
// Set »), avec BLX et BKPT de l'ARMv5. R15 vaut l'adresse + 4. Inclus par
// cpu.hpp.
namespace ravenemu::arm {

template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::execute_thumb(std::uint32_t opcode) {
    switch (opcode >> 11U) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: thumb_shift_or_add(opcode); break;
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: thumb_immediate(opcode); break;
    case 0x08:
        if ((opcode & 0x0400U) != 0) thumb_high_register(opcode);
        else thumb_alu(opcode);
        break;
    case 0x09: {  // LDR Rd,[PC,#nn], PC lu comme (adresse + 4) AND NOT 2
        const auto value = load_word((regs.r[15] & ~2U) + (opcode & 0xffU) * 4U);
        bus_.internal(1);
        regs.r[(opcode >> 8U) & 7U] = value;
        break;
    }
    case 0x0a:
    case 0x0b: thumb_load_store(opcode); break;
    case 0x14:
    case 0x15: thumb_address(opcode); break;
    case 0x16:
    case 0x17: thumb_push_pop(opcode); break;
    case 0x18:
    case 0x19: thumb_multiple(opcode); break;
    case 0x1a:
    case 0x1b:
    case 0x1c: thumb_branch(opcode); break;
    case 0x1d:
    case 0x1e:
    case 0x1f: thumb_long_branch(opcode); break;
    default: thumb_load_store_immediate(opcode); break;  // 0x0c à 0x13
    }
}

// Formats 1 et 2 : LSL, LSR, ASR par immédiat ; ADD et SUB à trois opérandes.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_shift_or_add(std::uint32_t opcode) {
    const auto rd = opcode & 7U;
    const auto rs = regs.r[(opcode >> 3U) & 7U];
    if ((opcode >> 11U) == 3) {
        const auto field = (opcode >> 6U) & 7U;
        const auto operand = ((opcode >> 10U) & 1U) != 0 ? field : regs.r[field];
        const auto subtract = ((opcode >> 9U) & 1U) != 0;
        regs.r[rd] = add_with_flags(rs, subtract ? ~operand : operand, subtract, true);
        return;
    }
    const auto shifted = shift_by_immediate(rs, opcode >> 11U, (opcode >> 6U) & 31U);
    regs.r[rd] = shifted.value;
    set_nz(shifted.value);
    regs.c = shifted.carry;
}

// Format 3 : MOV, CMP, ADD, SUB avec un immédiat de 8 bits.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_immediate(std::uint32_t opcode) {
    const auto rd = (opcode >> 8U) & 7U;
    const auto value = opcode & 0xffU;
    switch ((opcode >> 11U) & 3U) {
    case 0:
        regs.r[rd] = value;
        set_nz(value);
        break;
    case 1: add_with_flags(regs.r[rd], ~value, true, true); break;
    case 2: regs.r[rd] = add_with_flags(regs.r[rd], value, false, true); break;
    default: regs.r[rd] = add_with_flags(regs.r[rd], ~value, true, true); break;
    }
}

// Format 4 : opérations de l'UAL entre registres bas.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_alu(std::uint32_t opcode) {
    const auto rd = opcode & 7U;
    const auto a = regs.r[rd];
    const auto b = regs.r[(opcode >> 3U) & 7U];
    const auto code = (opcode >> 6U) & 15U;
    const auto logical = [this, rd](std::uint32_t result) {
        regs.r[rd] = result;
        set_nz(result);
    };
    switch (code) {
    case 0x0: logical(a & b); break;
    case 0x1: logical(a ^ b); break;
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x7: {  // LSL, LSR, ASR, ROR par registre : un cycle interne
        static constexpr std::array<std::uint32_t, 8> types{0, 0, 0, 1, 2, 0, 0, 3};
        bus_.internal(1);
        const auto shifted = shift_by_register(a, types[code], b);
        logical(shifted.value);
        regs.c = shifted.carry;
        break;
    }
    case 0x5: regs.r[rd] = add_with_flags(a, b, regs.c, true); break;
    case 0x6: regs.r[rd] = add_with_flags(a, ~b, regs.c, true); break;
    case 0x8: set_nz(a & b); break;
    case 0x9: regs.r[rd] = add_with_flags(0, ~b, true, true); break;
    case 0xa: add_with_flags(a, ~b, true, true); break;
    case 0xb: add_with_flags(a, b, false, true); break;
    case 0xc: logical(a | b); break;
    case 0xd:  // MUL : m cycles d'après Rd sur l'ARMv4, 3 sur l'ARMv5 ; C non modélisé
        bus_.internal(armv5 ? 3 : multiply_cycles(a, true));
        logical(a * b);
        break;
    case 0xe: logical(a & ~b); break;
    default: logical(~b); break;
    }
}

// Format 5 : ADD, CMP, MOV avec les registres hauts ; BX, et BLX sur l'ARMv5.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_high_register(std::uint32_t opcode) {
    const auto rs = (opcode >> 3U) & 15U;
    const auto rd = (opcode & 7U) | ((opcode >> 4U) & 8U);
    switch ((opcode >> 8U) & 3U) {
    case 0: write_register(rd, regs.r[rd] + regs.r[rs]); break;
    case 1: add_with_flags(regs.r[rd], ~regs.r[rs], true, true); break;
    case 2: write_register(rd, regs.r[rs]); break;
    default: {
        const auto target = regs.r[rs];
        if (armv5 && (opcode & 0x80U) != 0) regs.r[14] = (current_ + 2) | 1U;
        branch_exchange(target);
        break;
    }
    }
}

// Formats 7 et 8 : accès avec un registre de décalage.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_load_store(std::uint32_t opcode) {
    const auto rd = opcode & 7U;
    const auto address = regs.r[(opcode >> 3U) & 7U] + regs.r[(opcode >> 6U) & 7U];
    const auto code = (opcode >> 10U) & 3U;
    if ((opcode & 0x0200U) == 0) {
        switch (code) {
        case 0: bus_.write32(address & ~3U, regs.r[rd]); return;
        case 1: bus_.write8(address, regs.r[rd] & 0xffU); return;
        case 2: regs.r[rd] = load_word(address); break;
        default: regs.r[rd] = static_cast<std::uint32_t>(bus_.read8(address)) & 0xffU; break;
        }
    } else {
        switch (code) {
        case 0: bus_.write16(address & ~1U, regs.r[rd] & 0xffffU); return;
        case 1: regs.r[rd] = load_signed_byte(address); break;
        case 2: regs.r[rd] = load_halfword(address); break;
        default: regs.r[rd] = load_signed_halfword(address); break;
        }
    }
    bus_.internal(1);
}

// Formats 9, 10 et 11 : décalage immédiat (mot, octet, demi-mot) ou relatif à SP.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_load_store_immediate(std::uint32_t opcode) {
    const auto offset = (opcode >> 6U) & 31U;
    auto rd = opcode & 7U;
    const auto base = regs.r[(opcode >> 3U) & 7U];
    const auto load = ((opcode >> 11U) & 1U) != 0;
    std::uint32_t address{};
    unsigned width = 4;
    switch (opcode >> 12U) {
    case 6: address = base + offset * 4U; break;
    case 7: address = base + offset; width = 1; break;
    case 8: address = base + offset * 2U; width = 2; break;
    default:  // 9 : SP + nn × 4
        rd = (opcode >> 8U) & 7U;
        address = regs.r[13] + (opcode & 0xffU) * 4U;
        break;
    }
    if (!load) {
        if (width == 4) bus_.write32(address & ~3U, regs.r[rd]);
        else if (width == 2) bus_.write16(address & ~1U, regs.r[rd] & 0xffffU);
        else bus_.write8(address, regs.r[rd] & 0xffU);
        return;
    }
    if (width == 4) regs.r[rd] = load_word(address);
    else if (width == 2) regs.r[rd] = load_halfword(address);
    else regs.r[rd] = static_cast<std::uint32_t>(bus_.read8(address)) & 0xffU;
    bus_.internal(1);
}

// Formats 12 et 13 : ADD Rd,PC/SP,#nn et ADD SP,#±nn.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_address(std::uint32_t opcode) {
    const auto value = (opcode & 0xffU) * 4U;
    const auto base = ((opcode >> 11U) & 1U) != 0 ? regs.r[13] : regs.r[15] & ~2U;
    regs.r[(opcode >> 8U) & 7U] = base + value;
}

// 0xB000 à 0xBFFF : ADD SP, PUSH, POP, BKPT (ARMv5). POP {PC} reste en Thumb
// sur l'ARMv4 ; sur l'ARMv5, le bit 0 choisit l'état.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_push_pop(std::uint32_t opcode) {
    if ((opcode & 0x0f00U) == 0x0000U) {
        const auto value = (opcode & 0x7fU) * 4U;
        regs.r[13] = (opcode & 0x80U) != 0 ? regs.r[13] - value : regs.r[13] + value;
        return;
    }
    if (armv5 && (opcode & 0x0f00U) == 0x0e00U) {
        breakpoint();
        return;
    }
    if ((opcode & 0x0600U) != 0x0400U) {
        undefined_instruction();
        return;
    }
    const auto pop = ((opcode >> 11U) & 1U) != 0;
    const auto extra = ((opcode >> 8U) & 1U) != 0;
    const auto list = opcode & 0xffU;
    const auto count = static_cast<std::uint32_t>(std::popcount(list)) + (extra ? 1U : 0U);
    if (!pop) {
        auto address = regs.r[13] - count * 4U;
        regs.r[13] = address;
        for (unsigned index = 0; index < 8; ++index) {
            if ((list & (1U << index)) == 0) continue;
            bus_.write32(address & ~3U, regs.r[index]);
            address += 4;
        }
        if (extra) bus_.write32(address & ~3U, regs.r[14]);
        return;
    }
    auto address = regs.r[13];
    bus_.internal(1);
    for (unsigned index = 0; index < 8; ++index) {
        if ((list & (1U << index)) == 0) continue;
        regs.r[index] = static_cast<std::uint32_t>(bus_.read32(address & ~3U));
        address += 4;
    }
    std::uint32_t pc_value{};
    if (extra) {
        pc_value = static_cast<std::uint32_t>(bus_.read32(address & ~3U));
        address += 4;
    }
    regs.r[13] = address;
    if (extra) load_pc(pc_value);
}

// Format 15 : STMIA, LDMIA. Liste vide : R15 seul sur l'ARMv4, rien sur
// l'ARMv5, base + 0x40. Base dans la liste : STM stocke l'ancienne base si
// elle est la première (ARMv4) ou toujours (ARMv5) ; LDM ne la réécrit pas.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_multiple(std::uint32_t opcode) {
    const auto rb = (opcode >> 8U) & 7U;
    const auto load = ((opcode >> 11U) & 1U) != 0;
    auto list = opcode & 0xffU;
    const auto empty = list == 0;
    if (empty && !armv5) list = 0x8000U;
    const auto new_base = regs.r[rb] + (empty ? 0x40U : static_cast<std::uint32_t>(std::popcount(list)) * 4U);
    auto address = regs.r[rb];
    if (load) bus_.internal(1);
    bool first = true;
    for (unsigned index = 0; index < 16; ++index) {
        if ((list & (1U << index)) == 0) continue;
        if (load) {
            const auto value = static_cast<std::uint32_t>(bus_.read32(address & ~3U));
            if (index == 15) branch(value);
            else regs.r[index] = value;
        } else {
            bus_.write32(address & ~3U, index == 15 ? regs.r[15] + 2 : regs.r[index]);
            if (first && !armv5) regs.r[rb] = new_base;
        }
        address += 4;
        first = false;
    }
    if (!load || (list & (1U << rb)) == 0) regs.r[rb] = new_base;
}

// Formats 16, 17 et 18 : saut conditionnel, SWI, saut inconditionnel.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_branch(std::uint32_t opcode) {
    if ((opcode >> 11U) == 0x1c) {
        branch(regs.r[15] + detail::sign_extend((opcode & 0x7ffU) << 1U, 12));
        return;
    }
    const auto code = (opcode >> 8U) & 15U;
    if (code == 15) {
        software_interrupt(opcode & 0xffU);
    } else if (code == 14) {
        undefined_instruction();
    } else if (condition(code)) {
        branch(regs.r[15] + detail::sign_extend((opcode & 0xffU) << 1U, 9));
    }
}

// Format 19 : BL en deux moitiés ; la seconde peut être BLX (ARMv5), qui
// passe en ARM à une adresse alignée sur 4.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::thumb_long_branch(std::uint32_t opcode) {
    const auto field = opcode & 0x7ffU;
    const auto half = opcode >> 11U;
    if (half == 0x1e) {
        regs.r[14] = regs.r[15] + (detail::sign_extend(field, 11) << 12U);
        return;
    }
    if (half == 0x1d && !armv5) {
        undefined_instruction();
        return;
    }
    const auto target = regs.r[14] + (field << 1U);
    regs.r[14] = (current_ + 2) | 1U;
    if (half == 0x1d) regs.thumb = false;
    branch(target);
}

} // namespace ravenemu::arm
