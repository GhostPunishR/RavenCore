#include "machine/machine.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

// Batterie de cycles CPU : chaque cas exécute un court bloc d'instructions et
// compare ses cycles à la somme tirée des tables de GBATEK (« ARM CPU
// Instruction Cycle Times », « GBA Memory Map », « Waitstate Control »).
// Notations : S accès séquentiel, N non séquentiel, I cycle interne ; un
// accès coûte 1 cycle plus ses attentes. Sauf mention contraire, le code est
// en IWRAM (bus 32 bits, sans attente) et WAITCNT vaut 0 :
// - EWRAM : bus 16 bits, 2 attentes : 3 cycles par demi-mot, 6 par mot ;
// - ROM WS0 : 4 attentes hors séquence, 2 à la suite, par demi-mot :
//   N = 5 et S = 3 en 16 bits, N = 5 + 3 = 8 et S = 3 + 3 = 6 en 32 bits.
// Les opcodes viennent de llvm-mc (-triple=armv4t-none-eabi ou
// thumbv4t-none-eabi) ; l'assembleur figure à côté de chacun.

namespace {

using ravenemu::gba::i32;

constexpr std::uint32_t iwram_code = 0x03000100U;
constexpr std::uint32_t rom_code = 0x08000200U;
constexpr std::uint32_t iwram_data = 0x03000800U;  // r4
constexpr std::uint32_t ewram_data = 0x02000000U;  // r5
constexpr std::uint32_t rom_data = 0x08001000U;    // r6
constexpr std::uint32_t timer0 = 0x04000100U;      // r7
constexpr std::uint32_t waitcnt = 0x04000204U;

constexpr std::uint32_t arm_nop = 0xe1a00000U;    // mov r0, r0
constexpr std::uint32_t arm_loop = 0xeafffffeU;   // b .
constexpr std::uint32_t thumb_nop = 0x46c0U;      // mov r8, r8
constexpr std::uint32_t thumb_loop = 0xe7feU;     // b .

struct Program {
    bool thumb{};
    std::vector<std::uint32_t> code;  // mots ARM ou demi-mots Thumb
    std::uint32_t at{iwram_code};     // adresse du bloc, en IWRAM ou en ROM
    int wait_control{};
    std::vector<std::pair<int, std::uint32_t>> registers{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> memory{};  // mots en IWRAM
};

std::unique_ptr<ravenemu::gba::Machine> make_machine(const Program& program, std::uint32_t start,
                                                    const std::vector<std::uint32_t>& words) {
    const auto width = program.thumb ? 2U : 4U;
    const auto in_rom = (start >> 24U) == 0x08U;
    auto size = std::size_t{0x2000};
    if (in_rom) size = std::max(size, std::size_t{start - 0x08000000U + words.size() * width + 0x100U});
    std::vector<std::uint8_t> rom(size);
    rom[0xb2] = 0x96;
    if (in_rom) {
        auto at = std::size_t{start - 0x08000000U};
        for (const auto word : words)
            for (auto byte = 0U; byte < width; ++byte) rom[at++] = static_cast<std::uint8_t>(word >> (8U * byte));
    }
    auto owner = std::make_unique<ravenemu::gba::Machine>(
        std::make_shared<const std::vector<std::uint8_t>>(std::move(rom)),
        ravenemu::GbaSaveType::none, false, [] { return std::int64_t{0}; });
    auto& machine = *owner;
    machine.bus.write16(i32(waitcnt), program.wait_control);
    if (!in_rom) {
        auto at = start;
        for (const auto word : words) {
            if (program.thumb) machine.bus.write16(i32(at), static_cast<int>(word));
            else machine.bus.write32(i32(at), i32(word));
            at += width;
        }
    }
    for (const auto& [address, value] : program.memory) machine.bus.write32(i32(address), i32(value));
    machine.cpu.reset(i32(start));
    machine.cpu.state.thumb = program.thumb;
    auto& regs = machine.cpu.state.regs;
    regs[4] = i32(iwram_data);
    regs[5] = i32(ewram_data);
    regs[6] = i32(rom_data);
    regs[7] = i32(timer0);
    for (const auto& [index, value] : program.registers) regs[static_cast<std::size_t>(index)] = i32(value);
    machine.bus.take_wait_cycles();
    return owner;
}

// Deux NOP remplissent le pipeline avant le bloc, une boucle le suit. Un pas
// lit l'instruction à PC + 2 avant d'agir et le rechargement après un saut
// compte dans l'instruction visée : les pas du bloc somment ses cycles GBATEK.
int cycles(const Program& program) {
    const auto width = program.thumb ? 2U : 4U;
    const auto nop = program.thumb ? thumb_nop : arm_nop;
    const auto start = program.at - 2U * width;
    std::vector<std::uint32_t> words{nop, nop};
    words.insert(words.end(), program.code.begin(), program.code.end());
    words.push_back(program.thumb ? thumb_loop : arm_loop);
    auto owner = make_machine(program, start, words);
    auto& machine = *owner;
    machine.cpu.step();
    machine.cpu.step();
    const auto end = i32(program.at + static_cast<std::uint32_t>(program.code.size()) * width);
    auto total = 0;
    for (auto steps = 0; machine.cpu.state.regs[15] != end; ++steps) {
        if (steps == 64) return -1;
        total += machine.cpu.step();
    }
    return total;
}

struct Case {
    const char* name;
    Program program;
    int expected;
};

std::vector<Case> cases() {
    const auto arm = [](std::vector<std::uint32_t> code, std::vector<std::pair<int, std::uint32_t>> registers = {}) {
        return Program{false, std::move(code), iwram_code, 0, std::move(registers)};
    };
    const auto thumb = [](std::vector<std::uint32_t> code, std::vector<std::pair<int, std::uint32_t>> registers = {}) {
        return Program{true, std::move(code), iwram_code, 0, std::move(registers)};
    };
    const auto in_rom = [](Program program, std::uint32_t at, int wait_control) {
        program.at = at;
        program.wait_control = wait_control;
        return program;
    };
    const auto with_memory = [](Program program, std::uint32_t address, std::uint32_t value) {
        program.memory.emplace_back(address, value);
        return program;
    };
    const auto with_wait = [](Program program, int wait_control) {
        program.wait_control = wait_control;
        return program;
    };
    // umull r0, r1, r2, r3 et consorts : le multiplicateur est r3. mul et mla : r2.
    constexpr std::uint32_t mul = 0xe0000291U;    // mul r0, r1, r2
    constexpr std::uint32_t umull = 0xe0810392U;  // umull r0, r1, r2, r3
    constexpr std::uint32_t arm_mov = 0xe1a00001U;  // mov r0, r1
    return {
        // --- ARM, données et ALU : 1S, +1I pour un décalage par registre.
        {"ARM mov x4 : 4 x 1S", arm({arm_mov, arm_mov, arm_mov, arm_mov}), 4},
        {"ARM add r0, r1, r2, lsl r3 : 1S + 1I", arm({0xe0810312U}), 2},
        // --- Multiplications : m = 1 à 4 selon les octets hauts du
        // multiplicateur (tous à 0, ou tous à 1 hors UMULL/UMLAL).
        {"ARM mul, Rs = 0x12 : 1S + 1I", arm({mul}, {{2, 0x12U}}), 2},
        {"ARM mul, Rs = 0x1234 : 1S + 2I", arm({mul}, {{2, 0x1234U}}), 3},
        {"ARM mul, Rs = 0x123456 : 1S + 3I", arm({mul}, {{2, 0x123456U}}), 4},
        {"ARM mul, Rs = 0x12345678 : 1S + 4I", arm({mul}, {{2, 0x12345678U}}), 5},
        {"ARM mul, Rs = 0xffffff80 : 1S + 1I", arm({mul}, {{2, 0xffffff80U}}), 2},
        {"ARM mla r0, r1, r2, r3, m = 1 : 1S + 2I", arm({0xe0203291U}, {{2, 0x12U}}), 3},
        {"ARM umull, Rs = 0x12 : 1S + 2I", arm({umull}, {{3, 0x12U}}), 3},
        {"ARM umull, Rs = 0xffffff80 : 1S + 5I", arm({umull}, {{3, 0xffffff80U}}), 6},
        {"ARM smull, Rs = 0xffffff80 : 1S + 2I", arm({0xe0c10392U}, {{3, 0xffffff80U}}), 3},
        {"ARM umlal, Rs = 0x12 : 1S + 3I", arm({0xe0a10392U}, {{3, 0x12U}}), 4},
        // --- Accès mémoire : LDR 1S + 1N + 1I, STR 2N, LDM nS + 1N + 1I,
        // STM (n-1)S + 2N, SWP 1S + 2N + 1I.
        {"ARM ldr r0, [r4] (IWRAM) : 1 + 1 + 1", arm({0xe5940000U}), 3},
        {"ARM ldr r0, [r5] (EWRAM) : 1 + 6 + 1", arm({0xe5950000U}), 8},
        {"ARM ldrh r0, [r5] (EWRAM) : 1 + 3 + 1", arm({0xe1d500b0U}), 5},
        {"ARM str r0, [r5] (EWRAM) : 1 + 6", arm({0xe5850000U}), 7},
        {"ARM stmia r5, {r0-r3} (EWRAM) : 1 + 4 x 6", arm({0xe885000fU}), 25},
        {"ARM ldmia r5, {r0-r3} (EWRAM) : 1 + 4 x 6 + 1", arm({0xe895000fU}), 26},
        {"ARM swp r0, r1, [r5] (EWRAM) : 1 + 6 + 6 + 1", arm({0xe1050091U}), 14},
        {"ARM ldr r0, [r6] (ROM) : 1 + 8 + 1", arm({0xe5960000U}), 10},
        {"ARM ldrh r0, [r6] (ROM) : 1 + 5 + 1", arm({0xe1d600b0U}), 7},
        // WAITCNT 0x4317 : WS0 à 3 attentes hors séquence, 1 à la suite :
        // N = 4 et S = 2 par demi-mot, 4 + 2 = 6 pour un mot.
        {"ARM ldr r0, [r6] (ROM, WAITCNT 0x4317) : 1 + 6 + 1", with_wait(arm({0xe5960000U}), 0x4317), 8},
        // --- Sauts : 2S + 1N, puis 1S pour l'instruction visée.
        {"ARM b, puis mov : 2S + 1N + 1S", arm({0xea000000U, arm_mov, arm_mov}), 4},       // b 1f ; mov ; 1: mov
        {"ARM bl, puis mov : 2S + 1N + 1S", arm({0xeb000000U, arm_mov, arm_mov}), 4},      // bl 1f ; mov ; 1: mov
        {"ARM add pc, pc, #0, puis mov : 1S + (1S + 1N) + 1S", arm({0xe28ff000U, arm_mov, arm_mov}), 4},
        // ldr pc, [r4] : 1S + 1N + 1I + (1S + 1N), puis 1S. [r4] vise le second mov.
        {"ARM ldr pc, [r4], puis mov : 5 + 1S",
         with_memory(arm({0xe594f000U, arm_mov, arm_mov}), iwram_data, iwram_code + 8U), 6},
        // --- Thumb en IWRAM.
        {"Thumb nop x4 : 4 x 1S", thumb({thumb_nop, thumb_nop, thumb_nop, thumb_nop}), 4},
        {"Thumb lsls r0, r1 : 1S + 1I", thumb({0x4088U}), 2},
        // muls r0, r1, r0 : m dépend de Rd (GBATEK, THUMB.4).
        {"Thumb muls, Rd = 0x1234 : 1S + 2I", thumb({0x4348U}, {{0, 0x1234U}}), 3},
        {"Thumb ldr r0, [r4] : 1 + 1 + 1", thumb({0x6820U}), 3},
        {"Thumb ldr r0, [pc, #0] : 1 + 1 + 1", thumb({0x4800U}), 3},
        {"Thumb str r0, [r5] (EWRAM) : 1 + 6", thumb({0x6028U}), 7},
        {"Thumb push {r0-r3} : 1 + 4 x 1", thumb({0xb40fU}), 5},
        {"Thumb pop {r0-r3} : 1 + 4 x 1 + 1", thumb({0xbc0fU}), 6},
        {"Thumb b, puis nop : 2S + 1N + 1S", thumb({0xe000U, thumb_nop, thumb_nop}), 4},  // b 1f ; nop ; 1: nop
        // bl : 1S pour la première moitié, 2S + 1N pour la seconde.
        {"Thumb bl, puis nop : 3S + 1N + 1S", thumb({0xf000U, 0xf801U, thumb_nop, thumb_nop}), 5},
        // --- Code en ROM, préchargement coupé : chaque lecture d'instruction
        // paie ses attentes.
        {"Thumb nop x4 en ROM : 4 x 3", in_rom(thumb({thumb_nop, thumb_nop, thumb_nop, thumb_nop}), rom_code, 0), 12},
        {"ARM mov x4 en ROM : 4 x 6", in_rom(arm({arm_mov, arm_mov, arm_mov, arm_mov}), rom_code, 0), 24},
        // Après un cycle interne, la lecture suivante redevient non
        // séquentielle (GBATEK, « Prefetch Disable Bug ») : 3 + 1, puis 5.
        {"Thumb lsls, puis nop en ROM : 3 + 1 + 5", in_rom(thumb({0x4088U, thumb_nop}), rom_code, 0), 9},
        // Préchargement actif : l'unité lit pendant le cycle interne, il
        // reste 2 des 3 cycles du demi-mot suivant.
        {"Thumb lsls, puis nop en ROM préchargée : 3 + 1 + 2",
         in_rom(thumb({0x4088U, thumb_nop}), rom_code, 0x4000), 6},
        // Le nop à 0x0801fffc lit 0x08020000 : une frontière de bloc de
        // 128 Kio rend cette lecture non séquentielle (5 au lieu de 3).
        {"Thumb nop x4 sur la frontière des 128 Kio : 3 + 3 + 5 + 3",
         in_rom(thumb({thumb_nop, thumb_nop, thumb_nop, thumb_nop}), 0x0801fffcU, 0), 14},
    };
}

// Le timer 0, pré-diviseur 1, lu avant et après quatre mov en IWRAM : d'une
// lecture à l'autre, le cycle interne du premier ldrh (1), les quatre mov (4),
// la lecture du second ldrh (1) et son accès aux E/S (1).
int timer_delta() {
    Program program{false, {
        0xe1d720b0U,                                       // ldrh r2, [r7]
        arm_nop, arm_nop, arm_nop, arm_nop,
        0xe1d730b0U,                                       // ldrh r3, [r7]
    }};
    const auto start = program.at - 8U;
    std::vector<std::uint32_t> words{arm_nop, arm_nop};
    words.insert(words.end(), program.code.begin(), program.code.end());
    words.push_back(arm_loop);
    auto owner = make_machine(program, start, words);
    auto& machine = *owner;
    machine.bus.write16(i32(timer0 + 2U), 0x80);  // actif, pré-diviseur 1
    const auto end = i32(program.at + 4U * static_cast<std::uint32_t>(program.code.size()));
    for (auto steps = 0; machine.cpu.state.regs[15] != end; ++steps) {
        if (steps == 64) return -1;
        machine.run_for(1);
    }
    const auto& regs = machine.cpu.state.regs;
    return static_cast<int>((static_cast<std::uint32_t>(regs[3]) - static_cast<std::uint32_t>(regs[2])) & 0xffffU);
}

} // namespace

int main() {
    auto failures = 0;
    const auto report = [&](const char* name, int got, int expected) {
        if (got == expected) return;
        std::fprintf(stderr, "%s : %d cycles, attendu %d\n", name, got, expected);
        ++failures;
    };
    for (const auto& test : cases()) report(test.name, cycles(test.program), test.expected);
    report("Timer 0 entre deux lectures, quatre mov entre elles", timer_delta(), 7);
    return failures == 0 ? 0 : 1;
}
