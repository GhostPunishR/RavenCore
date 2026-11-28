#pragma once

#include "machine/scheduler.hpp"
#include "sound/capture_unit.hpp"
#include "sound/sound_channel.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Son du DS (GBATEK, « DS Sound Control Registers », « DS Sound Block
// Diagrams », « DS Sound Notes ») : 16 canaux (0x04000400 + 10h × n),
// SOUNDCNT (0x04000500), SOUNDBIAS (0x04000504) et les deux captures
// (0x04000508 à 0x0400051F, capture_unit.hpp), sur l'ARM7.
// Mixage aux largeurs de bits de GBATEK, pour chaque côté :
//   canal : PCM16 << 4 >> diviseur (0, 1, 2 ou 4), × volume, × panoramique,
//   >> 10 (arrondi vers le bas) ; somme des canaux ; × volume maître / 128 /
//   64 en ne gardant que la partie entière ; + biais ; bornes 0 à 3FFh.
// Les réglages 0 à 126 et 127 valent 0 à 126 et 128. SOUNDCNT bits 8-11
// choisissent la sortie (mixeur, canal 1, canal 3, ou les deux), bits 12-13
// retirent les canaux 1 et 3 du mixeur ; le maître coupé (bit 15), seul le
// biais sort.
// Addition (SNDCAPxCNT bits 0 et 7, « ch(a)+ch(b) Channel Addition ») : le
// canal b (1 ou 3) s'ajoute au canal a (0 ou 2) avant le panoramique, et ne
// sort plus seul ; vers le mixeur, b est retiré si SOUNDCNT l'exclut, et la
// somme se tait si a est arrêté.
// Entrées de la capture x : le mixeur gauche ou droit, borné à ±8000h ; ou le
// canal a après son volume, avec les bogues de GBATEK (« Capture Bugs ») :
// sans addition, -8000h si a et b sont tous deux négatifs ; avec addition,
// a + b (b seul si a est arrêté) sur 16 bits, sans borne.
// Sortie : un échantillon stéréo toutes les 1024 périodes système, soit
// 32 728,5 Hz (le DS module à 32,768 kHz), converti en 16 bits signés
// autour de 200h : (sortie - 200h) << 6. Les canaux et le mixeur sont lus au
// début de chaque échantillon de sortie, sans interpolation ; les captures
// gardent cette valeur jusqu'au suivant.
namespace ravenemu::nds {

class Sound {
public:
    static constexpr Cycles cycles_per_sample = 1024;
    static constexpr std::size_t buffer_limit = 2 * 65536;  // au-delà, les plus récents sont perdus

    Sound();

    void set_source(SampleSource source) noexcept { source_ = source; }
    void set_sink(SampleSink sink) noexcept { sink_ = sink; }
    // Mots d'E/S de 0x04000400 à 0x0400051F, `offset` depuis 0x04000400.
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, Cycles now);
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now);
    // Produit les échantillons dus jusqu'à `now`.
    void run(Cycles now);
    // Biais à 0 ou à 200h, d'un coup.
    void set_bias_level(bool high) noexcept;
    // SoundBias (SWI 08h) : le biais monte ou descend d'un pas toutes les
    // `step` périodes système, depuis `now`, jusqu'à 0 ou 200h. Rend la durée
    // de la rampe.
    Cycles ramp_bias(bool high, Cycles step, Cycles now);

    // Échantillons produits, gauche puis droite ; l'hôte les retire.
    [[nodiscard]] std::vector<std::int16_t>& output() noexcept { return output_; }

private:
    struct BiasRamp {
        std::uint32_t from{};
        std::uint32_t to{};
        Cycles start{};
        Cycles step{1};
        bool active{};
    };

    void mix();
    void add_channels(std::array<std::int64_t, 16>& volume);
    [[nodiscard]] std::uint32_t bias(Cycles now) noexcept;

    std::array<SoundChannel, 16> channels_;
    std::array<CaptureUnit, 2> captures_{};
    std::array<CaptureInput, 2> capture_input_{};
    std::uint32_t control_{};  // SOUNDCNT
    std::uint32_t bias_{};     // SOUNDBIAS
    BiasRamp ramp_{};
    Cycles time_{};
    SampleSource source_{};
    SampleSink sink_{};
    std::vector<std::int16_t> output_;
};

} // namespace ravenemu::nds
