#include "sound/adpcm.hpp"
#include "sound/sound.hpp"

#include "check.hpp"

#include <cstdint>
#include <vector>

// Son du DS, d'après GBATEK (« DS Sound Channels 0..15 », « DS Sound Control
// Registers », « DS Sound Notes »). Les canaux lisent une mémoire de test ;
// SOUNDxTMR = FE00h donne un échantillon par échantillon de sortie.
namespace {

using namespace ravenemu::nds;
using ravenemu::testing::check;

constexpr std::uint32_t all = 0xffff'ffffU;
constexpr std::uint32_t one_per_output = 0xfe00U;
constexpr std::int16_t silence = 0;      // biais 200h
constexpr std::int16_t high = 0x7fc0;    // +7FFFh, volume et maître au plus
constexpr std::int16_t low = -0x8000;    // -7FFFh

struct Bench {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x1000);
    Sound sound;
    Cycles now{};

    Bench() {
        sound.set_source({[](void* context, std::uint32_t address, unsigned width) {
                              const auto& bytes = static_cast<Bench*>(context)->memory;
                              std::uint32_t value = 0;
                              for (unsigned byte = 0; byte < width; ++byte) value |= std::uint32_t{bytes[(address + byte) & 0xfffU]} << (8U * byte);
                              return value;
                          },
                          this});
        sound.write(0x104, 0x200, all, 0);   // SOUNDBIAS
        sound.write(0x100, 0x807f, all, 0);  // SOUNDCNT : maître au plus
    }
    void channel(unsigned index, std::uint32_t control, std::uint32_t source = 0, std::uint32_t timer = one_per_output,
                 std::uint32_t loop = 0, std::uint32_t length = 0) {
        const auto base = index * 16U;
        sound.write(base + 4, source, all, now);
        sound.write(base + 8, timer | (loop << 16U), all, now);
        sound.write(base + 12, length, all, now);
        sound.write(base, control, all, now);
    }
    // Les `count` échantillons de sortie suivants, côté gauche ou droit.
    std::vector<std::int16_t> run(unsigned count, unsigned side = 0) {
        sound.output().clear();
        now += count * Sound::cycles_per_sample;
        sound.run(now);
        std::vector<std::int16_t> samples;
        for (std::size_t index = side; index < sound.output().size(); index += 2) samples.push_back(sound.output()[index]);
        return samples;
    }
    void put16(std::uint32_t at, std::uint16_t value) {
        memory[at] = static_cast<std::uint8_t>(value);
        memory[at + 1] = static_cast<std::uint8_t>(value >> 8U);
    }
};

// SOUNDxCNT : départ, format (29-30), rapport (24-26), panoramique (16-22),
// Hold (15), diviseur (8-9), volume (0-6) ; mode en bits 27-28.
constexpr std::uint32_t start = 0x8000'0000U;
constexpr std::uint32_t loop_mode = 1U << 27U;
constexpr std::uint32_t one_shot = 2U << 27U;
constexpr std::uint32_t format(unsigned value) { return value << 29U; }
constexpr std::uint32_t full_left = 0x7fU;  // volume au plus, panoramique tout à gauche

void psg_test() {
    Bench bench;
    bench.channel(8, start | format(3) | (3U << 24U) | full_left);
    check(bench.run(11) == std::vector<std::int16_t>{silence, silence, low, low, low, low, high, high, high, high, low},
          "PSG à 50 % : départ après un échantillon, période commencée par le bas");
    check(bench.run(4, 1) == std::vector<std::int16_t>(4, silence), "panoramique tout à gauche : rien à droite");
    bench.channel(8, start | format(3) | (7U << 24U) | full_left);  // relancé : rapport 7, toujours bas
    bench.channel(9, format(3) | full_left);                       // arrêt d'un autre canal : sans effet
    const auto duty7 = bench.run(10);
    check(std::vector<std::int16_t>(duty7.begin() + 2, duty7.end()) == std::vector<std::int16_t>(8, low), "rapport 7 : toujours bas");
    Bench pcm_channel;
    pcm_channel.channel(3, start | format(3) | (3U << 24U) | full_left);
    check(pcm_channel.run(8) == std::vector<std::int16_t>(8, silence), "PSG sur un canal 0 à 7 : muet");
}

void noise_test() {
    Bench bench;
    bench.channel(14, start | format(3) | full_left);
    std::vector<std::int16_t> expected{silence, silence};
    std::uint32_t x = 0x7fff;
    for (int index = 0; index < 30; ++index) {  // formule de GBATEK
        const auto carry = (x & 1U) != 0;
        x >>= 1U;
        if (carry) x ^= 0x6000U;
        expected.push_back(carry ? low : high);
    }
    check(bench.run(32) == expected, "bruit : X >> 1, bas et X ^ 6000h si la retenue sort");
}

void pcm_test() {
    Bench bench;
    const std::uint8_t pcm8[] = {0x40, 0xc0, 0x7f, 0x80};
    for (unsigned word = 0; word < 4; ++word)
        for (unsigned byte = 0; byte < 4; ++byte) bench.memory[0x100 + word * 4 + byte] = pcm8[byte];
    bench.channel(0, start | one_shot | format(0) | full_left, 0x100, one_per_output, 0, 4);
    const auto samples = bench.run(22);
    check(samples[4] == 0x4000 && samples[5] == -0x4000 && samples[6] == 0x7f00 && samples[7] == low,
          "PCM8 : trois échantillons de retard, valeur × 100h");
    check(samples[19] == low && samples[20] == silence, "coup unique : le dernier échantillon tient sa période, puis 0");
    check((bench.sound.read(0, bench.now) & start) == 0, "coup unique : bit 31 retombé");
    bench.channel(0, start | one_shot | 0x8000U | format(0) | full_left, 0x100, one_per_output, 0, 4);
    const auto held = bench.run(24);
    check(held[22] == low && held[23] == low, "Hold : le dernier échantillon reste");

    Bench looped;
    for (unsigned index = 0; index < 8; ++index) looped.put16(0x200 + index * 2, static_cast<std::uint16_t>(0x1000 * (index + 1)));
    looped.channel(0, start | loop_mode | format(1) | full_left, 0x200, one_per_output, 2, 2);  // PNT 2 mots, LEN 2 mots
    const auto pcm16 = looped.run(20);
    std::vector<std::int16_t> expected;
    for (unsigned index : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 4U, 5U, 6U, 7U, 4U, 5U, 6U, 7U})
        expected.push_back(static_cast<std::int16_t>(0x1000 * (index + 1)));
    check(std::vector<std::int16_t>(pcm16.begin() + 4, pcm16.end()) == expected, "PCM16 en boucle : retour à PNT");
}

void adpcm_test() {
    check(adpcm::step_table[0] == 7 && adpcm::step_table[3] == 0xa && adpcm::step_table[4] == 0xb && adpcm::step_table[50] == 0x36c &&
              adpcm::step_table[88] == 0x7fff,
          "table des pas : celle de GBATEK, générée par sa formule");
    adpcm::State state{0x4000, 40};
    std::vector<std::int32_t> values;
    for (const unsigned nibble : {7U, 7U, 15U, 8U}) {
        adpcm::decode(state, nibble);
        values.push_back(state.pcm);
    }
    check(values == std::vector<std::int32_t>{17015, 18372, 15462, 15047} && state.index == 63, "décodage : pas, arrondis et index");
    adpcm::State top{0x7ff0, 88};
    adpcm::decode(top, 7);
    adpcm::State bottom{-0x7ff0, 88};
    adpcm::decode(bottom, 15);
    check(top.pcm == 0x7fff && bottom.pcm == -0x7fff, "bornes +7FFFh et -7FFFh");

    Bench bench;
    bench.put16(0x300, 0x4000);  // en-tête : 4000h, index 40
    bench.put16(0x302, 40);
    bench.memory[0x304] = 0x77;
    bench.memory[0x305] = 0x8f;
    bench.channel(0, start | one_shot | format(2) | full_left, 0x300, one_per_output, 1, 3);
    const auto samples = bench.run(16);
    check(samples[11] == silence && samples[12] == 16960 && samples[13] == 18368 && samples[14] == 15424 && samples[15] == 15040,
          "ADPCM : onze échantillons de retard, puis les valeurs décodées");

    // Boucle sur la partie données (PNT = 1 mot) : au retour, l'index et la
    // valeur reprennent ceux du début de la boucle, la suite se répète.
    Bench looped;
    looped.put16(0x300, 0x4000);
    looped.put16(0x302, 40);
    for (unsigned byte = 0; byte < 4; ++byte) looped.memory[0x304 + byte] = 0x77;
    looped.channel(0, start | loop_mode | format(2) | full_left, 0x300, one_per_output, 1, 1);
    const auto repeated = looped.run(28);
    check(std::vector<std::int16_t>(repeated.begin() + 12, repeated.begin() + 20) ==
              std::vector<std::int16_t>(repeated.begin() + 20, repeated.begin() + 28) &&
              repeated[12] != repeated[19],
          "ADPCM en boucle : état rechargé au début de la boucle");
}

void mixer_test() {
    Bench bench;
    for (unsigned index = 0; index < 8; ++index) bench.put16(0x400 + index * 2, 0x4000);
    // Largeurs de GBATEK : 4000h / 4 (diviseur 2) × 64/128 (volume) × 64/128
    // (panoramique) = 400h, puis × 128/128/64 (maître) = 10h au-dessus du biais.
    bench.channel(4, start | loop_mode | format(1) | (64U << 16U) | (2U << 8U) | 64U, 0x400, one_per_output, 0, 4);
    const auto left = bench.run(8);
    check(left[6] == 0x10 * 64, "volume, diviseur, panoramique et maître : 10h au-dessus du biais");
    check(bench.run(4, 1)[3] == 0x10 * 64, "panoramique au milieu : même niveau à droite");
    bench.channel(1, start | loop_mode | format(1) | full_left, 0x400, one_per_output, 0, 4);
    bench.sound.write(0x100, 0x817f, all, bench.now);  // sortie gauche : le canal 1 seul
    const auto direct = bench.run(8);
    check(direct[7] == 0x4000, "sortie gauche prise au canal 1 seul (SOUNDCNT bits 8-9)");
    bench.sound.write(0x100, 0x007f, all, bench.now);  // maître coupé
    check(bench.run(4)[3] == silence, "maître coupé : seul le biais sort");
    bench.sound.set_bias_level(false);
    check(bench.run(4)[3] == low, "SoundBias à 0 : la sortie tombe à 000h");
}

} // namespace

int main() {
    psg_test();
    noise_test();
    pcm_test();
    adpcm_test();
    mixer_test();
    return 0;
}
