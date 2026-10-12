// Exécute une ROM de test GBA fournie séparément et rend un verdict.
//
// Protocole visé : celui des suites publiques qui terminent dans une boucle
// `b .` après avoir rangé dans un registre le numéro du premier test échoué
// (0 si tout passe). C'est le cas de jsmolka/gba-tests : r12, ou r7 pour la
// ROM Thumb. D'autres suites signalent un échec en mémoire (FuzzARM écrit
// 'AAAA' ou 'TTTT' au début de l'EWRAM) : --expect-word le vérifie à la fin.
// Aucune ROM n'est fournie ni téléchargée par ce programme.

#include "machine/machine.hpp"

#include <ravenemu/sha256.hpp>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ravenemu::gba::conformance {

struct WordExpectation {
    std::int32_t address{};
    std::uint32_t value{};
};

struct Options {
    std::string rom_path;
    std::vector<WordExpectation> words;
    std::optional<GbaSaveType> save_type;
    std::string framebuffer_sha256;
    std::string rom_sha256;
    int result_register{12};  // -1 : pas de registre de résultat
    int timeout_frames{600};
    int idle_frames{8};
    bool self_test{};
};

enum class Outcome { passed, failed, timeout };

struct Report {
    Outcome outcome{Outcome::timeout};
    int frames{};
    std::int32_t result{};
    std::optional<WordExpectation> mismatched_word;
    std::int32_t program_counter{};
    bool thumb{};
    std::string framebuffer_sha256;
    int unsupported_swi{};
    int undefined_instructions{};
    int unsupported_accesses{};
};

// Lecture sans effet de bord (ni attente ni diagnostic) des zones où une ROM
// de test peut boucler : ROM, IWRAM, EWRAM et VRAM.
std::optional<std::uint32_t> peek(Machine& machine, std::int32_t address, int width) {
    std::uint32_t value = 0;
    for (int index = 0; index < width; ++index) {
        const auto at = static_cast<std::uint32_t>(address) + static_cast<std::uint32_t>(index);
        const auto region = at >> 24U;
        int byte{};
        if (region >= 0x08U && region <= 0x0dU) byte = machine.cartridge.read8(static_cast<int>(at & 0x01ff'ffffU));
        else if (region == 0x03U) byte = machine.bus.iwram[at & 0x7fffU];
        else if (region == 0x02U) byte = machine.bus.ewram[at & 0x3ffffU];
        else if (region == 0x06U) {
            auto offset = at & 0x1ffffU;
            if (offset >= 0x18000U) offset -= 0x8000U;
            byte = machine.bus.vram[offset];
        }
        else return std::nullopt;
        value |= static_cast<std::uint32_t>(byte & 0xff) << (8U * static_cast<unsigned>(index));
    }
    return value;
}

// Vrai si la prochaine instruction est un branchement sur elle-même.
bool in_idle_loop(Machine& machine) {
    const auto pc = machine.cpu.state.regs[15];
    if (machine.cpu.state.thumb) return peek(machine, pc, 2) == 0xe7feU;
    const auto word = peek(machine, pc, 4);
    return word && (*word & 0x0fff'ffffU) == 0x0aff'fffeU;
}

std::string hex_sha256(std::span<const std::uint8_t> bytes) {
    const auto digest = detail::sha256(bytes);
    std::ostringstream out;
    for (const auto byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return out.str();
}

std::string frame_sha256(const Machine& machine) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(machine.ppu.frame.size() * 4);
    for (const auto pixel : machine.ppu.frame) {
        const auto value = static_cast<std::uint32_t>(pixel);
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    return hex_sha256(bytes);
}

Report run(const std::vector<std::uint8_t>& rom, const Options& options) {
    auto image = std::make_shared<const std::vector<std::uint8_t>>(rom);
    Machine machine(image, options.save_type, std::nullopt, [] { return std::int64_t{0}; });
    Report report;
    int idle_streak = 0;
    for (report.frames = 1; report.frames <= options.timeout_frames; ++report.frames) {
        machine.run_frame();
        idle_streak = in_idle_loop(machine) ? idle_streak + 1 : 0;
        if (idle_streak >= options.idle_frames) break;
    }
    report.frames = std::min(report.frames, options.timeout_frames);
    report.program_counter = machine.cpu.state.regs[15];
    report.thumb = machine.cpu.state.thumb;
    if (options.result_register >= 0) report.result = machine.cpu.state.regs[static_cast<std::size_t>(options.result_register)];
    for (const auto& expected : options.words) {
        const auto actual = peek(machine, expected.address, 4);
        if (actual != expected.value) { report.mismatched_word = WordExpectation{expected.address, actual.value_or(0)}; break; }
    }
    report.framebuffer_sha256 = frame_sha256(machine);
    report.unsupported_swi = machine.bus.diagnostics.count(DiagnosticEvent::unsupported_swi);
    report.undefined_instructions = machine.bus.diagnostics.count(DiagnosticEvent::undefined_instruction);
    report.unsupported_accesses = machine.bus.diagnostics.count(DiagnosticEvent::unsupported_access);
    // Une attente mémoire déçue est un échec même sans boucle finale : une
    // suite peut attendre une touche après avoir signalé son échec.
    if (report.mismatched_word) report.outcome = Outcome::failed;
    else if (idle_streak < options.idle_frames) report.outcome = Outcome::timeout;
    else if (report.result != 0) report.outcome = Outcome::failed;
    else if (!options.framebuffer_sha256.empty() && options.framebuffer_sha256 != report.framebuffer_sha256)
        report.outcome = Outcome::failed;
    else report.outcome = Outcome::passed;
    return report;
}

void print(const Report& report, const Options& options) {
    switch (report.outcome) {
    case Outcome::passed: std::cout << "PASS"; break;
    case Outcome::failed:
        if (report.mismatched_word) {
            std::cout << "FAIL mot 0x" << std::hex << static_cast<std::uint32_t>(report.mismatched_word->address)
                      << "=0x" << report.mismatched_word->value << std::dec;
        } else if (report.result != 0) std::cout << "FAIL test " << report.result;
        else std::cout << "FAIL image " << report.framebuffer_sha256;
        break;
    case Outcome::timeout: std::cout << "TIMEOUT"; break;
    }
    std::cout << " frames=" << report.frames;
    if (options.result_register >= 0) std::cout << " r" << options.result_register << '=' << report.result;
    std::cout << " pc=0x" << std::hex << static_cast<std::uint32_t>(report.program_counter) << std::dec
              << (report.thumb ? " thumb" : " arm") << " swi_inconnus=" << report.unsupported_swi
              << " indefinies=" << report.undefined_instructions
              << " acces_inconnus=" << report.unsupported_accesses << '\n';
}

// ROM minimale : range `value` dans r12 puis boucle sur `b .`.
std::vector<std::uint8_t> synthetic_rom(std::uint8_t value) {
    std::vector<std::uint8_t> rom(0x200);
    const auto put = [&](std::size_t at, std::uint32_t word) {
        for (unsigned index = 0; index < 4; ++index) rom[at + index] = static_cast<std::uint8_t>(word >> (8U * index));
    };
    put(0x00, 0xea00'002eU);          // b 0xc0
    rom[0xb2] = 0x96;
    put(0xc0, 0xe3a0'c000U | value);  // mov r12, #value
    put(0xc4, 0xeaff'fffeU);          // b .
    return rom;
}

int self_test() {
    Options options;
    options.timeout_frames = 30;
    const auto passed = run(synthetic_rom(0), options);
    const auto failed = run(synthetic_rom(42), options);
    auto stuck = synthetic_rom(0);
    stuck[0xc4] = 0x00; stuck[0xc5] = 0x00; stuck[0xc6] = 0xa0; stuck[0xc7] = 0xe1;  // mov r0, r0
    const auto timeout = run(stuck, options);
    if (passed.outcome != Outcome::passed) { std::cerr << "auto-test : ROM réussie non reconnue\n"; return 1; }
    if (failed.outcome != Outcome::failed || failed.result != 42) {
        std::cerr << "auto-test : échec du test 42 non reconnu\n"; return 1;
    }
    if (timeout.outcome != Outcome::timeout) { std::cerr << "auto-test : délai dépassé non reconnu\n"; return 1; }
    options.framebuffer_sha256 = std::string(64, '0');
    if (run(synthetic_rom(0), options).outcome != Outcome::failed) {
        std::cerr << "auto-test : image inattendue non reconnue\n"; return 1;
    }
    options.framebuffer_sha256.clear();
    options.result_register = -1;
    options.words = {{i32(0x0300'0000U), 0}};
    if (run(synthetic_rom(42), options).outcome != Outcome::passed) {
        std::cerr << "auto-test : registre de résultat non désactivé\n"; return 1;
    }
    options.words = {{i32(0x0800'00c0U), 0}};
    if (run(synthetic_rom(0), options).outcome != Outcome::failed) {
        std::cerr << "auto-test : mot inattendu non reconnu\n"; return 1;
    }
    std::cout << "auto-test GBA : OK\n";
    return 0;
}

std::optional<GbaSaveType> parse_save_type(std::string_view text) {
    if (text == "none") return GbaSaveType::none;
    if (text == "sram") return GbaSaveType::sram;
    if (text == "flash64") return GbaSaveType::flash_64k;
    if (text == "flash128") return GbaSaveType::flash_128k;
    if (text == "eeprom512") return GbaSaveType::eeprom_512;
    if (text == "eeprom8k") return GbaSaveType::eeprom_8k;
    throw std::invalid_argument("type de sauvegarde inconnu : " + std::string(text));
}

std::uint32_t parse_hex(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("valeur hexadécimale invalide : " + std::string(text));
    return value;
}

WordExpectation parse_word(std::string_view text) {
    const auto equals = text.find('=');
    if (equals == std::string_view::npos) throw std::invalid_argument("attendu ADRESSE=VALEUR : " + std::string(text));
    return {i32(parse_hex(text.substr(0, equals))), parse_hex(text.substr(equals + 1))};
}

int parse_int(std::string_view text, int minimum, int maximum) {
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("nombre invalide : " + std::string(text));
    return value;
}

void usage() {
    std::cerr << "Usage : gba_conformance_runner ROM [--result-register N|none] [--timeout-frames N]\n"
                 "        [--expect-word ADRESSE=VALEUR]... [--rom-sha256 HEX]\n"
                 "        [--idle-frames N] [--save-type none|sram|flash64|flash128|eeprom512|eeprom8k]\n"
                 "        [--expect-frame-sha256 HEX]\n"
                 "        gba_conformance_runner --self-test\n";
}

} // namespace ravenemu::gba::conformance

int main(int argc, char** argv) {
    using namespace ravenemu::gba::conformance;
    try {
        Options options;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            const auto value = [&]() -> std::string_view {
                if (index + 1 >= argc) throw std::invalid_argument("valeur manquante après " + std::string(argument));
                return argv[++index];
            };
            if (argument == "--self-test") options.self_test = true;
            else if (argument == "--result-register") {
                const auto text = value();
                options.result_register = text == "none" ? -1 : parse_int(text, 0, 14);
            } else if (argument == "--expect-word") options.words.push_back(parse_word(value()));
            else if (argument == "--timeout-frames") options.timeout_frames = parse_int(value(), 1, 1'000'000);
            else if (argument == "--idle-frames") options.idle_frames = parse_int(value(), 1, 10'000);
            else if (argument == "--save-type") options.save_type = parse_save_type(value());
            else if (argument == "--expect-frame-sha256") options.framebuffer_sha256 = std::string(value());
            else if (argument == "--rom-sha256") options.rom_sha256 = std::string(value());
            else if (!argument.empty() && argument.front() != '-' && options.rom_path.empty()) options.rom_path = argument;
            else { usage(); return 2; }
        }
        if (options.self_test) return self_test();
        if (options.rom_path.empty()) { usage(); return 2; }
        std::ifstream file(options.rom_path, std::ios::binary);
        if (!file) { std::cerr << "ROM illisible : " << options.rom_path << '\n'; return 2; }
        const std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>(file), {}};
        // Une autre version de la ROM rendrait le verdict trompeur : refus net.
        if (!options.rom_sha256.empty() && hex_sha256(rom) != options.rom_sha256) {
            std::cerr << "ROM inattendue (SHA-256 " << hex_sha256(rom) << ") : " << options.rom_path << '\n';
            return 2;
        }
        const auto report = run(rom, options);
        print(report, options);
        return report.outcome == Outcome::passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Erreur : " << error.what() << '\n';
        return 2;
    }
}
