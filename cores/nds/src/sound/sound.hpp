#pragma once

#include "machine/scheduler.hpp"
#include "sound/sound_channel.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Son du DS (GBATEK, « DS Sound Control Registers », « DS Sound Block
// Diagrams », « DS Sound Notes ») : 16 canaux (0x04000400 + 10h × n),
// SOUNDCNT (0x04000500) et SOUNDBIAS (0x04000504), sur l'ARM7.
// Mixage aux largeurs de bits de GBATEK, pour chaque côté :
//   canal : PCM16 << 4 >> diviseur (0, 1, 2 ou 4), × volume, × panoramique,
//   >> 10 (arrondi vers le bas) ; somme des canaux ; × volume maître / 128 /
//   64 en ne gardant que la partie entière ; + biais ; bornes 0 à 3FFh.
// Les réglages 0 à 126 et 127 valent 0 à 126 et 128. SOUNDCNT bits 8-11
// choisissent la sortie (mixeur, canal 1, canal 3, ou les deux), bits 12-13
// retirent les canaux 1 et 3 du mixeur ; le maître coupé (bit 15), seul le
// biais sort.
// Sortie : un échantillon stéréo toutes les 1024 périodes système, soit
// 32 728,5 Hz (le DS module à 32,768 kHz), converti en 16 bits signés
// autour de 200h : (sortie - 200h) << 6. Les canaux sont lus au début de
// chaque échantillon de sortie, sans interpolation.
// Pas encore : capture (SNDCAPxCNT, 0x04000508), rampe de SoundBias.
namespace ravenemu::nds {

class Sound {
public:
    static constexpr Cycles cycles_per_sample = 1024;
    static constexpr std::size_t buffer_limit = 2 * 65536;  // au-delà, les plus récents sont perdus

    Sound();

    void set_source(SampleSource source) noexcept { source_ = source; }
    // Mots d'E/S de 0x04000400 à 0x0400051F, `offset` depuis 0x04000400.
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, Cycles now);
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now);
    // Produit les échantillons dus jusqu'à `now`.
    void run(Cycles now);
    // SoundBias (SWI 08h) : biais à 0 ou à 200h.
    void set_bias_level(bool high) noexcept { bias_ = (bias_ & ~0x3ffU) | (high ? 0x200U : 0U); }

    // Échantillons produits, gauche puis droite ; l'hôte les retire.
    [[nodiscard]] std::vector<std::int16_t>& output() noexcept { return output_; }

private:
    void mix();

    std::array<SoundChannel, 16> channels_;
    std::uint32_t control_{};  // SOUNDCNT
    std::uint32_t bias_{};     // SOUNDBIAS
    Cycles time_{};
    SampleSource source_{};
    std::vector<std::int16_t> output_;
};

} // namespace ravenemu::nds
