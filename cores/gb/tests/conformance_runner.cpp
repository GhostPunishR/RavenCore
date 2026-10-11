#include "machine/machine.hpp"

#include <ravenemu/sha256.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ravenemu::cgb::conformance {

struct MemoryExpectation {
    int address{};
    int value{};
};

struct Options {
    std::string rom_path;
    std::string boot_rom_path;
    std::string hardware{"auto"};
    std::string category{"unspecified"};
    std::string serial_pass{"Passed"};
    std::string serial_fail{"Failed"};
    std::string framebuffer_sha256;
    std::string framebuffer_path;
    std::vector<MemoryExpectation> memory;
    int timeout_frames{600};
    int minimum_pass_frames{};
    int memory_stability_samples{1};
    bool mooneye_signature{true};
    bool memory_require_mismatch{};
    bool self_test{};
};

class SerialCapture final : public LinkEndpoint {
public:
    [[nodiscard]] bool attach(LinkPort& port) noexcept override {
        if (port_ != nullptr && port_ != &port) return false;
        port_ = &port;
        return true;
    }
    void detach(LinkPort& port) noexcept override {
        if (port_ == &port) port_ = nullptr;
    }
    [[nodiscard]] bool exchange_bit(LinkPort& source, bool outgoing_high) noexcept override {
        if (&source != port_) return true;
        byte_ = static_cast<std::uint8_t>(
            (static_cast<unsigned>(byte_) << 1U) | (outgoing_high ? 1U : 0U));
        if (++bits_ == 8) {
            text_.push_back(static_cast<char>(byte_));
            byte_ = 0;
            bits_ = 0;
        }
        return true;
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    LinkPort* port_{};
    std::uint8_t byte_{};
    int bits_{};
    std::string text_;
};

[[noreturn]] void usage(std::string_view error = {}) {
    if (!error.empty()) std::cerr << "Erreur: " << error << "\n\n";
    std::cerr
        << "Usage: gb_conformance_runner ROM [options]\n"
        << "  --hardware auto|dmg|cgb\n"
        << "  --boot-rom FICHIER\n"
        << "  --category NOM\n"
        << "  --timeout-frames N\n"
        << "  --minimum-pass-frames N\n"
        << "  --memory-stability-samples N\n"
        << "  --memory-require-mismatch\n"
        << "  --serial-pass TEXTE | --serial-fail TEXTE\n"
        << "  --expect-memory ADRESSE=VALEUR (répétable)\n"
        << "  --expect-frame-argb8888 FICHIER\n"
        << "  --expect-frame-sha256 HEX\n"
        << "  --no-mooneye-signature\n";
    throw std::invalid_argument("arguments invalides");
}

int parse_number(std::string_view text) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    } else if (!text.empty() && text[0] == '$') {
        text.remove_prefix(1);
        base = 16;
    }
    unsigned value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size() || value > 0x7fffffffU) {
        usage("nombre invalide");
    }
    return static_cast<int>(value);
}

Options parse_options(int argc, char** argv) {
    if (argc < 2) usage();
    Options result;
    int first_option = 2;
    if (std::string_view{argv[1]} == "--self-test") {
        result.self_test = true;
        result.category = "harness-self-test";
        result.timeout_frames = 1;
        first_option = 2;
    } else {
        result.rom_path = argv[1];
    }
    for (int index = first_option; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&]() -> std::string {
            if (++index >= argc) usage("valeur d'option absente");
            return argv[index];
        };
        if (argument == "--hardware") result.hardware = value();
        else if (argument == "--boot-rom") result.boot_rom_path = value();
        else if (argument == "--category") result.category = value();
        else if (argument == "--timeout-frames") result.timeout_frames = parse_number(value());
        else if (argument == "--minimum-pass-frames") result.minimum_pass_frames = parse_number(value());
        else if (argument == "--memory-stability-samples") result.memory_stability_samples = parse_number(value());
        else if (argument == "--serial-pass") result.serial_pass = value();
        else if (argument == "--serial-fail") result.serial_fail = value();
        else if (argument == "--expect-frame-argb8888") result.framebuffer_path = value();
        else if (argument == "--expect-frame-sha256") result.framebuffer_sha256 = value();
        else if (argument == "--expect-memory") {
            const auto specification = value();
            const auto separator = specification.find('=');
            if (separator == std::string::npos) usage("attente mémoire sans '='");
            result.memory.push_back({parse_number(std::string_view{specification}.substr(0, separator)),
                                     parse_number(std::string_view{specification}.substr(separator + 1))});
        } else if (argument == "--no-mooneye-signature") result.mooneye_signature = false;
        else if (argument == "--memory-require-mismatch") result.memory_require_mismatch = true;
        else usage("option inconnue");
    }
    if (result.timeout_frames <= 0) usage("timeout nul ou négatif");
    if (result.memory_stability_samples <= 0) usage("stabilité mémoire nulle");
    if (result.minimum_pass_frames >= result.timeout_frames) {
        usage("le délai minimum de réussite doit être inférieur au timeout");
    }
    if (result.hardware != "auto" && result.hardware != "dmg" && result.hardware != "cgb") {
        usage("modèle matériel inconnu");
    }
    return result;
}

std::vector<std::uint8_t> make_self_test_rom() {
    std::vector<std::uint8_t> rom(0x8000);
    rom[0x0100] = 0xc3; // JP $0150, au-delà de l'en-tête cartouche
    rom[0x0101] = 0x50;
    rom[0x0102] = 0x01;
    std::size_t cursor = 0x0150;
    const std::array<std::uint8_t, 5> memory_marker{
        0x3e, 0x42,       // LD A,$42
        0xea, 0x00, 0xc0, // LD [$C000],A
    };
    std::copy(memory_marker.begin(), memory_marker.end(),
              rom.begin() + static_cast<std::ptrdiff_t>(cursor));
    cursor += memory_marker.size();
    for (const char character : std::string_view{"Passed"}) {
        const std::array<std::uint8_t, 14> send{
            0x3e, static_cast<std::uint8_t>(character), // LD A,caractère
            0xe0, 0x01,            // LDH [SB],A
            0x3e, 0x81,            // LD A,$81
            0xe0, 0x02,            // LDH [SC],A
            0xf0, 0x02,            // attente: LDH A,[SC]
            0xcb, 0x7f,            // BIT 7,A
            0x20, 0xfa,            // JR NZ,attente
        };
        std::copy(send.begin(), send.end(), rom.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += send.size();
    }
    rom[cursor++] = 0x18; // JR -2
    rom[cursor] = 0xfe;
    rom[0x147] = 0x00;
    return rom;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("impossible d'ouvrir " + path);
    const auto end = stream.tellg();
    if (end < 0) throw std::runtime_error("taille de fichier indisponible");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("lecture incomplète de " + path);
    }
    return bytes;
}

gb::HardwareMode resolve_mode(const Options& options, const CartridgeHeader& header) {
    if (options.hardware == "dmg") {
        if (header.requires_color) throw RomLoadError("test CGB-only demandé sur DMG");
        return gb::HardwareMode::dmg;
    }
    if (options.hardware == "cgb") {
        return header.uses_color ? gb::HardwareMode::cgb_native
                                 : gb::HardwareMode::cgb_compatibility;
    }
    return header.uses_color ? gb::HardwareMode::cgb_native : gb::HardwareMode::dmg;
}

std::vector<std::uint8_t> serialize_frame(const Ppu& ppu) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ppu.completed_frame.size() * 4U);
    for (const auto pixel : ppu.completed_frame) {
        const auto value = static_cast<std::uint32_t>(pixel);
        bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
        bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

std::string frame_hash(const std::vector<std::uint8_t>& bytes) {
    const auto digest = detail::sha256(bytes);
    std::ostringstream hex;
    for (const auto byte : digest) hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return hex.str();
}

bool mooneye_passed(const Cpu& cpu) noexcept {
    return cpu.b == 3 && cpu.c == 5 && cpu.d == 8 && cpu.e == 13 && cpu.h == 21 && cpu.l == 34;
}

bool memory_passed(Machine& machine, const std::vector<MemoryExpectation>& expected) {
    if (expected.empty()) return false;
    for (const auto& item : expected) {
        if (machine.bus.read(item.address) != (item.value & 0xff)) return false;
    }
    return true;
}

int run(const Options& options) {
    auto rom_bytes = options.self_test ? make_self_test_rom() : read_file(options.rom_path);
    auto rom = std::make_shared<const std::vector<std::uint8_t>>(std::move(rom_bytes));
    const auto header = CartridgeHeader::parse(*rom);
    const auto boot = options.boot_rom_path.empty()
        ? std::vector<std::uint8_t>{} : read_file(options.boot_rom_path);
    const bool expected_frame_file = !options.framebuffer_path.empty();
    const auto expected_frame = !expected_frame_file
        ? std::vector<std::uint8_t>{} : read_file(options.framebuffer_path);
    constexpr auto expected_frame_bytes =
        static_cast<std::size_t>(Ppu::frame_pixels) * sizeof(std::uint32_t);
    if (expected_frame_file && expected_frame.size() != expected_frame_bytes) {
        throw std::invalid_argument(
            "framebuffer ARGB8888 attendu de taille " +
            std::to_string(expected_frame_bytes) + " octets");
    }
    // Le transport doit vivre plus longtemps que le port qui s'y détache dans
    // son destructeur.
    SerialCapture serial;
    Machine machine(rom, [] { return std::int64_t{0}; }, resolve_mode(options, header), boot);
    if (!machine.serial.connect(&serial)) throw std::runtime_error("capture série refusée");

    constexpr std::int64_t dots_per_frame = 70'224;
    const auto timeout = static_cast<std::int64_t>(options.timeout_frames) * dots_per_frame;
    const auto minimum_pass_dots =
        static_cast<std::int64_t>(options.minimum_pass_frames) * dots_per_frame;
    std::int64_t dots{};
    std::int64_t next_frame = dots_per_frame;
    int signature_stability{};
    int memory_stability{};
    bool memory_mismatch_observed =
        !options.memory.empty() && !memory_passed(machine, options.memory);
    std::string last_hash;
    std::optional<std::size_t> last_frame_mismatch;
    std::uint8_t last_frame_expected{};
    std::uint8_t last_frame_actual{};

    while (dots < timeout) {
        const int advanced = machine.step();
        if (advanced > 0) dots += advanced;

        const auto& text = serial.text();
        if (!options.serial_fail.empty() && text.find(options.serial_fail) != std::string::npos) {
            std::cout << "FAIL category=" << options.category << " mechanism=serial dots=" << dots
                      << " output=" << std::quoted(text) << "\n";
            return 1;
        }
        const bool pass_window_open = dots >= minimum_pass_dots;
        if (pass_window_open && !options.serial_pass.empty() &&
            text.find(options.serial_pass) != std::string::npos) {
            std::cout << "PASS category=" << options.category << " mechanism=serial dots=" << dots
                      << " output=" << std::quoted(text) << "\n";
            return 0;
        }

        signature_stability = pass_window_open && options.mooneye_signature &&
            mooneye_passed(machine.cpu)
            ? signature_stability + 1 : 0;
        if (signature_stability >= 8) {
            std::cout << "PASS category=" << options.category
                      << " mechanism=register-signature dots=" << dots << "\n";
            return 0;
        }

        const bool memory_matches = memory_passed(machine, options.memory);
        if (!options.memory.empty() && !memory_matches) {
            memory_mismatch_observed = true;
            memory_stability = 0;
        } else if (memory_matches &&
                   (!options.memory_require_mismatch || memory_mismatch_observed)) {
            ++memory_stability;
        } else {
            memory_stability = 0;
        }
        if (pass_window_open && memory_stability >= options.memory_stability_samples) {
            std::cout << "PASS category=" << options.category
                      << " mechanism=memory dots=" << dots << "\n";
            return 0;
        }

        const bool frame_expected =
            expected_frame_file || !options.framebuffer_sha256.empty();
        if (pass_window_open && frame_expected && dots >= next_frame) {
            const auto current_frame = serialize_frame(machine.ppu);
            last_hash = frame_hash(current_frame);
            if (expected_frame_file && current_frame == expected_frame) {
                std::cout << "PASS category=" << options.category
                          << " mechanism=framebuffer-argb8888 dots=" << dots << "\n";
                return 0;
            }
            if (expected_frame_file) {
                const auto mismatch = std::mismatch(
                    expected_frame.begin(), expected_frame.end(), current_frame.begin());
                if (mismatch.first != expected_frame.end()) {
                    last_frame_mismatch = static_cast<std::size_t>(
                        std::distance(expected_frame.begin(), mismatch.first));
                    last_frame_expected = *mismatch.first;
                    last_frame_actual = *mismatch.second;
                }
            }
            if (last_hash == options.framebuffer_sha256) {
                std::cout << "PASS category=" << options.category
                          << " mechanism=framebuffer-sha256 dots=" << dots << "\n";
                return 0;
            }
            while (next_frame <= dots) next_frame += dots_per_frame;
        }

        // STOP peut etre le dernier opcode d'un test. Evalue toutes les voies
        // de succes avant de signaler qu'aucune horloge ne peut plus avancer.
        if (advanced <= 0) {
            std::cout << "FAIL category=" << options.category
                      << " reason=cpu-stopped dots=" << dots << "\n";
            return 1;
        }
    }

    std::cout << "TIMEOUT category=" << options.category << " frames=" << options.timeout_frames;
    if (!last_hash.empty()) std::cout << " last-frame-sha256=" << last_hash;
    if (last_frame_mismatch) {
        std::cout << " frame-mismatch-byte=" << *last_frame_mismatch
                  << " expected=" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(last_frame_expected)
                  << " actual=" << std::setw(2) << static_cast<int>(last_frame_actual)
                  << std::dec;
    }
    if (!serial.text().empty()) std::cout << " serial=" << std::quoted(serial.text());
    std::cout << " registers=" << std::hex << std::setfill('0')
              << "AF:" << std::setw(4) << machine.cpu.af()
              << ",BC:" << std::setw(4) << machine.cpu.bc()
              << ",DE:" << std::setw(4) << machine.cpu.de()
              << ",HL:" << std::setw(4) << machine.cpu.hl()
              << ",SP:" << std::setw(4) << machine.cpu.sp
              << ",PC:" << std::setw(4) << machine.cpu.pc
              << " timer=DIV:" << std::setw(2) << machine.timer.read_div()
              << ",TIMA:" << std::setw(2) << machine.timer.read_tima()
              << ",TMA:" << std::setw(2) << machine.timer.tma()
              << ",TAC:" << std::setw(2) << machine.timer.read_tac()
              << ",IF:" << std::setw(2) << machine.interrupts.flags
              << ",IE:" << std::setw(2) << machine.interrupts.enable << std::dec;
    if (!options.memory.empty()) {
        std::cout << " memory=";
        for (std::size_t index = 0; index < options.memory.size(); ++index) {
            const auto& item = options.memory[index];
            if (index != 0) std::cout << ',';
            std::cout << std::hex << std::setfill('0') << std::setw(4) << item.address
                      << ':' << std::setw(2) << machine.bus.read(item.address)
                      << "(expected:" << std::setw(2) << (item.value & 0xff) << ')' << std::dec;
        }
    }
    std::cout << "\n";
    return 1;
}

} // namespace ravenemu::cgb::conformance

int main(int argc, char** argv) {
    try {
        return ravenemu::cgb::conformance::run(
            ravenemu::cgb::conformance::parse_options(argc, argv));
    } catch (const std::invalid_argument& error) {
        if (std::string_view{error.what()} != "arguments invalides") {
            std::cerr << "ERROR type=invalid-argument message=" << std::quoted(error.what()) << "\n";
        }
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "ERROR type=exception message=" << std::quoted(error.what()) << "\n";
        return 2;
    } catch (...) {
        std::cerr << "ERROR type=unknown-exception\n";
        return 2;
    }
}
