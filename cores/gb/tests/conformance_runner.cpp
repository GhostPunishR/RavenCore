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
    std::string framebuffer_rgb_path;
    std::string compatibility_palette;
    std::vector<MemoryExpectation> memory;
    std::vector<MemoryExpectation> memory_before;
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
        << "  --hardware auto|dmg|cgb|agb\n"
        << "  --boot-rom FICHIER\n"
        << "  --category NOM\n"
        << "  --timeout-frames N\n"
        << "  --minimum-pass-frames N\n"
        << "  --memory-stability-samples N\n"
        << "  --memory-require-mismatch\n"
        << "  --serial-pass TEXTE | --serial-fail TEXTE\n"
        << "  --expect-memory ADRESSE=VALEUR (répétable)\n"
        << "  --expect-memory-before ADRESSE=VALEUR (répétable)\n"
        << "  --expect-frame-argb8888 FICHIER\n"
        << "  --expect-frame-rgb888 FICHIER\n"
        << "  --expect-frame-sha256 HEX\n"
        << "  --compatibility-palette boot-default\n"
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
        else if (argument == "--expect-frame-rgb888") result.framebuffer_rgb_path = value();
        else if (argument == "--expect-frame-sha256") result.framebuffer_sha256 = value();
        else if (argument == "--compatibility-palette") result.compatibility_palette = value();
        else if (argument == "--expect-memory" || argument == "--expect-memory-before") {
            const auto specification = value();
            const auto separator = specification.find('=');
            if (separator == std::string::npos) usage("attente mémoire sans '='");
            auto& target = argument == "--expect-memory" ? result.memory : result.memory_before;
            target.push_back({parse_number(std::string_view{specification}.substr(0, separator)),
                              parse_number(std::string_view{specification}.substr(separator + 1))});
        } else if (argument == "--no-mooneye-signature") result.mooneye_signature = false;
        else if (argument == "--memory-require-mismatch") result.memory_require_mismatch = true;
        else usage("option inconnue");
    }
    if (!result.memory_before.empty() && result.memory.empty()) {
        usage("--expect-memory-before exige --expect-memory");
    }
    if (result.timeout_frames <= 0) usage("timeout nul ou négatif");
    if (result.memory_stability_samples <= 0) usage("stabilité mémoire nulle");
    if (result.minimum_pass_frames >= result.timeout_frames) {
        usage("le délai minimum de réussite doit être inférieur au timeout");
    }
    if (result.hardware != "auto" && result.hardware != "dmg" && result.hardware != "cgb" &&
        result.hardware != "agb") {
        usage("modèle matériel inconnu");
    }
    if (!result.compatibility_palette.empty() && result.compatibility_palette != "boot-default") {
        usage("palette de compatibilité inconnue");
    }
    if (!result.framebuffer_path.empty() + !result.framebuffer_rgb_path.empty() +
            !result.framebuffer_sha256.empty() > 1) {
        usage("une seule image attendue à la fois");
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
    // agb : la GBC d'une Game Boy Advance, que seuls ses registres de démarrage distinguent.
    if (options.hardware == "cgb" || options.hardware == "agb") {
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

/**
 * Image en RGB 8 bits par canal, l'espace des captures de référence publiques
 * (c-sp/game-boy-test-roms) : les quatre gris DMG valent FF, AA, 55 et 00 ;
 * en CGB, le PPU développe déjà chaque canal 5 bits en (c << 3) | (c >> 2).
 */
std::vector<std::uint8_t> frame_rgb(const Ppu& ppu, bool dmg_shades) {
    static constexpr std::array<std::uint8_t, 4> grays{0xff, 0xaa, 0x55, 0x00};
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ppu.completed_frame.size() * 3U);
    for (const auto pixel : ppu.completed_frame) {
        const auto value = static_cast<std::uint32_t>(pixel);
        if (dmg_shades) {
            const auto gray = grays[value & 3U];
            bytes.insert(bytes.end(), {gray, gray, gray});
        } else {
            bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
            bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
            bytes.push_back(static_cast<std::uint8_t>(value));
        }
    }
    return bytes;
}

/**
 * Un écran DMG n'affiche que quatre gris. Une capture de référence DMG peut
 * contenir des teintes intermédiaires, issues du mélange d'images de
 * l'émulateur qui l'a produite (oam_bug-dmg.png en a 50) : chaque pixel est
 * ramené au gris le plus proche, ce qui laisse intacts les quatre gris purs.
 */
void snap_to_dmg_shades(std::vector<std::uint8_t>& rgb) {
    for (std::size_t offset = 0; offset + 2 < rgb.size(); offset += 3) {
        const int level = (rgb[offset] + rgb[offset + 1] + rgb[offset + 2]) / 3;
        const auto shade = static_cast<std::uint8_t>((level + 0x2a) / 0x55 * 0x55);
        rgb[offset] = rgb[offset + 1] = rgb[offset + 2] = shade;
    }
}

/**
 * Palettes que le boot ROM CGB choisit pour une cartouche DMG qu'il ne
 * reconnaît pas. Sans boot ROM, le cœur garde des gris neutres ; les captures
 * de référence en mode compatibilité supposent celles-ci. Elles passent par
 * les mêmes registres que le boot ROM.
 */
void apply_boot_default_compatibility_palette(Ppu& ppu) {
    static constexpr std::array<int, 4> background{0x7fff, 0x1bef, 0x6180, 0x0000};
    static constexpr std::array<int, 4> objects{0x7fff, 0x421f, 0x1cf2, 0x0000};
    ppu.write_bcps(0x80);
    for (const int color : background) { ppu.write_bcpd(color & 0xff); ppu.write_bcpd(color >> 8); }
    ppu.write_ocps(0x80);
    for (int palette = 0; palette < 2; ++palette) {
        for (const int color : objects) { ppu.write_ocpd(color & 0xff); ppu.write_ocpd(color >> 8); }
    }
    ppu.write_bcps(0);
    ppu.write_ocps(0);
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

// Un test Mooneye en échec charge $42 dans B, C, D, E, H et L avant son LD B,B.
bool mooneye_failed(const Cpu& cpu) noexcept {
    return cpu.b == 0x42 && cpu.c == 0x42 && cpu.d == 0x42 && cpu.e == 0x42 && cpu.h == 0x42 && cpu.l == 0x42;
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
    // Une seule image attendue : ARGB8888 brut du cœur, ou RGB888 d'une capture.
    const bool rgb_frame = !options.framebuffer_rgb_path.empty();
    const bool expected_frame_file = rgb_frame || !options.framebuffer_path.empty();
    auto expected_frame = !expected_frame_file
        ? std::vector<std::uint8_t>{}
        : read_file(rgb_frame ? options.framebuffer_rgb_path : options.framebuffer_path);
    const auto expected_frame_bytes =
        static_cast<std::size_t>(Ppu::frame_pixels) * (rgb_frame ? 3U : sizeof(std::uint32_t));
    if (expected_frame_file && expected_frame.size() != expected_frame_bytes) {
        throw std::invalid_argument(
            std::string{"framebuffer "} + (rgb_frame ? "RGB888" : "ARGB8888") +
            " attendu de taille " + std::to_string(expected_frame_bytes) + " octets");
    }
    // Le transport doit vivre plus longtemps que le port qui s'y détache dans
    // son destructeur.
    SerialCapture serial;
    const auto mode = resolve_mode(options, header);
    if (rgb_frame && mode == gb::HardwareMode::dmg) snap_to_dmg_shades(expected_frame);
    Machine machine(rom, [] { return std::int64_t{0}; }, mode, boot, options.hardware == "agb");
    if (!machine.serial.connect(&serial)) throw std::runtime_error("capture série refusée");
    if (options.compatibility_palette == "boot-default" && boot.empty() &&
        mode == gb::HardwareMode::cgb_compatibility) {
        apply_boot_default_compatibility_palette(machine.ppu);
    }

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
    // État intermédiaire à voir avant la réussite mémoire. Un test Blargg écrit
    // sa signature alors que $A000 vaut encore le $00 d'une RAM effacée, puis
    // passe à $80 pendant l'exécution : seul le $00 qui suit ce $80 compte.
    bool memory_before_observed = options.memory_before.empty();
    std::string last_hash;
    std::optional<std::size_t> last_frame_mismatch;
    std::size_t last_frame_differences{};
    std::uint8_t last_frame_expected{};
    std::uint8_t last_frame_actual{};
    int failure_signature_stability{};

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
        failure_signature_stability = options.mooneye_signature && mooneye_failed(machine.cpu)
            ? failure_signature_stability + 1 : 0;
        if (failure_signature_stability >= 8) {
            std::cout << "FAIL category=" << options.category
                      << " mechanism=register-signature dots=" << dots
                      << " reason=failure-signature-0x42\n";
            return 1;
        }

        if (!memory_before_observed && memory_passed(machine, options.memory_before)) {
            memory_before_observed = true;
        }
        const bool memory_matches = memory_before_observed && memory_passed(machine, options.memory);
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
            const auto current_frame = rgb_frame
                ? frame_rgb(machine.ppu, mode == gb::HardwareMode::dmg)
                : serialize_frame(machine.ppu);
            last_hash = frame_hash(current_frame);
            if (expected_frame_file && current_frame == expected_frame) {
                std::cout << "PASS category=" << options.category << " mechanism=framebuffer-"
                          << (rgb_frame ? "rgb888" : "argb8888") << " dots=" << dots << "\n";
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
                const std::size_t pixel_bytes = rgb_frame ? 3U : 4U;
                last_frame_differences = 0;
                for (std::size_t offset = 0; offset < expected_frame.size(); offset += pixel_bytes) {
                    last_frame_differences += !std::equal(
                        expected_frame.begin() + static_cast<std::ptrdiff_t>(offset),
                        expected_frame.begin() + static_cast<std::ptrdiff_t>(offset + pixel_bytes),
                        current_frame.begin() + static_cast<std::ptrdiff_t>(offset));
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
        std::cout << " frame-differing-pixels=" << last_frame_differences
                  << " frame-mismatch-byte=" << *last_frame_mismatch
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
    if (!options.memory_before.empty()) {
        std::cout << " memory-before-observed=" << (memory_before_observed ? "yes" : "no");
    }
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
