#pragma once

#include "sound/adpcm.hpp"

#include <cstdint>

// Un canal du son du DS (GBATEK, « DS Sound Channels 0..15 », « DS Sound
// Notes ») : SOUNDxCNT (+0), SOUNDxSAD (+4), SOUNDxTMR (+8), SOUNDxPNT (+0Ah),
// SOUNDxLEN (+0Ch) ; seul SOUNDxCNT se relit.
// - Formats : PCM8 (valeur × 100h), PCM16, IMA-ADPCM (adpcm.hpp), PSG à
//   rapport cyclique (canaux 8 à 13 : N + 1 huitièmes à +7FFFh, le reste à
//   -7FFFh ; 7 = toujours bas ; la période commence par le bas) et bruit
//   (canaux 14 et 15 : X = X >> 1, bas et X ^= 6000h si la retenue sort,
//   sinon haut ; X = 7FFFh au départ). Le format 3 des canaux 0 à 7 est muet.
// - Le minuteur compte à 16,76 MHz depuis SOUNDxTMR : un échantillon par
//   débordement. Départ (bit 31 passé à 1) retardé de 1 échantillon (PSG,
//   bruit), 3 (PCM) ou 11 (ADPCM, dont 8 pour l'en-tête) ; la sortie y vaut
//   0, sauf au premier avec Hold, qui garde l'ancien niveau.
// - Longueur PNT + LEN mots ; boucle (mode 1) : retour à PNT, et pour
//   l'ADPCM, la valeur et l'index retrouvés tels qu'au premier passage ;
//   coup unique (mode 2) : bit 31 retombé au dernier échantillon, gardé
//   jusqu'à la fin de sa période, ou indéfiniment avec Hold (bit 15) ; mode
//   manuel (0) : la lecture continue au-delà, choix de RavenCore (GBATEK ne
//   le décrit pas).
// - Arrêté (bit 31 à 0), le canal est muet, sauf avec Hold : GBATEK garde
//   le dernier échantillon jusqu'à l'effacement du drapeau.
namespace ravenemu::nds {

// Lecture des échantillons par le bus de l'ARM7, donnée par la machine.
struct SampleSource {
    std::uint32_t (*read)(void* context, std::uint32_t address, unsigned width){};
    void* context{};
    [[nodiscard]] std::uint32_t operator()(std::uint32_t address, unsigned width) const {
        return read != nullptr ? read(context, address, width) : 0U;
    }
};

class SoundChannel {
public:
    explicit SoundChannel(unsigned index) noexcept : index_(index) {}

    [[nodiscard]] std::uint32_t control() const noexcept { return control_; }
    [[nodiscard]] bool enabled() const noexcept { return (control_ & 0x8000'0000U) != 0; }
    // SOUNDxTMR, partagé avec la capture pour les canaux 1 et 3.
    [[nodiscard]] std::uint16_t timer() const noexcept { return timer_; }
    // `offset` de 0 à 0Fh, mots de 32 bits.
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask) noexcept;
    // Avance de `ticks` coups de 16,76 MHz.
    void advance(std::uint32_t ticks, const SampleSource& source);
    // Échantillon de sortie, sur 16 bits signés.
    [[nodiscard]] std::int32_t sample() const noexcept { return output_; }

private:
    [[nodiscard]] unsigned format() const noexcept { return (control_ >> 29U) & 3U; }
    [[nodiscard]] std::uint32_t end() const noexcept;  // fin, en unités de position
    [[nodiscard]] std::uint32_t loop_start() const noexcept;
    void start() noexcept;
    void stop() noexcept;
    void next_sample(const SampleSource& source);
    [[nodiscard]] std::int32_t fetch(const SampleSource& source);
    void reach_end() noexcept;

    unsigned index_;
    std::uint32_t control_{};
    std::uint32_t source_{};  // SOUNDxSAD
    std::uint16_t timer_{};   // SOUNDxTMR
    std::uint16_t loop_{};    // SOUNDxPNT, en mots
    std::uint32_t length_{};  // SOUNDxLEN, en mots
    // Lecture : position en octets (PCM) ou en quartets (ADPCM), depuis SAD.
    std::uint32_t counter_{};
    std::uint32_t position_{};
    unsigned delay_{};
    unsigned start_delay_{};
    bool playing_{};
    bool finished_{};  // dernier échantillon d'un coup unique en cours
    std::int32_t output_{};
    adpcm::State adpcm_{};
    adpcm::State loop_state_{};
    bool loop_saved_{};
    unsigned step_{};  // PSG : position dans la période de 8
    std::uint32_t noise_{0x7fff};
};

} // namespace ravenemu::nds
