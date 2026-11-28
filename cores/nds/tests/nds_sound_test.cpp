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
        sound.set_sink({[](void* context, std::uint32_t address, std::uint32_t value, unsigned width) {
                            auto& bytes = static_cast<Bench*>(context)->memory;
                            for (unsigned byte = 0; byte < width; ++byte)
                                bytes[(address + byte) & 0xfffU] = static_cast<std::uint8_t>(value >> (8U * byte));
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
    [[nodiscard]] std::uint16_t get16(std::uint32_t at) const {
        return static_cast<std::uint16_t>(memory[at] | (memory[at + 1] << 8U));
    }
    // Un canal en PCM16 qui boucle sur la valeur `value`, quatre mots à `at`.
    void constant(unsigned index, std::int16_t value, std::uint32_t control, std::uint32_t at) {
        for (unsigned sample = 0; sample < 8; ++sample) put16(at + sample * 2, static_cast<std::uint16_t>(value));
        channel(index, control | 0x8000'0000U | (1U << 27U) | (1U << 29U), at, one_per_output, 0, 4);
    }
    // Capture x : SNDCAPxLEN, SNDCAPxDAD, puis SNDCAPxCNT.
    void capture(unsigned index, std::uint32_t control, std::uint32_t destination, std::uint32_t length) {
        sound.write(0x114 + index * 8, length, all, now);
        sound.write(0x110 + index * 8, destination, all, now);
        sound.write(0x108, control << (8U * index), 0xffU << (8U * index), now);
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

// Un canal arrêté se tait, sauf avec Hold.
void stop_test() {
    Bench bench;
    bench.constant(0, 0x4000, full_left, 0x100);
    check(bench.run(8)[7] == 0x4000, "canal en cours");
    bench.sound.write(0, format(1) | full_left, all, bench.now);
    check(bench.run(2) == std::vector<std::int16_t>(2, silence) && (bench.sound.read(0, bench.now) & start) == 0, "arrêté : muet");
    bench.constant(0, 0x4000, full_left | 0x8000U, 0x100);
    bench.run(8);
    bench.sound.write(0, 0x8000U | format(1) | full_left, all, bench.now);
    check(bench.run(2) == std::vector<std::int16_t>(2, 0x4000), "arrêté avec Hold : le dernier échantillon reste");
    bench.sound.write(0, format(1) | full_left, all, bench.now);
    check(bench.run(2) == std::vector<std::int16_t>(2, silence), "Hold effacé : muet");
}

void capture_rounding_test() {
    check(CaptureUnit::round({0x4080, 8}) == 0x40 && CaptureUnit::round({-0x40, 8}) == 0 && CaptureUnit::round({-0x80, 8}) == 0 &&
              CaptureUnit::round({-0xc0, 8}) == -1 && CaptureUnit::round({-0x140, 8}) == -1 &&
              CaptureUnit::round({-0x1c0, 8}) == -2,
          "arrondi : vers zéro si le bit fort de la fraction est levé, sinon fraction écartée");
}

// Capture des mixeurs, en PCM16 puis en PCM8, au rythme de SOUND1TMR et
// SOUND3TMR.
void capture_mixer_test() {
    Bench bench;
    bench.sound.write(0x18, one_per_output, all, 0);  // SOUND1TMR : un échantillon par sortie
    check(bench.sound.read(0x108, 0) == 0, "captures arrêtées au départ");
    bench.memory[0x410] = 0xee;
    bench.capture(1, 0x80, 0x410, 1);
    bench.run(4);
    check(bench.memory[0x410] == 0xee, "capture 1 sans SOUND3TMR : rien avant 10000h coups");
    bench.capture(1, 0x00, 0x410, 1);
    bench.sound.write(0x38, one_per_output, all, 0);  // SOUND3TMR
    bench.constant(4, 0x3000, full_left, 0x100);
    bench.constant(5, 0x1000, full_left, 0x140);
    bench.constant(6, -0x7000, 0x7f'0000U | 0x7fU, 0x180);  // tout à droite
    bench.run(8);
    for (unsigned at = 0x400; at < 0x420; ++at) bench.memory[at] = 0xee;
    bench.capture(0, 0x80, 0x400, 2);  // boucle, PCM16, 2 mots
    bench.capture(1, 0x8c, 0x410, 1);  // coup unique, PCM8, 1 mot
    bench.run(12);
    check(bench.get16(0x400) == 0x4000 && bench.get16(0x406) == 0x4000 && bench.memory[0x408] == 0xee,
          "PCM16 du mixeur gauche, en boucle sur LEN mots");
    check(bench.memory[0x410] == 0x90 && bench.memory[0x413] == 0x90 && bench.memory[0x414] == 0xee &&
              (bench.sound.read(0x108, bench.now) & 0x8080U) == 0x80U,
          "PCM8 du mixeur droit, coup unique : 4 octets puis bit 7 retombé");
    bench.constant(7, -0x7000, 0x7f'0000U | 0x7fU, 0x1c0);
    bench.capture(1, 0x80, 0x410, 1);
    bench.run(12);
    check(bench.get16(0x410) == 0x8000, "mixeur borné à -8000h pour la capture");
}

// Chaque capture suit son minuteur : SOUND1TMR pour la 0, SOUND3TMR pour la 1.
void capture_timer_test() {
    Bench bench;
    bench.sound.write(0x18, 0xfe00, all, 0);  // un débordement par échantillon de sortie
    bench.sound.write(0x38, 0xff00, all, 0);  // deux
    for (unsigned at = 0x400; at < 0x600; ++at) bench.memory[at] = 0xee;
    bench.capture(0, 0x88, 0x400, 4);  // boucle, PCM8, 16 octets
    bench.capture(1, 0x88, 0x500, 4);
    bench.run(5);
    const auto written = [&](std::uint32_t at) {
        unsigned count = 0;
        for (unsigned byte = 0; byte < 16; ++byte) count += bench.memory[at + byte] != 0xee ? 1U : 0U;
        return count;
    };
    check(written(0x400) == 5 && written(0x500) == 10, "capture 0 au rythme de SOUND1TMR, capture 1 à celui de SOUND3TMR");
    bench.sound.write(0x118, all, all, bench.now);
    check(bench.sound.read(0x110, bench.now) == 0x400 && bench.sound.read(0x118, bench.now) == 0x07ff'fffcU &&
              bench.sound.read(0x114, bench.now) == 0,
          "SNDCAPxDAD relu (bits 2 à 26), SNDCAPxLEN en écriture seule");
}

// Capture du canal 0 : volume avant le panoramique, bogues de GBATEK, addition.
void capture_channel_test() {
    Bench bench;
    bench.sound.write(0x18, one_per_output, all, 0);
    bench.constant(0, 0x4000, 0x7f'0000U | 64U, 0x100);  // volume 64, tout à droite
    bench.capture(0, 0x82, 0x400, 1);
    bench.run(12);
    check(bench.get16(0x400) == 0x2000, "canal 0 après son volume, avant le panoramique");
    bench.constant(0, -0x1000, full_left, 0x100);
    bench.constant(1, -0x10, 0x7f'0000U | 0x7fU, 0x140);
    bench.run(12);
    check(bench.get16(0x400) == 0x8000, "bogue : -8000h si les canaux 0 et 1 sont tous deux négatifs");
    bench.constant(0, 0x1000, full_left, 0x100);
    bench.constant(1, 0x1000, 0x7f'0000U | 0x7fU, 0x140);
    bench.capture(0, 0x83, 0x400, 1);  // addition
    const auto right = bench.run(12, 1);
    check(bench.get16(0x400) == 0x2000 && right.back() == silence, "addition : 1 + 0 capturés, le canal 1 ne sort plus seul");
    check(bench.run(4).back() == 0x2000, "addition : la somme sort par le panoramique du canal 0");
    bench.sound.write(0x100, 0x907f, all, bench.now);  // canal 1 retiré du mixeur
    check(bench.run(12).back() == 0x1000 && bench.get16(0x400) == 0x2000, "canal 1 exclu : retiré du mixeur, pas de la capture");
    bench.sound.write(0x100, 0x807f, all, bench.now);
    bench.constant(0, 0x6000, full_left, 0x100);
    bench.constant(1, 0x6000, 0x7f'0000U | 0x7fU, 0x140);
    bench.run(12);
    check(bench.get16(0x400) == 0xc000, "bogue : la somme déborde sur 16 bits vers la capture");
    // Volume 126 : 6001h donne 5E80h et une fraction. La somme, BD01h et une
    // fraction de bit fort levé, devient négative en débordant, et s'arrondit
    // donc vers zéro : BD02h.
    bench.constant(0, 0x6001, 126U, 0x100);  // tout à gauche, volume 126
    bench.constant(1, 0x6001, 0x7f'0000U | 126U, 0x140);
    bench.run(12);
    check(bench.get16(0x400) == 0xbd02, "bogue : la somme débordée s'arrondit comme une valeur négative");
    bench.constant(0, 0x6000, full_left | 0x8000U, 0x100);
    bench.constant(1, 0x6000, 0x7f'0000U | 0x7fU, 0x140);
    bench.run(4);
    bench.sound.write(0, 0x8000U | format(1) | full_left, all, bench.now);  // canal 0 arrêté, sa sortie gardée par Hold
    const auto left = bench.run(12);
    check(bench.get16(0x400) == 0x6000 && left.back() == silence, "canal 0 arrêté : capture du canal 1 seul, sortie muette");
}

// SoundBias : un pas au départ, puis un par période, jusqu'au niveau visé.
void bias_ramp_test() {
    Bench bench;
    bench.sound.write(0x100, 0, all, 0);  // maître coupé : seul le biais sort
    check(bench.sound.ramp_bias(false, Sound::cycles_per_sample, bench.now) == 0x200 * Sound::cycles_per_sample,
          "durée : 200h pas");
    const auto samples = bench.run(3);
    check(samples == std::vector<std::int16_t>{-2 * 64, -3 * 64, -4 * 64}, "le biais descend d'un pas par période");
    check(bench.sound.read(0x104, bench.now) == 0x200 - 4, "SOUNDBIAS suit la rampe");
    bench.run(0x200);
    check(bench.sound.read(0x104, bench.now) == 0 && bench.sound.ramp_bias(false, 1, bench.now) == 0, "rampe finie : 000h");
    bench.sound.ramp_bias(true, 2 * Sound::cycles_per_sample, bench.now);
    bench.sound.write(0x104, 0x123, all, bench.now + 4 * Sound::cycles_per_sample);
    check(bench.sound.read(0x104, bench.now + 8 * Sound::cycles_per_sample) == 0x123, "une écriture de SOUNDBIAS arrête la rampe");
}

} // namespace

int main() {
    psg_test();
    noise_test();
    pcm_test();
    adpcm_test();
    mixer_test();
    stop_test();
    capture_rounding_test();
    capture_mixer_test();
    capture_timer_test();
    capture_channel_test();
    bias_ramp_test();
    return 0;
}
