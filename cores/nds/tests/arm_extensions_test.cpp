#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/program_fixture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t code = 0x02000000, data = 0x02004000;
constexpr std::int64_t lo = -2147483648LL, hi = 2147483647;
std::int64_t signed32(std::uint32_t v) { return v < 0x80000000U ? std::int64_t{v} : std::int64_t{v} - 0x100000000LL; }
std::int64_t half(std::uint32_t v, bool top) {
    const auto h = (v >> (top ? 16U : 0U)) & 65535U;
    return h < 32768 ? std::int64_t{h} : std::int64_t{h} - 65536;
}

struct Fixture {
    Memory memory;
    Cpu cpu;
    explicit Fixture(Processor processor = arm9) : cpu(processor), processor_(processor) {}
    void reset(std::uint32_t flags = 0xb0000000U) {
        cpu.reset(code,false); cpu.set_register(12,flags);
        memory.write32(processor_,code,0xe128f00c); // MSR CPSR_f,r12
        cpu.step(memory);
    }
    void run(std::uint32_t instruction) {
        memory.write32(processor_,cpu.state().registers[15],instruction); cpu.step(memory);
    }
    void flags(std::uint32_t expected = 0xb0000000U) {
        check((cpu.state().cpsr() & 0xf0000000U) == expected,"extension ARM modifie NZCV");
    }
private:
    Processor processor_;
};

void clz_and_prefetch() {
    Fixture f;
    for (unsigned bit = 0; bit < 33; ++bit) {
        f.reset(); f.cpu.set_register(0,bit == 32 ? 0 : 1U << bit);
        f.run(0xe16f1f10); // CLZ r1,r0
        check(f.cpu.state().registers[1] == (bit == 32 ? 32 : 31 - bit),"CLZ frontière de bit"); f.flags();
    }
    f.reset(); f.cpu.set_register(2,0x00017fff); f.run(0xe16f2f12);
    check(f.cpu.state().registers[2] == 15,"CLZ destination/source identiques");
    f.cpu.set_register(0,0x04100000); // PLD ne retire pas une donnée de FIFO IPC
    f.memory.write16(arm7,0x04000184,0x8000); f.memory.write16(arm9,0x04000184,0x8000);
    f.memory.write32(arm7,0x04000188,0xa55a);
    for (const auto raw : {0xf5d0f000U,0xf550f004U,0xf7d0f081U,0xf55ff004U}) f.run(raw);
    check(f.memory.peek32(arm9,0x04100000) == 0xa55a,"PLD lit la FIFO");
    f.cpu.set_register(0,0xdeadbeef); f.run(0xf5d0f000); f.flags();
    // Une indication visant une région interdite ne doit pas lever Data Abort.
    auto& cp = f.memory.cp15(); cp.write(6,0,0,code | 23U); cp.write(5,0,2,3); cp.write(5,0,3,3);
    cp.write(1,0,0,cp.read(1,0,0) | 1U); f.run(0xf5d0f000);
    check(f.cpu.state().mode == CpuMode::system,"PLD déclenche une faute MPU");
}

void saturation() {
    constexpr std::array<std::uint32_t,10> values{0,1,0xffffffff,0x7fffffff,0x80000000,
        0x40000000,0xc0000000,0x3fffffff,0xbfffffff,0x12345678};
    Fixture f;
    for (unsigned operation = 0; operation < 4; ++operation) for (auto a : values) for (auto b : values) {
        f.reset(); f.cpu.set_register(0,a); f.cpu.set_register(1,b);
        auto second = signed32(b) * (operation >= 2 ? 2 : 1);
        bool q = second < lo || second > hi;
        second = std::clamp(second,lo,hi);
        const auto result = (operation & 1U) ? signed32(a) - second : signed32(a) + second;
        q |= result < lo || result > hi;
        f.run(0xe1012050U | (operation << 21U)); // Q{D}{ADD/SUB} r2,r0,r1
        check(f.cpu.state().registers[2] == static_cast<std::uint32_t>(std::clamp(result,lo,hi)) &&
              f.cpu.state().sticky_overflow == q,"saturation simple/doublée ou drapeau Q"); f.flags();
    }
    // Le doublement déborde, mais l'addition finale rentre dans les limites.
    f.reset(); f.cpu.set_register(0,0x80000000); f.cpu.set_register(1,0x40000000);
    f.run(0xe1410050); // QDADD r0,r0,r1 -> -1, Q=1
    check(f.cpu.state().registers[0] == 0xffffffff && f.cpu.state().sticky_overflow,"doublement non saturé avant addition");
    f.cpu.set_register(0,1); f.cpu.set_register(1,2); f.run(0xe1011050); // QADD r1,r0,r1
    check(f.cpu.state().registers[1] == 3 && f.cpu.state().sticky_overflow,"Q non collant ou recouvrement source/destination");
    f.run(0xe10f3000); check(f.cpu.state().registers[3] & 0x08000000U,"MRS perd Q");
    f.cpu.set_register(4,0xb0000000); f.run(0xe128f004);
    check(!f.cpu.state().sticky_overflow,"MSR n'efface pas Q"); f.flags();
    f.cpu.reset(code,false); check(!f.cpu.state().sticky_overflow,"reset conserve Q");
}

void dsp_products() {
    Fixture f;
    constexpr std::array<std::uint32_t,8> values{0,1,0xffffffff,0x7fff8000,0x80007fff,0x80000000,0x7fffffff,0x1234fedc};
    for (unsigned operation = 0; operation < 4; ++operation) for (unsigned halves = 0; halves < 4; ++halves)
        for (auto a : values) for (auto b : values) {
            const bool x = (halves & 1U) != 0, y = (halves & 2U) != 0;
            const bool accumulate = operation != 3 && !(operation == 1 && x);
            const auto rn = accumulate ? 3U : 0U;
            f.reset(); f.cpu.set_register(0,a); f.cpu.set_register(1,b);
            f.cpu.set_register(2,0x7fffffff); f.cpu.set_register(3,0x7fffffff);
            const auto raw = 0xe1020180U | (operation << 21U) | (rn << 12U) | (halves << 5U);
            auto product = operation == 1 ? signed32(a) * half(b,y) : half(a,x) * half(b,y);
            // Division mathématique vers -inf, indépendamment du décalage utilisé par le cœur.
            if (operation == 1) product = product >= 0 ? product / 65536 : -((-product + 65535) / 65536);
            f.run(raw); f.flags();
            if (operation == 2) {
                const auto expected = 0x7fffffff7fffffffULL + static_cast<std::uint64_t>(product);
                check(f.cpu.state().registers[2] == static_cast<std::uint32_t>(expected >> 32U) &&
                      f.cpu.state().registers[3] == static_cast<std::uint32_t>(expected) && !f.cpu.state().sticky_overflow,
                      "SMLALxy signé/accumulation 64 bits");
            } else {
                const auto expected = product + (accumulate ? 0x7fffffffLL : 0);
                check(f.cpu.state().registers[2] == static_cast<std::uint32_t>(expected) &&
                      f.cpu.state().sticky_overflow == (accumulate && (expected < lo || expected > hi)),
                      "SMUL/SMLA signé, demi-mot sélectionné, arrondi ou Q");
            }
        }
    f.reset(); f.cpu.set_register(0,1); f.cpu.set_register(1,1);
    f.cpu.set_register(2,0xffffffff); f.cpu.set_register(3,0xffffffff);
    f.run(0xe1423180); // SMLALBB r3,r2,r0,r1 : bouclage 2^64
    check(f.cpu.state().registers[2] == 0 && f.cpu.state().registers[3] == 0 && !f.cpu.state().sticky_overflow,"SMLALxy débordement 64 bits");
    f.reset(0xb8000000); f.cpu.set_register(0,0xffff); f.cpu.set_register(1,0x8000);
    f.run(0xe1600180); // SMULBB r0,r0,r1
    check(f.cpu.state().registers[0] == 32768 && f.cpu.state().sticky_overflow,"SMULxy recouvrement ou effacement de Q");
    f.cpu.set_register(0,0xffffffff); f.cpu.set_register(1,1); f.run(0xe12001a0); // SMULWB r0,r0,r1
    check(f.cpu.state().registers[0] == 0xffffffff,"SMULW négatif arrondi vers zéro");
    f.cpu.set_register(0,0x8000); f.cpu.set_register(1,1); f.cpu.set_register(3,0x80000000);
    f.run(0xe1023180); // SMLABB r2,r0,r1,r3 : résultat modulo, pas saturation
    check(f.cpu.state().registers[2] == 0x7fff8000 && f.cpu.state().sticky_overflow,"SMLA saturé au lieu de boucler");
}

void doubleword_addressing() {
    Fixture f;
    for (bool load : {false,true}) for (bool immediate : {false,true})
        for (bool pre : {false,true}) for (bool up : {false,true}) for (bool wb : {false,true}) {
            if (!pre && wb) continue;
            f.reset(); f.cpu.set_register(0,data + 32); f.cpu.set_register(1,8);
            f.cpu.set_register(2,0x11223344); f.cpu.set_register(3,0x55667788);
            const auto target = pre ? (up ? data + 40 : data + 24) : data + 32;
            f.memory.write32(arm9,target,0xaabbccdd); f.memory.write32(arm9,target + 4,0xfeedface);
            const auto raw = 0xe00020d0U | (pre ? 0x01000000U : 0U) | (up ? 0x00800000U : 0U) |
                (immediate ? 0x00400000U : 0U) | (wb ? 0x00200000U : 0U) | (load ? 0U : 0x20U) | (immediate ? 8U : 1U);
            f.run(raw); f.flags();
            check(f.cpu.state().registers[0] == ((!pre || wb) ? (up ? data + 40 : data + 24) : data + 32),"writeback LDRD/STRD");
            if (load) check(f.cpu.state().registers[2] == 0xaabbccdd && f.cpu.state().registers[3] == 0xfeedface,"LDRD ordre des mots");
            else check(f.memory.read32(arm9,target) == 0x11223344 && f.memory.read32(arm9,target + 4) == 0x55667788,"STRD ordre des mots");
        }
    f.reset(); f.cpu.set_register(2,data + 4); // alignement 4 permis sur le bus NDS
    f.memory.write32(arm9,data + 4,0x1234); f.memory.write32(arm9,data + 8,0x5678);
    f.run(0xe1c220d0); // LDRD r2,[r2], sans writeback
    check(f.cpu.state().registers[2] == 0x1234 && f.cpu.state().registers[3] == 0x5678,"LDRD base/destination ou alignement 4");
    f.reset(); f.memory.write32(arm9,code + 12,0x87654321); f.memory.write32(arm9,code + 16,0xfedcba98);
    f.run(0xe1cf20d0); // LDRD r2,[pc] : PC architectural = instruction+8
    check(f.cpu.state().registers[2] == 0x87654321 && f.cpu.state().registers[3] == 0xfedcba98,"LDRD littéral PC+8");
    // DTCM : les transferts doubles passent par le même bus que LDM/STM.
    f.reset(); auto& cp = f.memory.cp15(); cp.write(9,1,0,data | 0xaU); cp.write(1,0,0,cp.read(1,0,0) | 0x10000U);
    f.cpu.set_register(0,data); f.cpu.set_register(2,0x12345678); f.cpu.set_register(3,0xabcdef01); f.run(0xe1c020f0);
    f.cpu.set_register(2,0); f.cpu.set_register(3,0); f.run(0xe1c020d0);
    check(f.cpu.state().registers[2] == 0x12345678 && f.cpu.state().registers[3] == 0xabcdef01 &&
          f.memory.read32(arm7,data) != 0x12345678,"LDRD/STRD ignore la DTCM");
    f.cpu.set_register(1,0x55667788); f.run(0xe1002091);
    check(f.cpu.state().registers[2] == 0x12345678 && f.memory.read32(arm9,data) == 0x55667788 &&
          f.memory.read32(arm7,data) != 0x55667788,"SWP ignore la DTCM");
}

void swap_and_failures() {
    for (auto processor : {arm9,arm7}) {
        Fixture f{processor};
        for (bool byte : {false,true}) {
            f.reset(); f.cpu.set_register(0,data); f.cpu.set_register(1,0x88776655);
            f.memory.write32(processor,data,0xaabbccdd); f.run(byte ? 0xe1402091 : 0xe1002091);
            check(f.cpu.state().registers[2] == (byte ? 0xddU : 0xaabbccddU) &&
                  f.memory.read32(processor,data) == (byte ? 0xaabbcc55U : 0x88776655U),"SWP/SWPB lecture ou écriture"); f.flags();
        }
        f.reset(); f.cpu.set_register(0,data + 1); f.cpu.set_register(1,0x12345678); f.run(0xe1401091);
        check(f.cpu.state().registers[1] == 0xcc && f.memory.read8(processor,data + 1) == 0x78,"SWPB recouvrement Rd/Rm ou adresse impaire");
        f.reset(); f.cpu.set_register(0,data); f.run(0xe1000090);
        check(f.memory.read32(processor,data) == data && f.cpu.state().registers[0] != data,"SWP lit ses sources avant Rd");
        for (auto raw : {0xe1002091U,0xe1402091U}) {
            f.reset(); f.cpu.set_register(0,0x04100000); f.cpu.set_register(1,0x1234);
            f.memory.write16(processor,0x04000184,0x8000); const auto other = processor == arm9 ? arm7 : arm9;
            f.memory.write16(other,0x04000184,0x8000); f.memory.write32(other,0x04000188,0x9876);
            const auto before = f.cpu.state();
            expect_failure<CpuExecutionError>([&] { f.run(raw); },"SWP vers FIFO non inscriptible accepté");
            check(f.cpu.state() == before && f.memory.peek32(processor,0x04100000) == 0x9876,"SWP refusé retire une donnée FIFO");
            static_cast<void>(f.memory.read32(processor,0x04100000));
        }
        f.reset(); f.cpu.set_register(0,data + 1); const auto before = f.cpu.state();
        expect_failure<CpuExecutionError>([&] { f.run(0xe1002091); },"SWP mot non aligné accepté dans le modèle strict");
        check(f.cpu.state() == before,"SWP refusé modifie le CPU");
    }
    Fixture f;
    for (auto raw : {0xe1c020d0U,0xe1c020f0U}) {
        f.reset(); f.cpu.set_register(0,0x04000304); f.cpu.set_register(2,0x8203); f.cpu.set_register(3,1);
        const auto before = f.cpu.state();
        const auto power = f.memory.read32(arm9,0x04000304);
        expect_failure<CpuExecutionError>([&] { f.run(raw); },"deuxième mot non mappé accepté");
        check(f.cpu.state() == before && f.memory.read32(arm9,0x04000304) == power,"transfert double partiel sur erreur");
    }
    // Une lecture double ne doit pas consommer la FIFO si le second mot est invalide.
    f.reset(); f.cpu.set_register(0,0x04100000);
    f.memory.write16(arm9,0x04000184,0x8000); f.memory.write16(arm7,0x04000184,0x8000);
    f.memory.write32(arm7,0x04000188,0xa55a);
    expect_failure<CpuExecutionError>([&] { f.run(0xe1c020d0); },"LDRD FIFO+port absent accepté");
    check(f.memory.peek32(arm9,0x04100000) == 0xa55a,"LDRD invalide consomme la FIFO");
    for (auto address : {data + 2,0xfffffffcU}) for (auto raw : {0xe1c020d0U,0xe1c020f0U}) {
        f.reset(); f.cpu.set_register(0,address); const auto before = f.cpu.state();
        expect_failure<CpuExecutionError>([&] { f.run(raw); },"paire non alignée/débordante acceptée");
        check(f.cpu.state() == before,"paire invalide modifie l'état");
    }
}

void protection() {
    for (auto raw : {0xe1c020d0U,0xe1c020f0U,0xe1002091U,0xe1402091U}) for (unsigned permission : {0U,6U}) {
        Fixture f; f.reset(); f.cpu.set_register(0,data - 4); f.cpu.set_register(1,0xdeadbeef);
        f.cpu.set_register(2,0x1234); f.cpu.set_register(3,0x5678);
        f.memory.write32(arm9,data - 4,0xaabbccdd); f.memory.write32(arm9,data,0x11223344);
        auto& cp = f.memory.cp15();
        cp.write(6,0,0,code | 29U); // région 32 Kio accessible
        cp.write(6,1,0,data | 23U); // région prioritaire 4 Kio, deuxième mot
        const bool swap = (raw & 0x0fb00ff0U) == 0x01000090U;
        if (swap) f.cpu.set_register(0,data);
        cp.write(5,0,2,3U | (permission << 4U)); cp.write(5,0,3,3);
        cp.write(1,0,0,cp.read(1,0,0) | 1U);
        const auto count = f.cpu.state().instructions; f.run(raw);
        if (raw == 0xe1c020d0U && permission == 6) {
            check(f.cpu.state().registers[2] == 0xaabbccdd && f.cpu.state().registers[3] == 0x11223344,"LDRD lecture seule refusée");
        } else {
            check(f.cpu.state().mode == CpuMode::abort && f.cpu.state().registers[15] == 0xffff0010 &&
                  f.cpu.state().registers[14] == code + 12 && f.cpu.state().instructions == count,"faute MPU extension sans Data Abort correct");
            check(f.cpu.state().registers[2] == 0x1234 && f.cpu.state().registers[3] == 0x5678 &&
                  f.memory.read32(arm9,data - 4) == 0xaabbccdd && f.memory.read32(arm9,data) == 0x11223344,
                  "MPU laisse un transfert double/atomique partiel");
        }
    }
}

void decode_and_conditions() {
    constexpr std::array<std::uint32_t,10> arm9_only{0xe16f2f10,0xe1012050,0xe1412050,0xe1023180,
        0xe1223180,0xe12201a0,0xe1423180,0xe1620180,0xe1c020d0,0xe1c020f0};
    Fixture a7{arm7};
    for (auto raw : arm9_only) {
        a7.reset(); const auto before = a7.cpu.state();
        expect_failure<CpuExecutionError>([&] { a7.run(raw); },"instruction ARMv5TE acceptée sur ARM7");
        check(a7.cpu.state() == before,"instruction ARM9 sur ARM7 modifie le CPU");
    }
    expect_failure<CpuExecutionError>([&] { a7.run(0xf5d0f000); },"PLD accepté sur ARM7");
    Fixture f;
    for (auto raw : arm9_only) {
        f.reset(); auto before = f.cpu.state(); f.run(raw & 0x0fffffffU); // EQ avec Z=0
        before.registers[15] += 4; ++before.instructions;
        check(f.cpu.state() == before,"condition fausse exécute une extension ou accède au bus");
    }
    for (const auto processor : {arm9,arm7}) {
        Fixture conditional{processor}; conditional.reset(); conditional.cpu.set_register(0,0xdeadbeef);
        auto before = conditional.cpu.state(); conditional.run(0x01002091); // SWPEQ, Z=0
        before.registers[15] += 4; ++before.instructions;
        check(conditional.cpu.state() == before,"SWP conditionnel accède à une adresse invalide");
    }
    for (auto raw : {0xe16fff10U,0xe16f2f1fU,0xe101f050U,0xe10f2050U,0xe101205fU,
         0xe10f3180U,0xe102f180U,0xe1023f80U,0xe102318fU,0xe1621180U,0xe12211a0U,0xe1422180U,
         0xe1c030d0U,0xe1c0e0f0U,0xe1e220d0U,0xe1e320f0U,0xe0e020d0U,0xe18021d1U,0xe18020ffU,
         0xe100f091U,0xe10f2091U,0xe100209fU,0xf7d0f011U}) {
        f.reset(); f.cpu.set_register(0,data); const auto before = f.cpu.state();
        expect_failure<CpuExecutionError>([&] { f.run(raw); },"encodage réservé/PC/writeback ambigu accepté");
        check(f.cpu.state() == before,"encodage invalide modifie l'état");
    }
}

void dual_cpu_lock() {
    const auto worker = [](std::uint32_t done) {
        Program p; p.load(0,data); p.load(1,data + 4); p.load(2,done); p.load(3,1); p.load(6,100);
        const auto loop = static_cast<unsigned>(p.code.size());
        const auto branch = [&] {
            const auto here = static_cast<unsigned>(p.code.size()); p.code.push_back(0x1a000000U | ((loop - here - 2U) & 0xffffffU));
        };
        p.code.insert(p.code.end(),{0xe1004093,0xe3540000}); branch(); // SWP verrou, attendre zéro
        p.code.insert(p.code.end(),{0xe5915000,0xe2855001,0xe5815000,0xe3a04000,0xe5804000,0xe2566001}); branch();
        p.code.push_back(0xe5823000); return p.finish();
    };
    Machine machine; machine.load_cartridge(Cartridge{cartridge_image(worker(data + 8),worker(data + 12))});
    machine.run_cycles(10000);
    check(machine.memory().read32(arm9,data) == 0 && machine.memory().read32(arm9,data + 4) == 200 &&
          machine.memory().read32(arm9,data + 8) == 1 && machine.memory().read32(arm9,data + 12) == 1,
          "homebrew ARM9/ARM7 perd des mises à jour malgré le verrou SWP");
}
} // namespace

int main() {
    clz_and_prefetch(); saturation(); dsp_products(); doubleword_addressing(); swap_and_failures();
    protection(); decode_and_conditions(); dual_cpu_lock();
}
