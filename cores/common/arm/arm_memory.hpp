#pragma once

// Accès mémoire ARM : LDR/STR, LDRH/STRH/LDRSB/LDRSH, LDRD/STRD, SWP et
// LDM/STM (GBATEK, « ARM Opcodes: Memory » et « ARM CPU Memory Alignments »).
// Inclus par cpu.hpp.
namespace ravenemu::arm {

// Mot non aligné : lu à l'adresse alignée, puis tourné de 8 bits par octet.
template <Architecture Arch, Bus BusType>
std::uint32_t Cpu<Arch, BusType>::load_word(std::uint32_t address) {
    const auto word = static_cast<std::uint32_t>(bus_.read32(address & ~3U));
    return std::rotr(word, static_cast<int>((address & 3U) * 8U));
}

// Demi-mot à une adresse impaire : l'ARM9 aligne ; l'ARM7 aligne puis tourne
// de 8 bits.
template <Architecture Arch, Bus BusType>
std::uint32_t Cpu<Arch, BusType>::load_halfword(std::uint32_t address) {
    const auto half = static_cast<std::uint32_t>(bus_.read16(address & ~1U)) & 0xffffU;
    if constexpr (armv5) return half;
    else return std::rotr(half, static_cast<int>((address & 1U) * 8U));
}

// Demi-mot signé à une adresse impaire : l'ARM9 aligne ; l'ARM7 lit l'octet.
template <Architecture Arch, Bus BusType>
std::uint32_t Cpu<Arch, BusType>::load_signed_halfword(std::uint32_t address) {
    if constexpr (!armv5) {
        if ((address & 1U) != 0) return load_signed_byte(address);
    }
    return detail::sign_extend(static_cast<std::uint32_t>(bus_.read16(address & ~1U)) & 0xffffU, 16);
}

template <Architecture Arch, Bus BusType>
std::uint32_t Cpu<Arch, BusType>::load_signed_byte(std::uint32_t address) {
    return detail::sign_extend(static_cast<std::uint32_t>(bus_.read8(address)) & 0xffU, 8);
}

// LDR, STR, LDRB, STRB. R15 stocké vaut PC + 12.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_single_transfer(std::uint32_t opcode, std::uint32_t offset) {
    const auto pre = ((opcode >> 24U) & 1U) != 0;
    const auto up = ((opcode >> 23U) & 1U) != 0;
    const auto byte = ((opcode >> 22U) & 1U) != 0;
    const auto write_back = !pre || ((opcode >> 21U) & 1U) != 0;
    const auto rn = (opcode >> 16U) & 15U;
    const auto rd = (opcode >> 12U) & 15U;
    const auto base = regs.r[rn];
    const auto target = up ? base + offset : base - offset;
    const auto address = pre ? target : base;
    if (((opcode >> 20U) & 1U) != 0) {
        const auto value = byte ? static_cast<std::uint32_t>(bus_.read8(address)) & 0xffU : load_word(address);
        bus_.internal(1);
        // La valeur lue l'emporte sur la réécriture de la base quand Rn = Rd.
        if (write_back && rn != rd) write_register(rn, target);
        if (rd == 15) load_pc(value);
        else regs.r[rd] = value;
        return;
    }
    const auto value = rd == 15 ? regs.r[15] + 4 : regs.r[rd];
    if (byte) bus_.write8(address, value & 0xffU);
    else bus_.write32(address & ~3U, value);
    if (write_back) write_register(rn, target);
}

// LDRH, STRH, LDRSB, LDRSH ; LDRD et STRD sur l'ARMv5TE. Sur l'ARMv4, les
// codes de LDRD et STRD écrivent un demi-mot, comme le cœur GBA.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_halfword_transfer(std::uint32_t opcode) {
    const auto pre = ((opcode >> 24U) & 1U) != 0;
    const auto up = ((opcode >> 23U) & 1U) != 0;
    const auto immediate = ((opcode >> 22U) & 1U) != 0;
    const auto write_back = !pre || ((opcode >> 21U) & 1U) != 0;
    const auto load = ((opcode >> 20U) & 1U) != 0;
    const auto rn = (opcode >> 16U) & 15U;
    const auto rd = (opcode >> 12U) & 15U;
    const auto kind = (opcode >> 5U) & 3U;
    const auto offset = immediate ? ((opcode >> 4U) & 0xf0U) | (opcode & 15U) : regs.r[opcode & 15U];
    const auto base = regs.r[rn];
    const auto target = up ? base + offset : base - offset;
    const auto address = pre ? target : base;
    if (armv5 && !load && kind >= 2) {
        if ((rd & 1U) != 0) {  // Rd impair : indéfini
            undefined_instruction();
            return;
        }
        arm_doubleword_transfer(opcode, address);
        const auto second = rd + 1;
        if (write_back && (kind == 3 || (rn != rd && rn != second))) write_register(rn, target);
        return;
    }
    if (!load) {
        bus_.write16(address & ~1U, (rd == 15 ? regs.r[15] + 4 : regs.r[rd]) & 0xffffU);
        if (write_back) write_register(rn, target);
        return;
    }
    std::uint32_t value{};
    if (kind == 1) value = load_halfword(address);
    else if (kind == 2) value = load_signed_byte(address);
    else value = load_signed_halfword(address);
    bus_.internal(1);
    if (write_back && rn != rd) write_register(rn, target);
    write_register(rd, value);
}

// LDRD (code 2) et STRD (code 3) : Rd pair, deux mots consécutifs. L'adresse
// alignée sur 4 suffit sur DS (GBATEK).
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_doubleword_transfer(std::uint32_t opcode, std::uint32_t address) {
    const auto rd = (opcode >> 12U) & 15U;
    const auto aligned = address & ~3U;
    if (((opcode >> 5U) & 3U) == 2) {
        const auto low = static_cast<std::uint32_t>(bus_.read32(aligned));
        const auto high = static_cast<std::uint32_t>(bus_.read32(aligned + 4));
        bus_.internal(1);
        regs.r[rd] = low;
        if (rd + 1 == 15) load_pc(high);
        else regs.r[rd + 1] = high;
        return;
    }
    bus_.write32(aligned, regs.r[rd]);
    bus_.write32(aligned + 4, rd + 1 == 15 ? regs.r[15] + 4 : regs.r[rd + 1]);
}

// SWP, SWPB : lecture tournée comme LDR, écriture non tournée comme STR.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_swap(std::uint32_t opcode) {
    const auto address = regs.r[(opcode >> 16U) & 15U];
    const auto source = regs.r[opcode & 15U];
    std::uint32_t value{};
    if (((opcode >> 22U) & 1U) != 0) {
        value = static_cast<std::uint32_t>(bus_.read8(address)) & 0xffU;
        bus_.write8(address, source & 0xffU);
    } else {
        value = load_word(address);
        bus_.write32(address & ~3U, source);
    }
    bus_.internal(1);
    write_register((opcode >> 12U) & 15U, value);
}

// LDM, STM. Les registres vont toujours aux adresses croissantes, le plus
// petit à l'adresse la plus basse. Liste vide : R15 seul sur l'ARMv4, rien
// sur l'ARMv5, et la base avance de 0x40 dans les deux cas.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_block_transfer(std::uint32_t opcode) {
    const auto pre = ((opcode >> 24U) & 1U) != 0;
    const auto up = ((opcode >> 23U) & 1U) != 0;
    const auto rn = (opcode >> 16U) & 15U;
    auto list = opcode & 0xffffU;
    const auto empty = list == 0;
    if (empty && !armv5) list = 0x8000U;
    const auto bytes = empty ? 0x40U : static_cast<std::uint32_t>(std::popcount(list)) * 4U;
    const auto base = regs.r[rn];
    const auto lowest = up ? base : base - bytes;
    const auto address = pre == up ? lowest + 4 : lowest;
    const auto new_base = up ? base + bytes : base - bytes;
    if (list == 0) {
        if (((opcode >> 21U) & 1U) != 0 && rn != 15) regs.r[rn] = new_base;
        return;
    }
    if (((opcode >> 20U) & 1U) != 0) arm_block_load(opcode, list, address, new_base);
    else arm_block_store(opcode, list, address, new_base);
}

// STM. Base réécrite et présente dans la liste : l'ARMv4 stocke l'ancienne
// base si elle est le premier registre, la nouvelle sinon ; l'ARMv5 stocke
// toujours l'ancienne.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_block_store(std::uint32_t opcode, std::uint32_t list, std::uint32_t address,
                                         std::uint32_t new_base) {
    const auto rn = (opcode >> 16U) & 15U;
    const auto write_back = ((opcode >> 21U) & 1U) != 0 && rn != 15;
    const auto user_bank = ((opcode >> 22U) & 1U) != 0;
    bool first = true;
    for (unsigned index = 0; index < 16; ++index) {
        if ((list & (1U << index)) == 0) continue;
        std::uint32_t value{};
        if (index == 15) value = regs.r[15] + 4;
        else value = user_bank ? regs.user_register(index) : regs.r[index];
        bus_.write32(address & ~3U, value);
        address += 4;
        if (first && write_back && !armv5) regs.r[rn] = new_base;
        first = false;
    }
    if (write_back && armv5) regs.r[rn] = new_base;
}

// LDM. Base réécrite et présente dans la liste : jamais réécrite sur l'ARMv4 ;
// sur l'ARMv5, réécrite si elle est le seul registre ou pas le dernier.
// Avec « ^ » : banque utilisateur, ou CPSR = SPSR si R15 est chargé.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_block_load(std::uint32_t opcode, std::uint32_t list, std::uint32_t address,
                                        std::uint32_t new_base) {
    const auto rn = (opcode >> 16U) & 15U;
    const auto psr_bit = ((opcode >> 22U) & 1U) != 0;
    const auto loads_pc = (list & 0x8000U) != 0;
    const auto user_bank = psr_bit && !loads_pc;
    auto write_back = ((opcode >> 21U) & 1U) != 0 && rn != 15;
    if ((list & (1U << rn)) != 0) {
        if constexpr (armv5) {
            const auto last = 31U - static_cast<unsigned>(std::countl_zero(list));
            write_back = write_back && (std::popcount(list) == 1 || rn != last);
        } else {
            write_back = false;
        }
    }
    bus_.internal(1);
    std::uint32_t pc_value{};
    for (unsigned index = 0; index < 16; ++index) {
        if ((list & (1U << index)) == 0) continue;
        const auto value = static_cast<std::uint32_t>(bus_.read32(address & ~3U));
        address += 4;
        if (index == 15) pc_value = value;
        else if (user_bank) regs.set_user_register(index, value);
        else regs.r[index] = value;
    }
    if (write_back) regs.r[rn] = new_base;
    if (!loads_pc) return;
    if (psr_bit) {
        regs.set_cpsr(regs.spsr(), 0xffff'ffffU);
        branch(pc_value);
    } else {
        load_pc(pc_value);
    }
}

} // namespace ravenemu::arm
