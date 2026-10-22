#include <ravenemu/nds/machine.hpp>
#include "check.hpp"
#include "support/program_fixture.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

using namespace ravenemu::nds;
using namespace ravenemu::nds::test_support;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {
constexpr auto arm7 = Processor::arm7, arm9 = Processor::arm9;
constexpr std::uint32_t base = 0x04000400, enabled = 0x8000007fU;
struct Fixture {
    Audio audio;
    std::array<std::uint8_t,256> bytes{};
    static std::uint8_t read(const void* context, std::uint32_t address) noexcept {
        return static_cast<const Fixture*>(context)->bytes[address % 256];
    }
    void write(unsigned offset, std::uint32_t value, unsigned width = 4) {
        for (unsigned b = 0; b < width; ++b) audio.write_byte(offset + b,static_cast<std::uint8_t>(value >> (b * 8U)));
    }
    void start(unsigned channel, unsigned format, unsigned repeat = 1, unsigned pan = 0, unsigned extra = 0) {
        const auto offset = channel * 16;
        write(0x100,0x807f); write(offset + 8,0xfe00,2); // une période = 1024 cycles ARM7
        write(offset + 10,0,2); write(offset + 12,4);
        write(offset,enabled | (format << 29U) | (repeat << 27U) | (pan << 16U) | extra);
    }
    void advance(std::uint64_t cycles) { audio.advance_cycles(cycles,read,this); }
    std::vector<std::int16_t> drain() {
        std::vector<std::int16_t> out(Audio::buffer_frames * 2);
        out.resize(audio.read_samples(out)); return out;
    }
};
int quantize(int sample) { return (sample >> 6) * 64; }

void registers_and_bus() {
    Memory m;
    m.write32(arm7,base,0xffffffffU);
    check(m.read32(arm7,base) == 0xff7f837fU,"masque SOUNDxCNT");
    for (unsigned offset : {4U,8U,12U}) {
        m.write32(arm7,base + offset,0xffffffffU);
        check(m.read32(arm7,base + offset) == 0,"registre audio en écriture seule");
    }
    m.write32(arm7,base + 0x100,0xffffffffU);
    m.write32(arm7,base + 0x104,0xffffffffU);
    check(m.read32(arm7,base + 0x100) == 0xbf7f && m.read32(arm7,base + 0x104) == 1023,"masques contrôle/bias");
    expect_failure<ravenemu::Error>([&] { m.write32(arm9,base,enabled); },"ARM9 accède au son ARM7");
    expect_failure<ravenemu::Error>([&] { m.write32(arm7,base + 1,enabled); },"écriture audio non alignée");
    m.clear_ram();
    check(m.read32(arm7,base) == 0 && m.read16(arm7,base + 0x104) == 512,"reset audio");
    m.write32(arm9,0x02001000,0x40404040);
    for (unsigned i = 1; i < 4; ++i) m.write32(arm9,0x02001000 + i * 4,0x40404040);
    m.write32(arm7,base + 4,0x02401003); // alignement + miroir de RAM
    m.write32(arm7,base + 8,0xfe00); m.write32(arm7,base + 12,4);
    m.write16(arm7,base + 0x100,0x807f); m.write32(arm7,base,enabled | 0x08000000U);
    m.advance_cycles(10000);
    std::array<std::int16_t,32> output{}; const auto count = m.read_audio(output);
    check(count == 18 && output[6] == 16384 && output[7] == 0,"lecture PCM en RAM partagée et panoramique gauche");
    // La source suit le bus ARM7 ; déplacer une copie de Memory ne garde aucun pointeur vers l'original.
    auto copy = m; m.clear_ram(); copy.advance_cycles(2048);
    check(copy.read_audio(output) == 4 && output[0] == 16384,"copie du bus audio");
    copy.write32(arm7,0x04000304,0xfffffffeU);
    check(copy.read32(arm7,0x04000304) == 2 && copy.read32(arm9,0x04000304) == 0,"POWCNT2 masque/séparation ARM9");
    copy.advance_cycles(2048); check(copy.read_audio(output) == 4 && output[0] == 0,"haut-parleurs non coupés");
    check(copy.read32(arm7,base) & 0x80000000U,"POWCNT2 arrête le générateur");
    copy.write8(arm7,0x04000304,1); copy.advance_cycles(2048);
    check(copy.read_audio(output) == 4 && output[0] == 16384,"réactivation des haut-parleurs");
    for (const auto source : {0x03801000U,0x03001000U,0x06001000U}) {
        m.clear_ram(); m.set_wram_control(3);
        m.write8(arm9,0x04000242,0x82); // VRAM C attribuée à ARM7
        for (unsigned i = 0; i < 4; ++i) m.write32(arm7,source + i * 4,0x80808080U);
        m.write32(arm7,base + 4,source); m.write32(arm7,base + 8,0xfe00);
        m.write32(arm7,base + 12,4); m.write16(arm7,base + 0x100,0x807f);
        m.write32(arm7,base,enabled | 0x08000000U); m.advance_cycles(6000);
        check(m.read_audio(output) == 10 && output[6] == -32768,"source WRAM/VRAM audio ARM7");
    }
}

void pcm_and_stop() {
    Fixture f;
    for (unsigned i = 0; i < 16; ++i) f.bytes[i] = static_cast<std::uint8_t>(0x70 + i * 3);
    f.start(0,0,2); f.advance(20 * 1024);
    const auto out = f.drain();
    check(out.size() == 40,"cadence PCM");
    for (unsigned i = 0; i < 3; ++i) check(out[i * 2] == 0,"PCM démarre avant trois périodes");
    for (unsigned i = 0; i < 16; ++i) {
        const auto byte = f.bytes[i]; const auto signed_value = byte < 128 ? int{byte} : int{byte} - 256;
        check(out[(i + 3) * 2] == signed_value * 256 && out[(i + 3) * 2 + 1] == 0,"décodage PCM8 signé");
    }
    check(!(f.audio.read_byte(3) & 0x80) && out[38] == 0,"fin one-shot ou dernière période perdue");
    f.audio.reset(); f.start(0,0,2,127,0x8000); f.advance(20 * 1024);
    auto held = f.drain(); check(held[38] == 0 && held[39] == (int{f.bytes[15]} - 256) * 256,"hold ou panoramique droit");
    f.write(1,0,1); f.advance(1024); check(f.drain()[1] == 0,"hold non relâché");
    f.audio.reset();
    for (unsigned i = 0; i < 8; ++i) put16(f.bytes,i * 2,static_cast<std::uint16_t>(0x8000U + i * 0x1000U));
    f.start(5,1,1); f.write(5 * 16 + 10,1,2); f.write(5 * 16 + 12,3);
    f.advance(18 * 1024); const auto pcm16 = f.drain();
    for (unsigned i = 0; i < 15; ++i) {
        const auto position = i < 8 ? i : 2 + (i - 8) % 6;
        check(pcm16[(i + 3) * 2] == -32768 + static_cast<int>(position) * 4096,"PCM16 little-endian ou boucle PNT/LEN");
    }
    check(f.audio.read_byte(5 * 16 + 3) & 0x80,"boucle arrêtée");
    f.write(5 * 16,0); f.advance(1024); check(f.drain()[0] == 0,"arrêt logiciel");
    f.audio.reset(); f.start(0,0,0); f.advance(24 * 1024);
    check(f.audio.read_byte(3) & 0x80,"mode manuel arrêté par LEN");
    f.audio.reset(); f.write(0x100,0x807f); f.write(0,enabled | 0x08000000U);
    f.advance(400000); const auto short_buffer = f.drain();
    check((f.audio.read_byte(3) & 0x80) && std::ranges::all_of(short_buffer,[](auto s) { return s == 0; }),"buffer trop court");
}

void adpcm() {
    Fixture f;
    put32(f.bytes,0,0x00100000); // index 16, prédicteur zéro
    constexpr std::array<std::uint8_t,12> data{0x21,0x43,0x87,0xf0,0x55,0xaa,0x18,0x72,0xc3,0x4e,0x96,0x0f};
    std::copy(data.begin(),data.end(),f.bytes.begin() + 4);
    // Vecteur calculé séparément à partir des incréments IMA, puis arrondi DAC.
    constexpr std::array<int,24> expected{12,30,54,82,138,130,137,36,197,434,277,134,
                                        108,178,285,578,872,527,-75,665,1958,1430,-973,-630};
    f.start(0,2); f.write(10,1,2); f.write(12,3); f.advance(60 * 1024);
    const auto out = f.drain();
    for (unsigned i = 0; i < 11; ++i) check(out[i * 2] == 0,"ADPCM démarre avant en-tête/délai");
    for (unsigned i = 0; i < 49; ++i)
        check(out[(i + 11) * 2] == quantize(expected[i % expected.size()]),"vecteur ADPCM ou restauration de boucle");
    f.audio.reset(); put32(f.bytes,0,0x007f7fff); std::fill(f.bytes.begin() + 4,f.bytes.end(),0x77);
    f.start(0,2,2); f.advance(14 * 1024);
    check(f.drain()[22] == 32704,"ADPCM index invalide ou saturation positive");
    f.audio.reset(); put32(f.bytes,0,0x00588000); std::fill(f.bytes.begin() + 4,f.bytes.end(),0xff);
    f.start(0,2,2); f.advance(14 * 1024);
    check(f.drain()[22] == -32768,"ADPCM saturation négative");
}

void psg_noise_and_mixer() {
    for (unsigned duty = 0; duty < 8; ++duty) {
        Fixture f; f.start(8,3,2,0,duty << 24U); f.advance(18 * 1024); const auto out = f.drain();
        check(out[0] == 0,"PSG sans délai");
        for (unsigned i = 0; i < 17; ++i)
            check(out[(i + 1) * 2] == quantize(duty != 7 && i % 8 >= 7 - duty ? 32767 : -32767),"duty PSG");
        check(f.audio.read_byte(8 * 16 + 3) & 0x80,"PSG affecté par one-shot");
    }
    Fixture f; f.start(14,3); f.advance(32 * 1024); const auto noise = f.drain();
    // Les quatorze premières sorties sont basses, puis quatorze hautes.
    for (unsigned i = 1; i <= 14; ++i) check(noise[i * 2] == -32768,"amorçage LFSR bruit");
    check(noise[30] == 32704 && noise[32] == 32704,"rétroaction LFSR bruit");
    f.audio.reset(); f.bytes.fill(0x40); f.start(1,0,1,64); f.advance(6000); f.drain();
    f.write(0x100,0x907f); f.advance(1024); check(f.drain()[0] == 0,"exclusion canal 1 du mélangeur");
    f.write(0x100,0x957f); f.advance(1024); const auto bypass = f.drain();
    check(bypass[0] == 8192 && bypass[1] == 8192,"sélection directe canal 1");
    f.write(0x100,0x803f); f.advance(1024); check(f.drain()[0] == 4032,"volume maître 63/128");
    f.write(0x100,0); f.advance(1024); check(f.drain()[0] == 0,"master disable");
    f.write(0x104,0x3ff); f.advance(1024); check(f.drain()[0] == 32704,"BIAS indépendant du master");
    f.audio.reset(); f.bytes.fill(0x7f);
    for (unsigned n = 0; n < 16; ++n) f.start(n,0);
    f.advance(6000); check(f.drain()[6] == 32704,"saturation mélange de 16 canaux");
}

void clock_queue_reset() {
    Fixture large, sliced;
    large.bytes.fill(0x55); sliced.bytes = large.bytes;
    large.start(0,0); sliced.start(0,0); large.start(15,3); sliced.start(15,3);
    large.advance(100003);
    for (unsigned i = 0; i < 100003; ++i) sliced.advance(1);
    check(large.drain() == sliced.drain(),"audio dépend du découpage des cycles");
    Fixture silent;
    std::size_t count = 0;
    for (unsigned i = 0; i < 100; ++i) {
        silent.advance(timer_clock_hz / 100); count += silent.drain().size();
    }
    silent.advance(timer_clock_hz % 100); count += silent.drain().size();
    check(count == Audio::sample_rate * 2,"dérive de fréquence audio");
    silent.advance(std::numeric_limits<std::uint64_t>::max());
    std::array<std::int16_t,3> odd{1,2,3};
    check(silent.audio.read_samples(odd) == 2 && odd[2] == 3,"lecture impaire coupe une trame");
    check(silent.drain().size() == Audio::buffer_frames * 2 - 2,"file audio non bornée");
    silent.audio.reset(); check(silent.drain().empty(),"reset garde du son ancien");
    auto machine = Machine{}; machine.load_cartridge(Cartridge{cartridge_image()});
    machine.memory().advance_cycles(10000); machine.reset();
    check(machine.memory().read_audio(odd) == 0,"reset machine garde la file audio");
}

void live_controls() {
    Fixture f; f.bytes.fill(0x40); f.start(0,0);
    f.advance(1023); f.drain();
    f.write(8,0xfc00,2); // recharge future = 2048, échéance courante encore à 1024
    f.advance(5120 - 1023); auto out = f.drain();
    check(std::ranges::all_of(out,[](auto s) { return s == 0; }),"écriture TMR réinitialise la période courante");
    f.advance(1024); check(f.drain()[0] == 16384,"recharge TMR non appliquée");
    // Modifier les bits bas de CNT ne doit ni réamorcer le canal ni ajouter un délai.
    for (unsigned divisor = 0; divisor < 4; ++divisor) {
        f.write(1,divisor,1); f.advance(1024);
        check(f.drain()[0] == 16384 / (divisor == 3 ? 16 : 1 << divisor),"diviseur de volume ou redémarrage intempestif");
    }
    f.write(0,enabled | 0x08000000U); f.advance(1024); check(f.drain()[0] == 16384,"Start réécrit à un redémarre");
    f.write(0,0); f.write(8,0xfe00,2); f.write(0,enabled | 0x08000000U); f.advance(2048);
    out = f.drain(); check(std::ranges::all_of(out,[](auto s) { return s == 0; }),"redémarrage sans délai PCM");
    f.audio.reset(); f.start(1,0,1,0); f.start(3,0,1,127);
    f.write(0x100,0xbf7f); f.advance(6000); out = f.drain();
    check(out[6] == 16384 && out[7] == 16384,"somme directe canaux 1/3");
    f.write(0x100,0xba7f); f.advance(1024); out = f.drain();
    check(out[0] == 0 && out[1] == 16384,"sélection canal 3 indépendante des exclusions du mélangeur");
}
} // namespace

int main() {
    registers_and_bus(); pcm_and_stop(); adpcm(); psg_noise_and_mixer(); clock_queue_reset(); live_controls();
}
