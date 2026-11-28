#pragma once

#include <cstdint>

// Capture du son du DS (GBATEK, « DS Sound Capture ») : SNDCAPxCNT
// (0x04000508 + x), SNDCAPxDAD (0x04000510 + 8x), SNDCAPxLEN (0x04000514 + 8x).
// - SNDCAPxCNT : bit 0 addition des canaux (avec le bit 7), bit 1 source
//   (0 = mixeur gauche ou droit, 1 = canal 0 ou 2), bit 2 coup unique, bit 3
//   PCM8, bit 7 départ et occupation.
// - La capture 0 suit le minuteur de SOUND1TMR, la 1 celui de SOUND3TMR :
//   un échantillon par débordement, écrit dès sa capture à DAD (le FIFO de
//   GBATEK n'est pas modélisé). LEN compte en mots, 0 valant 1 ; en boucle,
//   l'écriture reprend à DAD ; en coup unique, le bit 7 retombe à la fin.
// - Entier capturé (« Capture Clipping/Rounding ») : PCM8 divise la valeur
//   par 100h ; si le bit de poids fort de la partie fractionnaire est à 1,
//   la valeur est arrondie vers zéro ; la partie fractionnaire est ensuite
//   écartée.
// Les valeurs à capturer, et leurs bogues, viennent du mixeur (sound.cpp).
namespace ravenemu::nds {

// Écriture des échantillons capturés par le bus de l'ARM7, donnée par la machine.
struct SampleSink {
    void (*write)(void* context, std::uint32_t address, std::uint32_t value, unsigned width){};
    void* context{};
    void operator()(std::uint32_t address, std::uint32_t value, unsigned width) const {
        if (write != nullptr) write(context, address, value, width);
    }
};

// Valeur à capturer, en virgule fixe : `fraction` bits après la virgule.
struct CaptureInput {
    std::int64_t value{};
    unsigned fraction{};
};

class CaptureUnit {
public:
    [[nodiscard]] std::uint32_t control() const noexcept { return control_; }
    [[nodiscard]] std::uint32_t destination() const noexcept { return destination_; }
    [[nodiscard]] bool adds() const noexcept { return (control_ & 0x81U) == 0x81U; }
    [[nodiscard]] bool from_channel() const noexcept { return (control_ & 0x02U) != 0; }

    // Le départ (bit 7 passé à 1) recharge le compteur depuis `timer`.
    void write_control(std::uint32_t value, std::uint16_t timer) noexcept;
    void write_destination(std::uint32_t value, std::uint32_t mask) noexcept;
    void write_length(std::uint32_t value, std::uint32_t mask) noexcept;
    // Avance de `ticks` coups de 16,76 MHz au rythme de `timer`, en capturant
    // `input` à chaque débordement.
    void advance(std::uint32_t ticks, std::uint16_t timer, CaptureInput input, const SampleSink& sink);

    // Entier capturé, avec l'arrondi de GBATEK.
    [[nodiscard]] static std::int32_t round(CaptureInput input) noexcept;

private:
    void capture(CaptureInput input, const SampleSink& sink);

    std::uint32_t control_{};
    std::uint32_t destination_{};
    std::uint16_t length_{};
    std::uint32_t counter_{};
    std::uint32_t position_{};  // octets écrits depuis DAD
};

} // namespace ravenemu::nds
