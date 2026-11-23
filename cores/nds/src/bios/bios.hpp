#pragma once

#include "bios/decompression.hpp"
#include "machine/hardware.hpp"
#include "math/square_root.hpp"

#include <cstdint>
#include <limits>

// Fonctions SWI des BIOS du DS, exécutées par l'hôte (GBATEK, « BIOS Function
// Summary » et suivants). Numéros : octet haut du commentaire en ARM, octet
// bas en Thumb. Écrites pour RavenCore d'après leur description ; aucun code
// des BIOS de Nintendo.
//
// Pas encore écrites : décompressions par fonctions de rappel (0x12, 0x13,
// 0x15), tables du son de l'ARM7 (0x1A-0x1C), GetBootProcs, CustomPost,
// SoftReset. Elles ne font rien et sont comptées dans `unsupported`.
namespace ravenemu::nds {

template <typename CpuType, typename BusType, bool Arm9>
class Bios {
public:
    Bios(Hardware& hardware, CpuType& cpu, BusType& bus) noexcept : hw_(hardware), cpu_(cpu), bus_(bus) {}

    bool call(std::uint32_t comment, bool thumb) {
        auto& r = cpu_.regs.r;
        const auto number = thumb ? comment & 0xffU : (comment >> 16U) & 0xffU;
        switch (number) {
        case 0x03: bus_.internal(static_cast<int>(std::min<std::uint32_t>(r[0], 0x1000'0000U)) * 4); break;
        case 0x04: wait_for_interrupt(); break;
        case 0x05:
            r[0] = 1;
            r[1] = 1;
            wait_for_interrupt();
            break;
        case 0x06: halt(); break;
        case 0x07:  // Sleep (ARM7) : comme Halt, faute de mode veille modélisé
        case 0x1f:  // CustomHalt (ARM7) ; CustomPost sur l'ARM9
            if (Arm9) ++unsupported;
            else halt();
            break;
        case 0x08: break;  // SoundBias (ARM7) : le biais du son n'est pas modélisé
        case 0x09: divide(); break;
        case 0x0b: copy_or_fill(false); break;
        case 0x0c: copy_or_fill(true); break;
        case 0x0d: r[0] = integer_square_root(r[0]); break;
        case 0x0e: r[0] = crc16(r[0], r[1], r[2]); break;
        case 0x0f: r[0] = 0; break;  // IsDebugger : console de détail
        case 0x10: decompression::bit_unpack(bus_, r[0], r[1], r[2]); break;
        case 0x11: decompression::lz77(bus_, r[0], r[1]); break;
        case 0x14: decompression::run_length(bus_, r[0], r[1]); break;
        case 0x16:
        case 0x18:  // filtres différentiels : ARM9 seul
            if (Arm9) decompression::differential(bus_, r[0], r[1], number == 0x18);
            else ++unsupported;
            break;
        default: ++unsupported; break;
        }
        return true;
    }

    unsigned unsupported{};

private:
    void halt() noexcept {
        if constexpr (Arm9) hw_.arm9_halted = true;
        else hw_.arm7_halted = true;
    }

    // Mot que le gestionnaire d'IRQ du jeu complète à chaque interruption.
    [[nodiscard]] std::uint32_t check_address() const noexcept {
        return Arm9 ? hw_.cp15.dtcm_base() + 0x3ff8U : 0x0380'fff8U;
    }

    // IntrWait : r0 = 1 oublie les drapeaux déjà levés ; r1 = interruptions
    // attendues. IME passe à 1. On s'arrête, et la même SWI se refait au
    // retour de chaque IRQ, jusqu'au drapeau attendu. L'ARM9 attend toujours
    // au moins une IRQ, même si le drapeau est déjà levé (bogue de son BIOS).
    void wait_for_interrupt() {
        auto& r = cpu_.regs.r;
        (Arm9 ? hw_.irq9 : hw_.irq7).set_master(true);
        const auto address = check_address();
        auto flags = static_cast<std::uint32_t>(bus_.read32(address));
        if (!waiting_) {
            if (r[0] != 0) {
                bus_.write32(address, flags & ~r[1]);
                flags &= ~r[1];
            }
            if (!Arm9 && (flags & r[1]) != 0) {
                bus_.write32(address, flags & ~r[1]);
                return;
            }
            waiting_ = true;
        } else if ((flags & r[1]) != 0) {
            bus_.write32(address, flags & ~r[1]);
            waiting_ = false;
            return;
        }
        halt();
        cpu_.repeat_instruction();
    }

    // Div : r0 / r1 → r0 quotient, r1 reste, r3 valeur absolue du quotient.
    // Le BIOS boucle sans fin sur une division par zéro ; ici, rien ne change.
    void divide() noexcept {
        auto& r = cpu_.regs.r;
        const auto numerator = static_cast<std::int32_t>(r[0]);
        const auto denominator = static_cast<std::int32_t>(r[1]);
        if (denominator == 0) return;
        std::int32_t quotient = numerator;
        std::int32_t remainder = 0;
        if (numerator != std::numeric_limits<std::int32_t>::min() || denominator != -1) {
            quotient = numerator / denominator;
            remainder = numerator % denominator;
        }
        r[0] = static_cast<std::uint32_t>(quotient);
        r[1] = static_cast<std::uint32_t>(remainder);
        r[3] = quotient < 0 ? 0U - r[0] : r[0];
    }

    // CpuSet (mots ou demi-mots) et CpuFastSet (mots) : r0 source, r1
    // destination, r2 bits 0-20 nombre, bit 24 remplissage, bit 26 mots.
    // Le BIOS de l'ARM7 refuse une source dans sa propre zone.
    void copy_or_fill(bool fast) {
        auto& r = cpu_.regs.r;
        const auto count = r[2] & 0x1f'ffffU;
        const auto fill = (r[2] & (1U << 24U)) != 0;
        const auto words = fast || (r[2] & (1U << 26U)) != 0;
        const auto width = words ? 4U : 2U;
        auto source = r[0] & ~(width - 1U);
        auto destination = r[1] & ~(width - 1U);
        if (!Arm9 && source < 0x4000U) return;
        const auto first = words ? static_cast<std::uint32_t>(bus_.read32(source)) : static_cast<std::uint32_t>(bus_.read16(source));
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t value = first;
            if (!fill && index != 0)
                value = words ? static_cast<std::uint32_t>(bus_.read32(source)) : static_cast<std::uint32_t>(bus_.read16(source));
            if (words) bus_.write32(destination, value);
            else bus_.write16(destination, value & 0xffffU);
            if (!fill) source += width;
            destination += width;
        }
    }

    // GetCRC16 : CRC-16 réfléchi, polynôme 0xA001 (GBATEK, « BIOS Misc
    // Functions »), sur r2 octets depuis r1.
    std::uint32_t crc16(std::uint32_t initial, std::uint32_t address, std::uint32_t length) {
        auto crc = initial & 0xffffU;
        for (std::uint32_t index = 0; index < (length & ~1U); ++index) {
            crc ^= static_cast<std::uint32_t>(bus_.read8(address + index)) & 0xffU;
            for (unsigned bit = 0; bit < 8; ++bit) crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xa001U : crc >> 1U;
        }
        return crc;
    }

    Hardware& hw_;
    CpuType& cpu_;
    BusType& bus_;
    bool waiting_{};
};

} // namespace ravenemu::nds
