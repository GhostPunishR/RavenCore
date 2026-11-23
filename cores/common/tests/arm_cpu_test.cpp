#include "arm/cpu.hpp"

#include "check.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <vector>

/**
 * CPU ARM commun, ARMv4T et ARMv5TE. Chaque programme est assemblé par
 * llvm-mc (source en commentaire), chargé dans une mémoire factice de 64 Kio,
 * puis exécuté instruction par instruction jusqu'à sa boucle finale `b .`.
 * Les valeurs attendues suivent GBATEK (« ARM CPU Reference »).
 */
namespace {

using namespace ravenemu::arm;
using ravenemu::testing::check;

constexpr std::uint32_t arm_code = 0x1000;
constexpr std::uint32_t thumb_code = 0x2000;
constexpr std::uint32_t data = 0x8000;

struct CoprocessorWrite {
    CoprocessorAccess access;
    std::uint32_t value;
};

struct SoftwareInterrupt {
    std::uint32_t comment;
    bool thumb;
};

// Bus factice : un cycle par accès, plus les cycles internes signalés.
class FakeBus {
public:
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x10000);
    long cycles = 0;
    bool has_cp15 = false;
    bool hle_swi = false;
    std::vector<CoprocessorWrite> cp_writes;
    std::vector<SoftwareInterrupt> swis;

    [[nodiscard]] std::uint32_t peek(std::uint32_t address, unsigned size) const {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < size; ++index)
            value |= std::uint32_t{memory[(address + index) & 0xffffU]} << (8U * index);
        return value;
    }
    void poke(std::uint32_t address, std::uint32_t value, unsigned size) {
        for (unsigned index = 0; index < size; ++index)
            memory[(address + index) & 0xffffU] = static_cast<std::uint8_t>(value >> (8U * index));
    }

    std::uint32_t code16(std::uint32_t address) { return access(address, 2); }
    std::uint32_t code32(std::uint32_t address) { return access(address, 4); }
    std::uint32_t read8(std::uint32_t address) { return access(address, 1); }
    std::uint32_t read16(std::uint32_t address) {
        check((address & 1U) == 0, "lecture de 16 bits non alignée par le CPU");
        return access(address, 2);
    }
    std::uint32_t read32(std::uint32_t address) {
        check((address & 3U) == 0, "lecture de 32 bits non alignée par le CPU");
        return access(address, 4);
    }
    void write8(std::uint32_t address, std::uint32_t value) { store(address, value, 1); }
    void write16(std::uint32_t address, std::uint32_t value) {
        check((address & 1U) == 0, "écriture de 16 bits non alignée par le CPU");
        store(address, value, 2);
    }
    void write32(std::uint32_t address, std::uint32_t value) {
        check((address & 3U) == 0, "écriture de 32 bits non alignée par le CPU");
        store(address, value, 4);
    }
    void internal(int count) { cycles += count; }
    // Rechargement du pipeline : deux lectures de code à la cible.
    void refill(std::uint32_t, bool) { cycles += 2; }
    std::optional<std::uint32_t> coprocessor_read(CoprocessorAccess access) {
        if (!has_cp15 || access.coprocessor != 15) return std::nullopt;
        return 0x4105'9461U;  // identifiant de l'ARM946E-S
    }
    bool coprocessor_write(CoprocessorAccess access, std::uint32_t value) {
        if (!has_cp15 || access.coprocessor != 15) return false;
        cp_writes.push_back({access, value});
        return true;
    }
    bool software_interrupt(std::uint32_t comment, bool thumb) {
        swis.push_back({comment, thumb});
        return hle_swi;
    }

private:
    std::uint32_t access(std::uint32_t address, unsigned size) {
        ++cycles;
        return peek(address, size);
    }
    void store(std::uint32_t address, std::uint32_t value, unsigned size) {
        ++cycles;
        poke(address, value, size);
    }
};

template <Architecture Arch>
class Fixture {
public:
    static constexpr bool armv5 = Arch == Architecture::armv5te;

    Fixture() {
        bus.poke(data, 0xa493'8211U, 4);
        bus.poke(data + 4, 0x0bad'f00dU, 4);
        cpu.reset();
    }
    void arm(std::initializer_list<std::uint32_t> code, std::uint32_t at = arm_code) {
        load_arm(at, code);
        cpu.jump(at, false);
    }
    void thumb(std::initializer_list<std::uint16_t> code, std::uint32_t at = thumb_code) {
        load_thumb(at, code);
        cpu.jump(at, true);
    }
    void load_arm(std::uint32_t at, std::initializer_list<std::uint32_t> code) {
        for (const auto word : code) {
            bus.poke(at, word, 4);
            at += 4;
        }
    }
    void load_thumb(std::uint32_t at, std::initializer_list<std::uint16_t> code) {
        for (const auto half : code) {
            bus.poke(at, half, 2);
            at += 2;
        }
    }
    // Jusqu'à `b .` : PC ne bouge plus après une instruction.
    void run() {
        for (int step = 0; step < 1000; ++step) {
            const auto pc = cpu.pc();
            cpu.step();
            if (cpu.pc() == pc) return;
        }
        throw std::runtime_error("programme de test sans boucle finale");
    }
    [[nodiscard]] std::uint32_t r(unsigned index) const { return cpu.regs.r[index]; }
    [[nodiscard]] std::uint32_t word(std::uint32_t address) const { return bus.peek(address, 4); }
    // Passe en mode système, IRQ permises : l'état de départ des tests d'exceptions.
    void system_mode() {
        cpu.regs.switch_mode(mode::system);
        cpu.regs.irq_disabled = false;
    }

    FakeBus bus;
    Cpu<Arch, FakeBus> cpu{bus};
};

// Drapeaux N, Z, C et V d'un CPSR lu par MRS.
constexpr std::uint32_t nzcv(std::uint32_t psr) { return psr >> 28U; }

template <Architecture Arch>
void alu_test() {
    Fixture<Arch> f;
    f.arm({
        0xe3a00102U,  // mov r0, #0x80000000
        0xe0901000U,  // adds r1, r0, r0
        0xe10f2000U,  // mrs r2, cpsr
        0xe3a03001U,  // mov r3, #1
        0xe2534002U,  // subs r4, r3, #2
        0xe10f5000U,  // mrs r5, cpsr
        0xe1b06020U,  // movs r6, r0, lsr #32
        0xe10f7000U,  // mrs r7, cpsr
        0xe3a08003U,  // mov r8, #3
        0xe1b09068U,  // movs r9, r8, rrx
        0xe28fa000U,  // add r10, pc, #0
        0xe3a0b000U,  // mov r11, #0
        0xe08fcb1bU,  // add r12, pc, r11, lsl r11
        0xeafffffeU,  // b .
    });
    f.run();
    check(f.r(1) == 0 && nzcv(f.r(2)) == 0x7, "ADDS : retenue, débordement et zéro");
    check(f.r(4) == 0xffff'ffffU && nzcv(f.r(5)) == 0x8, "SUBS : emprunt et signe");
    check(f.r(6) == 0 && nzcv(f.r(7)) == 0x6, "LSR#32 : résultat nul, C = bit 31");
    check(f.r(9) == 0x8000'0001U && f.cpu.regs.c && f.cpu.regs.n, "RRX : C entre au bit 31, le bit 0 sort");
    check(f.r(10) == arm_code + 0x28 + 8, "R15 lu comme PC + 8");
    check(f.r(12) == arm_code + 0x30 + 12, "R15 lu comme PC + 12 avec un décalage par registre");
}

template <Architecture Arch>
void status_register_test() {
    Fixture<Arch> f;
    f.arm({
        0xe10f0000U,  // mrs r0, cpsr
        0xe3c0101fU,  // bic r1, r0, #0x1f
        0xe3811012U,  // orr r1, r1, #0x12
        0xe121f001U,  // msr cpsr_c, r1
        0xe3a0dc01U,  // mov sp, #0x100
        0xe381101fU,  // orr r1, r1, #0x1f
        0xe121f001U,  // msr cpsr_c, r1
        0xe3a0dc02U,  // mov sp, #0x200
        0xe3c1100fU,  // bic r1, r1, #0x0f
        0xe121f001U,  // msr cpsr_c, r1
        0xe10f2000U,  // mrs r2, cpsr
        0xe329f01fU,  // msr cpsr_fc, #0x1f
        0xe10f3000U,  // mrs r3, cpsr
        0xe328f33eU,  // msr cpsr_f, #0xf8000000
        0xe10f4000U,  // mrs r4, cpsr
        0xeafffffeU,  // b .
    });
    f.run();
    check(f.r(0) == 0xd3U, "mise sous tension : superviseur, IRQ et FIQ masquées");
    check(f.r(2) == 0xd0U && f.r(3) == 0xd0U, "mode utilisateur : les bits de contrôle ne s'écrivent plus");
    check(f.r(4) == (Fixture<Arch>::armv5 ? 0xf800'00d0U : 0xf000'00d0U), "drapeaux par MSR, Q sur l'ARMv5 seul");
    check(f.r(13) == 0x200, "utilisateur et système partagent SP");
    f.cpu.regs.switch_mode(mode::irq);
    check(f.r(13) == 0x100, "SP de l'IRQ dans sa banque");
}

template <Architecture Arch>
void multiply_test() {
    Fixture<Arch> f;
    f.arm({
        0xe3a00007U,  // mov r0, #7
        0xe3e01002U,  // mvn r1, #2
        0xe0120190U,  // muls r2, r0, r1
        0xe3a03064U,  // mov r3, #100
        0xe0243190U,  // mla r4, r0, r1, r3
        0xe0865091U,  // umull r5, r6, r1, r0
        0xe0c87091U,  // smull r7, r8, r1, r0
        0xe3a09001U,  // mov r9, #1
        0xe3a0a000U,  // mov r10, #0
        0xe0ea9091U,  // smlal r9, r10, r1, r0
        0xeafffffeU,  // b .
    });
    f.run();
    check(f.r(2) == 0xffff'ffebU && f.cpu.regs.n && !f.cpu.regs.z, "MULS : 7 × -3, N levé");
    check(f.r(4) == 79, "MLA : 7 × -3 + 100");
    check(f.r(5) == 0xffff'ffebU && f.r(6) == 6, "UMULL : 0xFFFFFFFD × 7 sur 64 bits");
    check(f.r(7) == 0xffff'ffebU && f.r(8) == 0xffff'ffffU, "SMULL : -3 × 7 sur 64 bits");
    check(f.r(9) == 0xffff'ffecU && f.r(10) == 0xffff'ffffU, "SMLAL : 1 + -21");
}

void dsp_test() {
    Fixture<Architecture::armv5te> f;
    f.arm({
        0xe3a00102U,  // mov r0, #0x80000000
        0xe16f1f10U,  // clz r1, r0
        0xe3a02000U,  // mov r2, #0
        0xe16f3f12U,  // clz r3, r2
        0xe3e04102U,  // mvn r4, #0x80000000
        0xe3a05001U,  // mov r5, #1
        0xe1056054U,  // qadd r6, r4, r5
        0xe10f7000U,  // mrs r7, cpsr
        0xe328f000U,  // msr cpsr_f, #0
        0xe1608055U,  // qdsub r8, r5, r0
        0xe10f9000U,  // mrs r9, cpsr
        0xe328f000U,  // msr cpsr_f, #0
        0xe3a0a002U,  // mov r10, #2
        0xe38aa803U,  // orr r10, r10, #0x30000
        0xe3e0b000U,  // mvn r11, #0
        0xe16c0bcaU,  // smulbt r12, r10, r11
        0xe1600beaU,  // smultt r0, r10, r11
        0xe1014a8aU,  // smlabb r1, r10, r10, r4
        0xe10f2000U,  // mrs r2, cpsr
        0xe1230aa4U,  // smulwb r3, r4, r10
        0xe3a05005U,  // mov r5, #5
        0xe3a06000U,  // mov r6, #0
        0xe1465a8bU,  // smlalbb r5, r6, r11, r10
        0xeafffffeU,  // b .
    });
    f.run();
    check((f.r(7) >> 27U) == 1, "QADD sature et lève Q, sans toucher NZCV");
    check(f.r(8) == 0x7fff'ffffU && (f.r(9) >> 27U) == 1, "QDSUB : 1 - 2 × 0x80000000 sature");
    check(f.r(12) == 0xffff'fffeU && f.r(0) == 0xffff'fffdU, "SMULBT et SMULTT");
    check(f.r(1) == 0x8000'0003U && (f.r(2) >> 27U) == 1, "SMLABB : débordement signalé par Q, sans saturer");
    check(f.r(3) == 0xffffU, "SMULWB : bits 47-16 du produit");
    check(f.r(5) == 3 && f.r(6) == 0, "SMLALBB : 5 + -1 × 2 sur 64 bits");
}

void count_leading_zeros_test() {
    Fixture<Architecture::armv5te> f;
    f.arm({
        0xe3a00102U,  // mov r0, #0x80000000
        0xe16f1f10U,  // clz r1, r0
        0xe3a02000U,  // mov r2, #0
        0xe16f3f12U,  // clz r3, r2
        0xe3a04801U,  // mov r4, #0x10000
        0xe16f5f14U,  // clz r5, r4
        0xeafffffeU,  // b .
    });
    f.run();
    check(f.r(1) == 0 && f.r(3) == 32 && f.r(5) == 15, "CLZ");
    Fixture<Architecture::armv4t> old;
    old.arm({0xe16f1f10U});  // clz r1, r0
    old.cpu.step();
    check(old.cpu.regs.mode == mode::undefined && old.cpu.pc() == 0x04, "CLZ indéfini sur l'ARMv4");
}

template <Architecture Arch>
void load_store_test() {
    Fixture<Arch> f;
    f.arm({
        0xe3a00902U,  // mov r0, #0x8000
        0xe5901001U,  // ldr r1, [r0, #1]
        0xe1d020b1U,  // ldrh r2, [r0, #1]
        0xe1d030f3U,  // ldrsh r3, [r0, #3]
        0xe1d040d2U,  // ldrsb r4, [r0, #2]
        0xe5d05003U,  // ldrb r5, [r0, #3]
        0xe3a06902U,  // mov r6, #0x8000
        0xe5b67004U,  // ldr r7, [r6, #4]!
        0xe4168004U,  // ldr r8, [r6], #-4
        0xe5b66004U,  // ldr r6, [r6, #4]!
        0xe3a09c81U,  // mov r9, #0x8100
        0xe589f000U,  // str pc, [r9]
        0xe5c95005U,  // strb r5, [r9, #5]
        0xe1c940b9U,  // strh r4, [r9, #9]
        0xeafffffeU,  // b .
    });
    f.run();
    constexpr bool armv5 = Fixture<Arch>::armv5;
    check(f.r(1) == 0x11a4'9382U, "LDR non aligné : mot tourné de 8 bits");
    check(f.r(2) == (armv5 ? 0x8211U : 0x1100'0082U), "LDRH impair : aligné (ARM9) ou tourné (ARM7)");
    check(f.r(3) == (armv5 ? 0xffff'a493U : 0xffff'ffa4U), "LDRSH impair : aligné (ARM9) ou octet signé (ARM7)");
    check(f.r(4) == 0xffff'ff93U && f.r(5) == 0xa4, "LDRSB et LDRB");
    check(f.r(7) == 0x0bad'f00dU && f.r(8) == 0x0bad'f00dU, "pré- et post-indexation");
    check(f.r(6) == 0x0bad'f00dU, "Rn = Rd : la valeur lue l'emporte sur la réécriture");
    check(f.word(0x8100) == arm_code + 0x2c + 12, "STR PC stocke PC + 12");
    check(f.word(0x8104) == 0xa400U && f.word(0x8108) == 0xff93U, "STRB, et STRH impair aligné");
}

template <Architecture Arch>
void swap_test() {
    Fixture<Arch> f;
    f.arm({
        0xe3a00902U,  // mov r0, #0x8000
        0xe3a04055U,  // mov r4, #0x55
        0xe1005094U,  // swp r5, r4, [r0]
        0xe1406094U,  // swpb r6, r4, [r0]
        0xeafffffeU,  // b .
    });
    f.run();
    check(f.r(5) == 0xa493'8211U && f.r(6) == 0x55 && f.word(data) == 0x55, "SWP et SWPB");
}

void doubleword_test() {
    Fixture<Architecture::armv5te> f;
    f.arm({
        0xe3a00902U,  // mov r0, #0x8000
        0xe1c020d0U,  // ldrd r2, r3, [r0]
        0xe3a01c81U,  // mov r1, #0x8100
        0xe1e120f8U,  // strd r2, r3, [r1, #8]!
        0xeafffffeU,  // b .
    });
    f.run();
    check(f.r(2) == 0xa493'8211U && f.r(3) == 0x0bad'f00dU, "LDRD");
    check(f.word(0x8108) == 0xa493'8211U && f.word(0x810c) == 0x0bad'f00dU && f.r(1) == 0x8108, "STRD et réécriture");
}

template <Architecture Arch>
void block_transfer_test() {
    Fixture<Arch> f;
    f.arm({
        0xe3a00902U,  // mov r0, #0x8000
        0xe3a01001U,  // mov r1, #1
        0xe3a02002U,  // mov r2, #2
        0xe3a03003U,  // mov r3, #3
        0xe8a00003U,  // stmia r0!, {r0, r1}
        0xe3a04c81U,  // mov r4, #0x8100
        0xe8a40012U,  // stmia r4!, {r1, r4}
        0xe3a05c81U,  // mov r5, #0x8100
        0xe8b50022U,  // ldmia r5!, {r1, r5}
        0xe3a06c81U,  // mov r6, #0x8100
        0xe8b600c0U,  // ldmia r6!, {r6, r7}
        0xe3a08c82U,  // mov r8, #0x8200
        0xe928000eU,  // stmdb r8!, {r1, r2, r3}
        0xe9980c00U,  // ldmib r8, {r10, r11}
        0xeafffffeU,  // b .
    });
    f.run();
    constexpr bool armv5 = Fixture<Arch>::armv5;
    check(f.word(data) == data && f.r(0) == 0x8008, "STM : base en tête, ancienne valeur stockée");
    check(f.word(0x8104) == (armv5 ? 0x8100U : 0x8108U), "STM : base plus loin, nouvelle (ARM7) ou ancienne (ARM9)");
    check(f.r(5) == f.word(0x8104), "LDM : base en dernier, valeur lue gardée");
    check(f.r(6) == (armv5 ? 0x8108U : 1U), "LDM : base pas en dernier, réécrite sur l'ARM9 seul");
    check(f.r(8) == 0x81f4 && f.word(0x81f4) == 1 && f.word(0x81fc) == 3, "STMDB");
    check(f.r(10) == 2 && f.r(11) == 3, "LDMIB");
}

template <Architecture Arch>
void empty_list_test() {
    constexpr bool armv5 = Fixture<Arch>::armv5;
    Fixture<Arch> store;
    store.arm({0xe3a01c81U, 0xe8a10000U, 0xeafffffeU});  // mov r1, #0x8100 ; stmia r1!, {} ; b .
    store.run();
    check(store.r(1) == 0x8140, "STM sans registre : base + 0x40");
    check(store.word(0x8100) == (armv5 ? 0U : arm_code + 4 + 12), "STM sans registre : R15 stocké sur l'ARM7 seul");
    Fixture<Arch> load;
    load.bus.poke(0x8100, arm_code + 12, 4);
    load.arm({0xe3a01c81U, 0xe8b10000U, 0xe3a02007U, 0xeafffffeU});  // mov r1 ; ldmia r1!, {} ; mov r2, #7 ; b .
    load.run();
    check(load.r(1) == 0x8140 && load.r(2) == (armv5 ? 7U : 0U), "LDM sans registre : R15 chargé sur l'ARM7 seul");
}

void user_bank_test() {
    Fixture<Architecture::armv5te> f;
    f.cpu.regs.switch_mode(mode::system);
    f.cpu.regs.r[13] = 0x1234;
    f.cpu.regs.switch_mode(mode::irq);
    f.cpu.regs.r[13] = 0x5678;
    f.cpu.regs.r[0] = 0x8100;
    f.arm({0xe8c02000U, 0xeafffffeU});  // stmia r0, {r13}^ ; b .
    f.run();
    check(f.word(0x8100) == 0x1234, "STM ^ : SP de la banque utilisateur");

    Fixture<Architecture::armv4t> ret;
    ret.cpu.regs.set_spsr(mode::user, 0xffff'ffffU);
    ret.cpu.regs.r[13] = 0x9000;
    ret.bus.poke(0x9000, 0x77, 4);
    ret.bus.poke(0x9004, 0x1100, 4);
    ret.load_arm(0x1100, {0xeafffffeU});  // b .
    ret.arm({0xe8fd8001U});               // ldmfd sp!, {r0, pc}^
    ret.run();
    check(ret.cpu.regs.mode == mode::user && ret.r(0) == 0x77 && ret.cpu.pc() == 0x1100,
          "LDM ^ avec PC : retour d'exception, CPSR = SPSR");
    ret.cpu.regs.switch_mode(mode::supervisor);
    check(ret.r(13) == 0x9008, "LDM ^ avec PC : SP du superviseur réécrit");
}

template <Architecture Arch>
void branch_test() {
    Fixture<Arch> f;
    f.load_thumb(thumb_code, {0x222aU, 0xe7feU});  // movs r2, #42 ; b .
    f.arm({
        0xe3a00a02U,  // mov r0, #0x2000
        0xe3800001U,  // orr r0, r0, #1
        0xeb000000U,  // bl .Lsub
        0xe12fff10U,  // bx r0
        // .Lsub:
        0xe1a0100eU,  // mov r1, lr
        0xe12fff1eU,  // bx lr
    });
    f.run();
    check(f.r(1) == arm_code + 0x0c, "BL range l'adresse suivante dans LR");
    check(f.cpu.regs.thumb && f.r(2) == 42 && f.cpu.pc() == thumb_code + 2, "BX vers du Thumb");
}

// LDR PC : l'ARMv5 change d'état d'après le bit 0, sauf en mode pré-ARMv5 de CP15.
template <Architecture Arch>
void load_pc_test(bool interworking) {
    Fixture<Arch> f;
    f.cpu.load_interworking = interworking;
    const auto thumb = Fixture<Arch>::armv5 && interworking;
    if (thumb) f.load_thumb(thumb_code, {0x222aU, 0xe7feU});  // movs r2, #42 ; b .
    else f.load_arm(thumb_code, {0xe3a0202aU, 0xeafffffeU});  // mov r2, #42 ; b .
    f.bus.poke(data, thumb_code | 1U, 4);
    f.arm({0xe3a00902U, 0xe590f000U});  // mov r0, #0x8000 ; ldr pc, [r0]
    f.run();
    check(f.r(2) == 42 && f.cpu.regs.thumb == thumb, "LDR PC : état choisi par le bit 0 sur l'ARMv5 seul");
}

void exchange_link_test() {
    Fixture<Architecture::armv5te> immediate;
    immediate.load_thumb(thumb_code, {0x222aU, 0xe7feU});  // movs r2, #42 ; b .
    immediate.arm({0xfa0003feU});                          // blx 0x2000
    immediate.run();
    check(immediate.r(14) == arm_code + 4 && immediate.r(2) == 42 && immediate.cpu.regs.thumb, "BLX immédiat");
    Fixture<Architecture::armv5te> reg;
    reg.load_thumb(thumb_code, {0x222aU, 0xe7feU});
    reg.arm({0xe3a00a02U, 0xe3800001U, 0xe12fff30U});  // mov r0, #0x2000 ; orr r0, r0, #1 ; blx r0
    reg.run();
    check(reg.r(14) == arm_code + 0x0c && reg.cpu.regs.thumb, "BLX registre");
}

template <Architecture Arch>
void software_interrupt_test() {
    Fixture<Arch> hle;
    hle.bus.hle_swi = true;
    hle.arm({0xef050000U, 0xe3a01001U, 0xeafffffeU});  // swi #0x050000 ; mov r1, #1 ; b .
    hle.run();
    check(hle.bus.swis.size() == 1 && hle.bus.swis[0].comment == 0x050000 && !hle.bus.swis[0].thumb,
          "SWI pris par l'hôte : champ de commentaire transmis");
    check(hle.r(1) == 1 && hle.cpu.regs.mode == mode::supervisor, "SWI pris par l'hôte : l'exécution continue");

    Fixture<Arch> real;
    real.system_mode();
    const auto before = real.cpu.regs.cpsr();
    real.arm({0xef000012U});  // swi #0x12
    real.cpu.step();
    check(real.cpu.regs.mode == mode::supervisor && real.cpu.pc() == 0x08 && real.r(14) == arm_code + 4,
          "SWI : superviseur, vecteur 0x08, retour dans LR");
    check(real.cpu.regs.spsr() == before && real.cpu.regs.irq_disabled, "SWI : CPSR sauvé, IRQ masquées");
}

template <Architecture Arch>
void undefined_and_breakpoint_test() {
    Fixture<Arch> undefined;
    undefined.arm({0xe7f000f0U});  // udf #0
    undefined.cpu.step();
    check(undefined.cpu.regs.mode == mode::undefined && undefined.cpu.pc() == 0x04 && undefined.r(14) == arm_code + 4,
          "instruction indéfinie");
    Fixture<Arch> breakpoint;
    breakpoint.cpu.vector_base = 0xffff'0000U;
    breakpoint.arm({0xe1200070U});  // bkpt #0
    breakpoint.cpu.step();
    if constexpr (Fixture<Arch>::armv5)
        check(breakpoint.cpu.regs.mode == mode::abort && breakpoint.cpu.pc() == 0xffff'000cU &&
                  breakpoint.r(14) == arm_code + 4,
              "BKPT : abandon de préchargement, vecteurs hauts");
    else
        check(breakpoint.cpu.regs.mode == mode::undefined, "BKPT indéfini sur l'ARMv4");
}

template <Architecture Arch>
void coprocessor_test() {
    Fixture<Arch> f;
    f.bus.has_cp15 = true;
    f.cpu.regs.r[1] = 0x55;
    f.arm({0xee100f10U, 0xee071f90U, 0xeafffffeU});  // mrc p15,0,r0,c0,c0,0 ; mcr p15,0,r1,c7,c0,4 ; b .
    f.run();
    check(f.r(0) == 0x4105'9461U, "MRC lit le coprocesseur");
    check(f.bus.cp_writes.size() == 1 && f.bus.cp_writes[0].access.crn == 7 && f.bus.cp_writes[0].access.opcode2 == 4 &&
              f.bus.cp_writes[0].value == 0x55,
          "MCR écrit le coprocesseur");
    Fixture<Arch> absent;
    absent.arm({0xee100f10U});
    absent.cpu.step();
    check(absent.cpu.regs.mode == mode::undefined, "coprocesseur absent : instruction indéfinie");
}

template <Architecture Arch>
void interrupt_test() {
    Fixture<Arch> f;
    f.load_arm(0x18, {0xe25ef004U});  // subs pc, lr, #4
    f.system_mode();
    f.arm({0xe3a00001U, 0xe3a01002U, 0xeafffffeU});  // mov r0, #1 ; mov r1, #2 ; b .
    f.cpu.step();
    const auto before = f.cpu.regs.cpsr();
    f.cpu.set_irq_line(true);
    f.cpu.step();
    check(f.cpu.regs.mode == mode::irq && f.cpu.pc() == 0x18 && f.r(14) == arm_code + 8, "entrée d'IRQ");
    check(f.cpu.regs.spsr() == before && f.cpu.regs.irq_disabled, "IRQ : CPSR sauvé, IRQ masquées");
    f.cpu.step();  // la ligne reste levée, mais les IRQ sont masquées
    check(f.cpu.regs.mode == mode::system && f.cpu.pc() == arm_code + 4, "SUBS PC, LR, #4 : retour d'IRQ");
    f.cpu.set_irq_line(false);
    f.run();
    check(f.r(0) == 1 && f.r(1) == 2, "l'instruction interrompue s'exécute au retour");
}

void never_condition_test() {
    Fixture<Architecture::armv4t> f;
    f.arm({0xf3a01001U, 0xeafffffeU});  // movnv r1, #1 ; b .
    f.run();
    check(f.r(1) == 0 && f.cpu.regs.mode == mode::supervisor, "condition NV jamais exécutée sur l'ARMv4");
}

template <Architecture Arch>
void cycles_test() {
    Fixture<Arch> f;
    f.arm({0xe0120190U, 0xe0120190U, 0xe08fcb1bU, 0xe5901001U});  // muls r2,r0,r1 ×2 ; add r12,pc,r11,lsl r11 ; ldr
    f.cpu.regs.r[1] = 0xffff'fffdU;
    const auto measure = [&f] {
        const auto start = f.bus.cycles;
        f.cpu.step();
        return f.bus.cycles - start;
    };
    check(measure() == 2, "MUL : un cycle de multiplieur pour un opérande signé court");
    f.cpu.regs.r[1] = 0x1234'5678U;
    check(measure() == 5, "MUL : quatre cycles de multiplieur pour un opérande long");
    check(measure() == 2, "décalage par registre : un cycle interne");
    f.cpu.regs.r[0] = data;
    check(measure() == 3, "LDR : lecture du code, de la donnée, et un cycle interne");
    f.load_arm(arm_code + 16, {0xeafffffeU});  // b .
    check(measure() == 3, "B : 2S + 1N, rechargement du pipeline compris");
}

template <Architecture Arch>
void thumb_alu_test() {
    Fixture<Arch> f;
    f.thumb({
        0x2005U,  // movs r0, #5
        0x2103U,  // movs r1, #3
        0x1a42U,  // subs r2, r0, r1
        0x1dc3U,  // adds r3, r0, #7
        0x0784U,  // lsls r4, r0, #30
        0x0845U,  // lsrs r5, r0, #1
        0x424eU,  // negs r6, r1
        0x4348U,  // muls r0, r1, r0
        0x2708U,  // movs r7, #8
        0x41fbU,  // rors r3, r7
        0x413cU,  // asrs r4, r7
        0x2903U,  // cmp r1, #3
        0xe7feU,  // b .
    });
    f.run();
    check(f.r(2) == 2 && f.r(5) == 2 && f.r(6) == 0xffff'fffdU, "Thumb : SUBS, LSRS, NEGS");
    check(f.r(0) == 15, "Thumb : MULS");
    check(f.r(3) == 0x0c00'0000U && f.r(4) == 0x0040'0000U, "Thumb : ROR et ASR par registre");
    check(f.cpu.regs.z && f.cpu.regs.c && !f.cpu.regs.n && !f.cpu.regs.v, "Thumb : CMP égal");
}

template <Architecture Arch>
void thumb_memory_test() {
    Fixture<Arch> f;
    f.bus.poke(0x201c, 0xcafe'babeU, 4);
    f.thumb({
        0x2080U,  // movs r0, #0x80
        0x0200U,  // lsls r0, r0, #8
        0x6841U,  // ldr r1, [r0, #4]
        0x8842U,  // ldrh r2, [r0, #2]
        0x2303U,  // movs r3, #3
        0x5ec4U,  // ldrsh r4, [r0, r3]
        0x56c5U,  // ldrsb r5, [r0, r3]
        0x7846U,  // ldrb r6, [r0, #1]
        0x8101U,  // strh r1, [r0, #8]
        0x60c1U,  // str r1, [r0, #12]
        0x7441U,  // strb r1, [r0, #17]
        0x4f01U,  // ldr r7, [pc, #4]
        0xe7feU,  // b .
    });
    f.run();
    check(f.r(1) == 0x0bad'f00dU && f.r(2) == 0xa493U && f.r(6) == 0x82, "Thumb : LDR, LDRH, LDRB");
    check(f.r(4) == (Fixture<Arch>::armv5 ? 0xffff'a493U : 0xffff'ffa4U) && f.r(5) == 0xffff'ffa4U,
          "Thumb : LDRSH impair selon l'architecture, LDRSB");
    check(f.word(0x8008) == 0xf00dU && f.word(0x800c) == 0x0bad'f00dU && f.word(0x8010) == 0x0d00U,
          "Thumb : STRH, STR, STRB");
    check(f.r(7) == 0xcafe'babeU, "Thumb : LDR relatif à PC, aligné sur 4");
}

template <Architecture Arch>
void thumb_stack_test() {
    Fixture<Arch> f;
    f.cpu.regs.r[14] = 0x4321;
    f.thumb({
        0x2090U,  // movs r0, #0x90
        0x0200U,  // lsls r0, r0, #8
        0x4685U,  // mov sp, r0
        0x2101U,  // movs r1, #1
        0x2202U,  // movs r2, #2
        0xb506U,  // push {r1, r2, lr}
        0xbc18U,  // pop {r3, r4}
        0xad01U,  // add r5, sp, #4
        0xb082U,  // sub sp, #8
        0xa601U,  // adr r6, .Ldata
        0xe7feU,  // b .
    });
    f.run();
    check(f.word(0x8ff4) == 1 && f.word(0x8ffc) == 0x4321, "Thumb : PUSH avec LR");
    check(f.r(3) == 1 && f.r(4) == 2 && f.r(5) == 0x9000, "Thumb : POP, ADD Rd, SP");
    check(f.r(13) == 0x8ff4 && f.r(6) == 0x2018, "Thumb : SUB SP et ADR");
}

template <Architecture Arch>
void thumb_multiple_test() {
    Fixture<Arch> f;
    f.thumb({
        0x2080U,  // movs r0, #0x80
        0x0200U,  // lsls r0, r0, #8
        0x2101U,  // movs r1, #1
        0xc003U,  // stmia r0!, {r0, r1}
        0x2281U,  // movs r2, #0x81
        0x0212U,  // lsls r2, r2, #8
        0xc206U,  // stmia r2!, {r1, r2}
        0x2381U,  // movs r3, #0x81
        0x021bU,  // lsls r3, r3, #8
        0xcb0aU,  // ldmia r3, {r1, r3}
        0x2481U,  // movs r4, #0x81
        0x0224U,  // lsls r4, r4, #8
        0xcc60U,  // ldmia r4!, {r5, r6}
        0xe7feU,  // b .
    });
    f.run();
    constexpr bool armv5 = Fixture<Arch>::armv5;
    check(f.word(data) == data && f.r(0) == 0x8008, "Thumb STMIA : base en tête, ancienne valeur");
    check(f.word(0x8104) == (armv5 ? 0x8100U : 0x8108U), "Thumb STMIA : base plus loin, selon l'architecture");
    check(f.r(3) == f.word(0x8104), "Thumb LDMIA : base dans la liste, jamais réécrite");
    check(f.r(5) == 1 && f.r(4) == 0x8108, "Thumb LDMIA : réécriture");
}

template <Architecture Arch>
void thumb_branch_test() {
    Fixture<Arch> f;
    f.thumb({
        0x2000U,          // movs r0, #0
        0xd000U,          // beq .Lskip
        0x2009U,          // movs r0, #9
                          // .Lskip:
        0xf000U, 0xf802U, // bl .Lsub
        0x2201U,          // movs r2, #1
        0xe001U,          // b .Lswitch
                          // .Lsub:
        0x4671U,          // mov r1, lr
        0x4770U,          // bx lr
                          // .Lswitch:
        0x4778U,          // bx pc
    });
    f.load_arm(0x2014, {0xeafffffeU});  // b .
    f.run();
    check(f.r(0) == 0, "Thumb : saut conditionnel pris");
    check(f.r(1) == 0x200b && f.r(2) == 1, "Thumb : BL, retour avec le bit Thumb");
    check(!f.cpu.regs.thumb && f.cpu.pc() == 0x2014, "Thumb : BX PC passe en ARM à l'adresse alignée");
}

template <Architecture Arch>
void thumb_pop_pc_test() {
    constexpr bool armv5 = Fixture<Arch>::armv5;
    Fixture<Arch> f;
    f.cpu.regs.r[13] = 0x9000;
    f.bus.poke(0x9000, 0x2100, 4);
    if (armv5) f.load_arm(0x2100, {0xeafffffeU});
    else f.load_thumb(0x2100, {0xe7feU});
    f.thumb({0xbd00U});  // pop {pc}
    f.run();
    check(f.cpu.pc() == 0x2100 && f.cpu.regs.thumb == !armv5 && f.r(13) == 0x9004,
          "POP {PC} : reste en Thumb sur l'ARM7, suit le bit 0 sur l'ARM9");
}

template <Architecture Arch>
void thumb_exception_test() {
    Fixture<Arch> hle;
    hle.bus.hle_swi = true;
    hle.thumb({0xdf12U, 0xe7feU});  // swi #0x12 ; b .
    hle.run();
    check(hle.bus.swis.size() == 1 && hle.bus.swis[0].comment == 0x12 && hle.bus.swis[0].thumb, "SWI Thumb pris par l'hôte");
    Fixture<Arch> real;
    real.thumb({0xdf12U});
    real.cpu.step();
    check(real.cpu.regs.mode == mode::supervisor && !real.cpu.regs.thumb && real.r(14) == thumb_code + 2 &&
              (real.cpu.regs.spsr() & psr::thumb) != 0,
          "SWI Thumb : ARM, LR = adresse + 2, T dans le SPSR");
}

template <Architecture Arch>
void thumb_exchange_link_test() {
    Fixture<Arch> f;
    f.load_arm(0x3000, {0xe3a0202aU, 0xeafffffeU});  // mov r2, #42 ; b .
    f.thumb({0xf000U, 0xeffeU});                     // blx 0x3000
    if constexpr (Fixture<Arch>::armv5) {
        f.run();
        check(f.r(2) == 42 && !f.cpu.regs.thumb && f.r(14) == thumb_code + 5, "Thumb BLX immédiat vers l'ARM");
    } else {
        f.cpu.step();
        f.cpu.step();
        check(f.cpu.regs.mode == mode::undefined, "Thumb BLX indéfini sur l'ARMv4");
    }
}

template <Architecture Arch>
void architecture_tests() {
    alu_test<Arch>();
    status_register_test<Arch>();
    multiply_test<Arch>();
    load_store_test<Arch>();
    swap_test<Arch>();
    block_transfer_test<Arch>();
    empty_list_test<Arch>();
    branch_test<Arch>();
    load_pc_test<Arch>(true);
    software_interrupt_test<Arch>();
    undefined_and_breakpoint_test<Arch>();
    coprocessor_test<Arch>();
    interrupt_test<Arch>();
    cycles_test<Arch>();
    thumb_alu_test<Arch>();
    thumb_memory_test<Arch>();
    thumb_stack_test<Arch>();
    thumb_multiple_test<Arch>();
    thumb_branch_test<Arch>();
    thumb_pop_pc_test<Arch>();
    thumb_exception_test<Arch>();
    thumb_exchange_link_test<Arch>();
}

} // namespace

int main() {
    architecture_tests<Architecture::armv4t>();
    architecture_tests<Architecture::armv5te>();
    dsp_test();
    count_leading_zeros_test();
    doubleword_test();
    user_bank_test();
    load_pc_test<Architecture::armv5te>(false);
    exchange_link_test();
    never_condition_test();
    return 0;
}
