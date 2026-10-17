#pragma once

#include "cpu/cpu.hpp"
#include "apu/apu.hpp"
#include "timers/timers.hpp"
#include "dma/dma_controller.hpp"
#include "bios/decompression.hpp"

namespace ravenemu::gba {

class Bios {
public:
    struct WaitState {
        int interrupt_mask{};
        bool discard_old_flags{};
    };
    Bios(Cpu& cpu, Bus& bus) : cpu_(cpu), bus_(bus), decompression_(bus) { install_irq_handler(); }

    void handle_swi(int number) {
        handle_swi_function(number);
        // Le BIOS d'origine rend la main depuis la même adresse après chaque
        // SWI ; SoftReset repart comme au démarrage.
        bus_.bios_latch = number == 0 || number == 0x26 ? Bus::bios_latch_after_startup : Bus::bios_latch_after_swi;
        bus_.executing_bios = false;
    }
    void handle_swi_function(int number) {
        auto& registers = cpu_.state.regs;
        bus_.diagnostics.swi(number);
        switch (number) {
        case 0x00: soft_reset(); break;
        case 0x01: register_ram_reset(registers[0]); break;
        case 0x02:
        case 0x03:
            wait_state_.reset();
            cpu_.state.halted = true;
            break;
        case 0x04: intr_wait(registers[0] != 0, registers[1] & 0x3fff); break;
        case 0x05: intr_wait(true, 1 << InterruptController::vblank); break;
        case 0x06: divide(registers[0], registers[1]); break;
        case 0x07: divide(registers[1], registers[0]); break;
        case 0x08: registers[0] = integer_sqrt(u32(registers[0])); break;
        case 0x09: {
            const auto result = arc_tan(registers[0]);
            registers[0] = result.angle;
            registers[1] = result.square;
            registers[3] = result.sum;
            break;
        }
        case 0x0a: arc_tan2(registers[0], registers[1]); break;
        case 0x0b: cpu_set(registers[0], registers[1], registers[2]); break;
        case 0x0c: cpu_fast_set(registers[0], registers[1], registers[2]); break;
        case 0x0d: registers[0] = i32(0xbaae'187fU); break;  // GetBiosChecksum
        case 0x0e: bg_affine_set(registers[0], registers[1], registers[2]); break;
        case 0x0f: obj_affine_set(registers[0], registers[1], registers[2], registers[3]); break;
        case 0x10: decompression_.bit_unpack(registers[0], registers[1], registers[2]); break;
        case 0x11: decompression_.lz77(registers[0], registers[1], false); break;
        case 0x12: decompression_.lz77(registers[0], registers[1], true); break;
        case 0x13: decompression_.huffman(registers[0], registers[1]); break;
        case 0x14: decompression_.run_length(registers[0], registers[1], false); break;
        case 0x15: decompression_.run_length(registers[0], registers[1], true); break;
        case 0x16: decompression_.differential(registers[0], registers[1], false, false); break;
        case 0x17: decompression_.differential(registers[0], registers[1], false, true); break;
        case 0x18: decompression_.differential(registers[0], registers[1], true, true); break;
        case 0x19: sound_bias(registers[0] != 0); break;
        case 0x1f: registers[0] = midi_key_to_frequency(registers[0], registers[1], registers[2]); break;
        case 0x25: registers[0] = 1; break;  // MultiBoot : échec, aucun câble
        case 0x26: hard_reset(); break;
        case 0x27: halt(); break;  // CustomHalt : arrêt jusqu'à une interruption
        case 0x28:
        case 0x29: break;  // SoundDriverVSyncOff/On : sans pilote BIOS, rien à suspendre
        default:
            bus_.diagnostics.report(DiagnosticEvent::unsupported_swi,
                                    "appel logiciel GBA non pris en charge " + std::to_string(number));
            break;
        }
    }
    void interrupt_raised(int mask) { write_flags(flags() | mask); }
    [[nodiscard]] bool should_resume() {
        if (!bus_.interrupts) return true;
        if (!wait_state_) return (bus_.interrupts->enable & bus_.interrupts->flags & 0x3fff) != 0;
        if ((flags() & wait_state_->interrupt_mask) == 0) return false;
        clear_flags(wait_state_->interrupt_mask);
        wait_state_.reset();
        return true;
    }
    [[nodiscard]] const std::optional<WaitState>& wait_state() const noexcept { return wait_state_; }
    // Halt (SWI 2, CustomHalt ou écriture de HALTCNT) : reprise à la première
    // interruption activée et demandée, sans exiger IME.
    void halt() noexcept {
        wait_state_.reset();
        cpu_.state.halted = true;
    }
    void restore_wait_state(std::optional<WaitState> value) noexcept { wait_state_ = value; }

private:
    static constexpr std::int32_t interrupt_flags_address = i32(0x03007ff8U);
    void install_irq_handler() noexcept {
        // Même séquence que le gestionnaire du BIOS d'origine. Le mot qui le
        // suit n'est jamais exécuté, mais il est préchargé au retour : c'est
        // la valeur que GBATEK documente pour une lecture du BIOS après une IRQ.
        constexpr std::array<std::uint32_t, 8> handler{
            0xe92d500fU, 0xe3a00301U, 0xe28fe000U, 0xe510f004U, 0xe8bd500fU, 0xe25ef004U, 0, 0xe55ec002U,
        };
        auto offset = 0x18U;
        for (const auto word : handler)
            for (unsigned byte = 0; byte < 4; ++byte) {
                bus_.bios[offset++] = static_cast<std::uint8_t>(word >> (byte * 8U));
            }
    }
    int flags() { return bus_.read32(interrupt_flags_address); }
    void write_flags(int value) { bus_.write32(interrupt_flags_address, value); }
    void clear_flags(int mask) { write_flags(flags() & ~mask); }
    void intr_wait(bool discard, int mask) {
        if (bus_.interrupts) bus_.interrupts->master_enable = true;
        if (mask == 0) return;
        if (discard)
            clear_flags(mask);
        else if ((flags() & mask) != 0) {
            clear_flags(mask);
            return;
        }
        wait_state_ = WaitState{mask, discard};
        cpu_.state.halted = true;
    }
    // SoftReset (GBATEK) : efface 0x03007E00..0x03007FFF (piles, vecteur et
    // drapeaux d'IRQ), réinitialise les registres et repart en ROM, ou en
    // EWRAM si l'octet 0x03007FFA est non nul (jeux chargés par MultiBoot).
    void soft_reset() {
        const auto entry = bus_.iwram[0x7ffaU] != 0 ? i32(0x0200'0000U) : i32(0x0800'0000U);
        std::fill(bus_.iwram.end() - 0x200, bus_.iwram.end(), std::uint8_t{0});
        cpu_.reset(entry);
        cpu_.branch_to(entry);
    }
    // HardReset : comme à la mise sous tension, sans l'animation du BIOS.
    void hard_reset() {
        register_ram_reset(0xff);
        bus_.iwram.fill(0);
        cpu_.reset(i32(0x0800'0000U));
        cpu_.branch_to(i32(0x0800'0000U));
    }
    // SoundBias : niveau de SOUNDBIAS porté à 0x200 ou ramené à 0 (le BIOS y
    // va par petits pas ; seul le niveau final est observable ensuite).
    void sound_bias(bool raise) {
        const auto current = bus_.read16(i32(0x0400'0088U));
        bus_.write16(i32(0x0400'0088U), (current & 0xfc00) | (raise ? 0x200 : 0));
    }
    // MidiKey2Freq : fréquence de WaveData divisée par 2^((180 - touche - fin/256) / 12).
    std::int32_t midi_key_to_frequency(std::int32_t wave, std::int32_t key, std::int32_t fine) {
        const auto base = static_cast<double>(u32(bus_.read32(add32(wave, 4))));
        const auto exponent = (180.0 - static_cast<double>(key) - static_cast<double>(fine & 0xff) / 256.0) / 12.0;
        return i32(static_cast<std::uint32_t>(base / std::exp2(exponent)));
    }
    void register_ram_reset(int flags) {
        if ((flags & 1) != 0) bus_.ewram.fill(0);
        if ((flags & 2) != 0) std::fill(bus_.iwram.begin(), bus_.iwram.end() - 0x200, 0);
        if ((flags & 4) != 0) bus_.palette.fill(0);
        if ((flags & 8) != 0) bus_.vram.fill(0);
        if ((flags & 0x10) != 0) bus_.oam.fill(0);
        if ((flags & 0x20) != 0) std::fill(bus_.io.begin() + 0x120, bus_.io.begin() + 0x160, 0);
        if ((flags & 0x40) != 0) {
            std::fill(bus_.io.begin() + 0x60, bus_.io.begin() + 0x0a8, 0);
            if (bus_.apu) bus_.apu->reset();
        }
        if ((flags & 0x80) != 0) {
            std::fill(bus_.io.begin(), bus_.io.begin() + 0x60, 0);
            std::fill(bus_.io.begin() + 0x0a8, bus_.io.begin() + 0x120, 0);
            std::fill(bus_.io.begin() + 0x160, bus_.io.end(), 0);
            if (bus_.timers) bus_.timers->reset();
            if (bus_.interrupts) bus_.interrupts->reset();
            if (bus_.dma) bus_.dma->reset();
            bus_.reset_affine_matrices();
        }
    }
    // Div : quotient, reste et valeur absolue du quotient. Par zéro, la
    // console rend le signe du numérateur (1 pour 0), le numérateur en reste
    // et 1 en r3 ; -0x80000000 / -1 déborde en -0x80000000.
    void divide(std::int32_t numerator, std::int32_t denominator) noexcept {
        auto& registers = cpu_.state.regs;
        if (denominator == 0) {
            registers[0] = numerator < 0 ? -1 : 1;
            registers[1] = numerator;
            registers[3] = 1;
            return;
        }
        const auto quotient = bios_quotient(numerator, denominator);
        registers[0] = quotient;
        registers[1] = i32(static_cast<std::uint32_t>(static_cast<std::int64_t>(numerator) % denominator));
        registers[3] = quotient < 0 ? i32(0U - u32(quotient)) : quotient;
    }
    // Division entière du BIOS, tronquée vers zéro, sur 32 bits.
    static std::int32_t bios_quotient(std::int32_t numerator, std::int32_t denominator) noexcept {
        return i32(static_cast<std::uint32_t>(static_cast<std::int64_t>(numerator) / denominator));
    }
    static int integer_sqrt(std::uint32_t value) noexcept {
        auto root = static_cast<std::uint32_t>(std::sqrt(static_cast<double>(value)));
        while (static_cast<std::uint64_t>(root) * root > value) --root;
        while (static_cast<std::uint64_t>(root + 1U) * (root + 1U) <= value) ++root;
        return static_cast<int>(root);
    }
    /**
     * ArcTan du BIOS : tangente t en virgule fixe 1.14, angle sur 16 bits (un
     * tour vaut 0x10000). Le BIOS n'évalue pas l'arc tangente exacte, mais la
     * série t·(1 − t²/3 + t⁴/5 − …), coupée au degré 15 et aux coefficients
     * retouchés, par la méthode de Horner sur a = −t² / 2^14. Le dernier
     * coefficient, 0xA2F9, vaut à peu près 2^17 / π. Tout se calcule sur
     * 32 bits signés : les produits débordent comme ceux de l'ARM et les
     * décalages sont arithmétiques. Le BIOS lit r0 en entier, si bien qu'au-delà
     * de [−1, 1] il rend un angle sans signification, celui de la console.
     * Coefficients du BIOS d'origine, vérifiés sur les registres mesurés sur
     * console, où restent a (r1) et la valeur du polynôme (r3).
     */
    struct ArcTan {
        std::int32_t angle;   // r0
        std::int32_t square;  // r1 : a
        std::int32_t sum;     // r3 : polynôme, multiplié ensuite par t
    };
    static ArcTan arc_tan(std::int32_t tangent) noexcept {
        static constexpr std::array<std::int32_t, 7> coefficients{0x390, 0x91c, 0xfb6, 0x16aa, 0x2081, 0x3651, 0xa2f9};
        const auto product = [](std::int32_t left, std::int32_t right) { return i32(u32(left) * u32(right)); };
        const auto square = -(product(tangent, tangent) >> 14);
        auto sum = std::int32_t{0xa9};
        for (const auto coefficient : coefficients) sum = (product(sum, square) >> 14) + coefficient;
        return {product(tangent, sum) >> 16, square, sum};
    }
    /**
     * ArcTan2 du BIOS : angle de (x, y) sur un tour, de 0 à 0xFFFF. Le BIOS
     * divise la plus petite coordonnée par la plus grande, en valeur absolue,
     * puis passe la tangente obtenue, entre −1 et 1, à ArcTan et replace
     * l'angle dans son quadrant. Les coordonnées sont décalées de 14 bits sur
     * 32 bits avant la division, sans garde contre le débordement. Il laisse
     * en r1 le a d'ArcTan, et en r3 l'adresse 0x170 de sa propre routine.
     */
    void arc_tan2(std::int32_t x, std::int32_t y) noexcept {
        auto& registers = cpu_.state.regs;
        registers[3] = 0x170;
        if (y == 0) {
            registers[0] = x < 0 ? 0x8000 : 0;
            return;
        }
        if (x == 0) {
            registers[0] = y < 0 ? 0xc000 : 0x4000;
            return;
        }
        // Comparaisons du BIOS, avec la négation sur 32 bits : à égalité, la
        // division se fait par x, sauf quand x et y sont tous deux négatifs.
        const auto minus_x = i32(0U - u32(x));
        const auto minus_y = i32(0U - u32(y));
        const auto divide_by_x = y > 0 ? (x > 0 ? x >= y : minus_x >= y) : (x < 0 ? minus_x > minus_y : x >= minus_y);
        const auto shifted = [](std::int32_t value) { return i32(u32(value) << 14U); };
        std::int32_t angle{};
        ArcTan result{};
        if (divide_by_x) {
            result = arc_tan(bios_quotient(shifted(y), x));
            angle = result.angle + (x < 0 ? 0x8000 : 0);
        } else {
            result = arc_tan(bios_quotient(shifted(x), y));
            angle = (y < 0 ? 0xc000 : 0x4000) - result.angle;
        }
        registers[0] = angle & 0xffff;  // un angle négatif revient sur le tour
        registers[1] = result.square;
    }
    void cpu_set(std::int32_t source, std::int32_t destination, std::int32_t control) {
        const auto count = static_cast<int>(u32(control) & 0x1fffffU);
        const auto fixed = (u32(control) & (1U << 24U)) != 0;
        const auto words = (u32(control) & (1U << 26U)) != 0;
        const auto unit = words ? 4 : 2;
        for (int index = 0; index < count; ++index) {
            if (words)
                bus_.write32(destination, bus_.read32(source));
            else
                bus_.write16(destination, bus_.read16(source));
            if (!fixed) source = add32(source, unit);
            destination = add32(destination, unit);
        }
    }
    void cpu_fast_set(std::int32_t source, std::int32_t destination, std::int32_t control) {
        const auto count = static_cast<int>(u32(control) & 0x1fffffU);
        const auto fixed = (u32(control) & (1U << 24U)) != 0;
        for (int index = 0; index < count; ++index) {
            bus_.write32(destination, bus_.read32(source));
            if (!fixed) source = add32(source, 4);
            destination = add32(destination, 4);
        }
    }
    void bg_affine_set(std::int32_t source, std::int32_t destination, int count) {
        count = std::clamp(count, 0, 512);
        for (int entry = 0; entry < count; ++entry) {
            const auto origin_x = bus_.read32(source);
            const auto origin_y = bus_.read32(add32(source, 4));
            const auto screen_x = static_cast<std::int16_t>(bus_.read16(add32(source, 8)));
            const auto screen_y = static_cast<std::int16_t>(bus_.read16(add32(source, 10)));
            const auto scale_x = static_cast<std::int16_t>(bus_.read16(add32(source, 12)));
            const auto scale_y = static_cast<std::int16_t>(bus_.read16(add32(source, 14)));
            const auto angle =
                static_cast<double>((bus_.read16(add32(source, 16)) >> 8) & 0xff) * 2.0 * std::numbers::pi / 256.0;
            source = add32(source, 20);
            const auto cosine = std::cos(angle);
            const auto sine = std::sin(angle);
            const auto pa = static_cast<std::int32_t>(cosine * scale_x);
            const auto pb = static_cast<std::int32_t>(-sine * scale_x);
            const auto pc = static_cast<std::int32_t>(sine * scale_y);
            const auto pd = static_cast<std::int32_t>(cosine * scale_y);
            bus_.write16(destination, pa);
            bus_.write16(add32(destination, 2), pb);
            bus_.write16(add32(destination, 4), pc);
            bus_.write16(add32(destination, 6), pd);
            bus_.write32(add32(destination, 8),
                         add32(origin_x, i32(0U - u32(i32(u32(pa) * static_cast<std::uint32_t>(screen_x) +
                                                          u32(pb) * static_cast<std::uint32_t>(screen_y))))));
            bus_.write32(add32(destination, 12),
                         add32(origin_y, i32(0U - u32(i32(u32(pc) * static_cast<std::uint32_t>(screen_x) +
                                                          u32(pd) * static_cast<std::uint32_t>(screen_y))))));
            destination = add32(destination, 16);
        }
    }
    void obj_affine_set(std::int32_t source, std::int32_t destination, int count, int stride) {
        count = std::clamp(count, 0, 512);
        for (int entry = 0; entry < count; ++entry) {
            const auto scale_x = static_cast<std::int16_t>(bus_.read16(source));
            const auto scale_y = static_cast<std::int16_t>(bus_.read16(add32(source, 2)));
            const auto angle =
                static_cast<double>((bus_.read16(add32(source, 4)) >> 8) & 0xff) * 2.0 * std::numbers::pi / 256.0;
            source = add32(source, 8);
            const auto cosine = std::cos(angle);
            const auto sine = std::sin(angle);
            bus_.write16(destination, static_cast<int>(cosine * scale_x));
            bus_.write16(add32(destination, stride), static_cast<int>(-sine * scale_x));
            bus_.write16(add32(destination, stride * 2), static_cast<int>(sine * scale_y));
            bus_.write16(add32(destination, stride * 3), static_cast<int>(cosine * scale_y));
            destination = add32(destination, stride * 4);
        }
    }

    Cpu& cpu_;
    Bus& bus_;
    BiosDecompression decompression_;
    std::optional<WaitState> wait_state_;
};

}  // namespace ravenemu::gba
