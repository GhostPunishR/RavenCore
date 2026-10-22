#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/program_fixture.hpp"

#include <array>
#include <bit>
#include <limits>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t divcnt = 0x04000280, numer = divcnt + 0x10, denom = divcnt + 0x18;
constexpr std::uint32_t quotient = divcnt + 0x20, remainder = divcnt + 0x28;
constexpr std::uint32_t sqrtcnt = divcnt + 0x30, root = divcnt + 0x34, param = divcnt + 0x38;
constexpr std::uint64_t all = std::numeric_limits<std::uint64_t>::max();

void write64(Memory& memory, std::uint32_t address, std::uint64_t value) {
    const std::array<std::uint32_t,2> words{static_cast<std::uint32_t>(value),static_cast<std::uint32_t>(value >> 32U)};
    memory.write_words(arm9,address,words);
}
std::uint64_t read64(const Memory& memory, std::uint32_t address) {
    return memory.peek32(arm9,address) | (std::uint64_t{memory.peek32(arm9,address + 4)} << 32U);
}
void divide(Memory& memory, unsigned mode, std::uint64_t n, std::uint64_t d) {
    memory.write16(arm9,divcnt,static_cast<std::uint16_t>(mode));
    write64(memory,numer,n); write64(memory,denom,d);
}

void division_vectors() {
    struct Vector { unsigned mode; std::uint64_t n, d, q, r; };
    const Vector vectors[]{
        {0,100,7,14,2}, {0,0xffffff9c,7,all - 13,all - 1},
        {0,100,0xfffffff9,all - 13,2}, {0,0xffffff9c,0xfffffff9,14,all - 1},
        {0,0x1234567800000064ULL,0xabcdef0000000007ULL,14,2},
        {0,0x80000000,0xffffffff,0x80000000,0},
        {0,100,0,0xffffffff,100}, {0,0,0,0xffffffff,0},
        {0,0xffffffff,0,0xffffffff00000001ULL,all},
        {0,0x80000000,0,0xffffffff00000001ULL,0xffffffff80000000ULL},
        {0,100,0x100000000ULL,0xffffffff,100}, // DIV0 arithmétique sans flag DIV0
        {1,0x100000000ULL,3,0x55555555,1}, {3,0x100000000ULL,3,0x55555555,1},
        {1,all - 99,7,all - 13,all - 1},
        {1,100,0xabcdef00fffffff9ULL,all - 13,2},
        {1,0x8000000000000000ULL,0xffffffff,0x8000000000000000ULL,0},
        {1,100,0,all,100}, {1,all - 99,0,1,all - 99},
        {3,all - 99,0x100000000ULL,1,all - 99},
        {2,0x100000000ULL,0x100000000ULL,1,0},
        {2,0x8000000000000000ULL,all,0x8000000000000000ULL,0},
        {2,0x8000000000000000ULL,0x8000000000000000ULL,1,0},
        {2,0x7fffffffffffffffULL,0x8000000000000000ULL,0,0x7fffffffffffffffULL},
        {2,0x8000000000000000ULL,3,0xd555555555555556ULL,all - 1},
        {2,0,0,all,0}, {2,0x8000000000000000ULL,0,1,0x8000000000000000ULL}
    };
    Memory memory;
    for (const auto& v : vectors) {
        const auto old_q = read64(memory,quotient), old_r = read64(memory,remainder);
        divide(memory,v.mode,v.n,v.d);
        check(read64(memory,numer) == v.n && read64(memory,denom) == v.d, "opérandes DIV tronqués");
        check(memory.read16(arm9,divcnt) == (v.mode | 0x8000U | (v.d == 0 ? 0x4000U : 0U)), "mode, busy ou DIV0");
        memory.advance_cycles(v.mode == 0 ? 17 : 33);
        check((memory.read16(arm9,divcnt) & 0x8000U) && read64(memory,quotient) == old_q && read64(memory,remainder) == old_r,
              "résultat DIV publié avant 18/34 cycles communs");
        memory.advance_cycles(1);
        check(!(memory.read16(arm9,divcnt) & 0x8000U) && read64(memory,quotient) == v.q && read64(memory,remainder) == v.r,
              "division signée, reste, zéro ou débordement incorrect");
    }
    check(memory.read32(arm9,0x04000214) == 0 && memory.read32(arm7,0x04000214) == 0, "IRQ DIV fictive");
}

void arithmetic_properties() {
    Memory memory;
    std::uint64_t random = 0x534e44534d415448ULL;
    const auto next = [&] { random ^= random << 13U; random ^= random >> 7U; random ^= random << 17U; return random; };
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned i = 0; i < 256; ++i) {
        const auto raw_n = next(), raw_d = next();
        const auto n = mode == 0 ? static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw_n))) :
            std::bit_cast<std::int64_t>(raw_n);
        const auto d = mode == 2 ? std::bit_cast<std::int64_t>(raw_d) :
            static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw_d)));
        if (d == 0 || (n == std::numeric_limits<std::int64_t>::min() && d == -1)) continue;
        divide(memory,mode,raw_n,raw_d); memory.advance_cycles(34);
        check(read64(memory,quotient) == static_cast<std::uint64_t>(n / d) &&
              read64(memory,remainder) == static_cast<std::uint64_t>(n % d), "oracle division signée C++");
    }
    for (unsigned mode = 0; mode < 2; ++mode) for (unsigned i = 0; i < 256; ++i) {
        const auto raw = next(); const auto n = mode ? raw : raw & 0xffffffffU;
        memory.write16(arm9,sqrtcnt,static_cast<std::uint16_t>(mode)); write64(memory,param,raw);
        memory.advance_cycles(13);
        const std::uint64_t result = memory.read32(arm9,root);
        check((!result || result <= n / result) && (result + 1) > n / (result + 1), "SQRT ne donne pas le plancher");
    }
}

void roots_parallel_and_restart() {
    Memory memory;
    struct Vector { unsigned mode; std::uint64_t n; std::uint32_t result; };
    const Vector vectors[]{{0,0,0},{0,1,1},{0,2,1},{0,15,3},{0,16,4},{0,17,4},
        {0,all,0xffff},{0,0x100000000ULL,0},{1,0x100000000ULL,0x10000},
        {1,0xfffffffe00000000ULL,0xfffffffe},{1,0xfffffffe00000001ULL,0xffffffff},
        {1,all,0xffffffff}};
    for (const auto& v : vectors) {
        const auto previous = memory.read32(arm9,root);
        memory.write32(arm9,sqrtcnt,v.mode | 0xfffffffeU); // bits réservés et busy ne s'écrivent pas
        write64(memory,param,v.n); memory.advance_cycles(12);
        check(memory.read32(arm9,sqrtcnt) == (0x8000U | v.mode) && memory.read32(arm9,root) == previous,
              "masque SQRTCNT ou délai SQRT");
        memory.advance_cycles(1);
        check(memory.read32(arm9,sqrtcnt) == v.mode && memory.read32(arm9,root) == v.result, "vecteur SQRT incorrect");
    }
    divide(memory,2,100,3); memory.advance_cycles(10);
    memory.write16(arm9,sqrtcnt,0); write64(memory,param,144); memory.advance_cycles(13);
    check(memory.read32(arm9,root) == 12 && (memory.read16(arm9,divcnt) & 0x8000U), "DIV et SQRT sérialisés");
    memory.advance_cycles(10); check(memory.read16(arm9,divcnt) & 0x8000U, "division raccourcie par SQRT");
    memory.advance_cycles(1); check(read64(memory,quotient) == 33, "division parallèle perdue");
    divide(memory,0,100,3); memory.advance_cycles(17);
    memory.write8(arm9,denom,7); memory.advance_cycles(17);
    check(read64(memory,quotient) == 33 && (memory.read16(arm9,divcnt) & 0x8000U), "écriture busy ne relance pas DIV");
    memory.advance_cycles(1); check(read64(memory,quotient) == 14, "nouvel opérande DIV ignoré");
    memory.write8(arm9,divcnt,2); memory.advance_cycles(18);
    check(memory.read16(arm9,divcnt) & 0x8000U, "changement de mode n'applique pas les 34 cycles");
    memory.advance_cycles(16);
    memory.write8(arm9,param,81); memory.advance_cycles(12);
    memory.write8(arm9,param,64); memory.advance_cycles(12);
    check(memory.read32(arm9,root) == 12, "ancien SQRT publie après relance");
    memory.advance_cycles(1); check(memory.read32(arm9,root) == 8, "SQRT relancé incorrect");
}

void access_and_reset() {
    Memory memory;
    check(memory.read32(arm9,divcnt) == 0 && memory.read32(arm9,sqrtcnt) == 0, "contrôles initiaux");
    for (unsigned i = 0; i < 8; ++i) memory.write8(arm9,numer + i,static_cast<std::uint8_t>(i + 1));
    check(read64(memory,numer) == 0x0807060504030201ULL, "voies octet NUMER");
    for (unsigned i = 0; i < 4; ++i) memory.write16(arm9,denom + i * 2,static_cast<std::uint16_t>(0x1234U + i));
    check(read64(memory,denom) == 0x1237123612351234ULL, "voies demi-mot DENOM");
    divide(memory,0,100,7); memory.advance_cycles(18);
    write64(memory,quotient,all); write64(memory,remainder,all); memory.write32(arm9,root,0xffffffff);
    memory.write16(arm9,divcnt + 2,0xffff); memory.write16(arm9,sqrtcnt + 2,0xffff);
    check(memory.read32(arm9,divcnt) == 0 && read64(memory,quotient) == 14 && read64(memory,remainder) == 2 &&
          memory.read32(arm9,sqrtcnt) == 0 && memory.read32(arm9,root) == 0, "résultats/réservés modifiables ou relance");
    memory.write8(arm9,divcnt + 1,0xff);
    check(memory.read16(arm9,divcnt) == 0x8000, "DIVCNT haut modifie les flags ou ne relance pas");
    memory.advance_cycles(18);
    for (const auto address : {divcnt,numer,denom,quotient,remainder,sqrtcnt,root,param}) {
        expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm7,address)); }, "DIV/SQRT accessible ARM7");
        expect_failure<ravenemu::Error>([&] { memory.write32(arm7,address,0); }, "écriture ARM7 acceptée");
    }
    expect_failure<ravenemu::Error>([&] { memory.write16(arm9,numer + 1,0); }, "opérande non aligné accepté");
    const std::array<std::uint32_t,2> invalid{3,0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm9,divcnt,invalid); }, "trou DIVCNT accepté");
    check(memory.read32(arm9,divcnt) == 0 && read64(memory,quotient) == 14, "STM refusé relance DIV");
    // La DTCM garde sa priorité sur les registres physiques ARM9.
    memory.cp15().write(9,1,0,0x0400000a); memory.cp15().write(1,0,0,0x12078);
    memory.write32(arm9,numer,123); memory.write32(arm9,divcnt,0x12345678);
    check(memory.read32(arm9,divcnt) == 0x12345678, "DIV masque la DTCM");
    memory.cp15().write(1,0,0,0x2078);
    check(memory.read32(arm9,divcnt) == 0 && read64(memory,numer) == 100, "écriture DTCM atteint DIV");
    divide(memory,0,100,9); write64(memory,param,81); memory.advance_cycles(7); auto copy = memory;
    memory.clear_ram(); copy.advance_cycles(11);
    check(read64(copy,quotient) == 11 && read64(copy,remainder) == 1 && copy.read32(arm9,root) == 9 &&
          memory.read32(arm9,divcnt) == 0 && memory.read32(arm9,sqrtcnt) == 0,
          "copie ne conserve pas le calcul ou reset partagé");
    memory.advance_cycles(all);
    check(read64(memory,quotient) == 0 && read64(memory,numer) == 0 && memory.read32(arm9,root) == 0 && read64(memory,param) == 0,
          "calcul différé après reset");
}

void executable_and_reload() {
    Program p; p.load(5,0x8000);
    const auto poll = [&] { p.code.insert(p.code.end(),{0xe5901000,0xe1110005,0x1afffffc}); };
    p.load(1,0xffffff9c); p.store(1,numer); p.load(1,7); p.store(1,denom);
    p.load(0,divcnt); poll();
    p.load(0,quotient); p.code.push_back(0xe8900006); // LDMIA r0,{r1,r2}
    p.store(1,0x02001000); p.store(2,0x02001004);
    p.load(0,remainder); p.code.push_back(0xe5903000); p.store(3,0x02001008);
    p.load(1,1); p.store(1,sqrtcnt); p.store(1,param + 4);
    p.load(0,sqrtcnt); poll(); p.load(0,root); p.code.push_back(0xe5903000); p.store(3,0x0200100c);
    const auto rom = cartridge_image(p.finish(),{0xe2800001,0xeafffffd});
    Machine machine; machine.load_cartridge(Cartridge{rom}); machine.run_cycles(200);
    check(machine.memory().read32(arm9,0x02001000) == 0xfffffff2 && machine.memory().read32(arm9,0x02001004) == 0xffffffff &&
          machine.memory().read32(arm9,0x02001008) == 0xfffffffe && machine.memory().read32(arm9,0x0200100c) == 0x10000 &&
          machine.cpu_state(arm7).registers[0] > 0, "programme ARM9 DIV/SQRT ou concurrence ARM7");
    divide(machine.memory(),2,100,6); machine.advance_cycles(5); auto copy = machine;
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { copy.load_cartridge(Cartridge{invalid}); }, "destination invalide acceptée");
    copy.advance_cycles(29); check(read64(copy.memory(),quotient) == 16 && read64(copy.memory(),remainder) == 4,
                                 "chargement refusé perd le calcul en cours");
    machine.load_cartridge(Cartridge{rom});
    check(machine.memory().read32(arm9,divcnt) == 0 && read64(machine.memory(),quotient) == 0 &&
          machine.memory().read32(arm9,root) == 0, "nouvelle cartouche conserve DIV/SQRT");
    machine.run_cycles(200); check(machine.memory().read32(arm9,0x0200100c) == 0x10000, "redémarrage DIV/SQRT");
    // WFI suspend le CPU, pas le diviseur autonome.
    Memory memory; memory.write32(arm9,0x02000000,0xee070f90);
    Cpu cpu{arm9}; cpu.reset(0x02000000,false); cpu.step(memory);
    check(cpu.state().waiting_for_interrupt, "WFI synthétique non exécuté");
    divide(memory,0,100,7); const auto sleeping = cpu.state();
    for (unsigned i = 0; i < 30; ++i) cpu.step(memory);
    check(memory.read16(arm9,divcnt) & 0x8000U, "pas CPU sans cycles avance DIV");
    memory.advance_cycles(18); cpu.step(memory);
    check(read64(memory,quotient) == 14 && cpu.state() == sleeping, "WFI arrête DIV ou DIV réveille le CPU");
    // Une extinction Powerman bloque aussi une opération arithmétique en cours.
    memory.write16(arm7,0x040001c0,0x8800); memory.write8(arm7,0x040001c2,0); memory.advance_cycles(64);
    memory.write16(arm7,0x040001c0,0x8000); memory.write8(arm7,0x040001c2,0x40); memory.advance_cycles(63);
    divide(memory,2,100,3); write64(memory,param,81); memory.advance_cycles(all);
    check(memory.powered_off() && read64(memory,quotient) == 14 && memory.read32(arm9,root) == 0 &&
          (memory.read16(arm9,divcnt) & 0x8000U) && (memory.read16(arm9,sqrtcnt) & 0x8000U),
          "DIV/SQRT avancent au-delà de l'extinction");
    memory.advance_cycles(1000); check(read64(memory,quotient) == 14, "calcul publié hors tension");
}
} // namespace

int main() {
    division_vectors(); arithmetic_properties(); roots_parallel_and_restart();
    access_and_reset(); executable_and_reload();
}
