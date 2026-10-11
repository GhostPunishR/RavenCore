#include "apu/apu.hpp"
#include "check.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace ravenemu::cgb::testing {

/**
 * Fait tourner l'APU comme le bus le ferait : un cycle à la fois, avec le front
 * descendant du compteur DIV tous les 8192 cycles, et relève ce qui en sort.
 */
class Runner {
public:
    explicit Runner(Apu& apu) : apu_(apu) {}

    /** Avance de [cycles] et rend le plus grand écart au silence rencontré. */
    [[nodiscard]] int run(int cycles) {
        for (int cycle = 0; cycle < cycles; ++cycle) {
            apu_.tick(1);
            if (++divider_ >= 8192) { divider_ = 0; apu_.clock_divider_falling_edge(); }
        }
        int peak = 0;
        std::array<std::int16_t, 4096> block{};
        while (true) {
            const auto count = apu_.read_samples(block);
            if (count == 0) break;
            for (std::size_t i = 0; i < count; ++i) {
                peak = std::max(peak, std::abs(static_cast<int>(block[i])));
            }
        }
        return peak;
    }

private:
    Apu& apu_;
    int divider_{};
};

/**
 * L'entrée en $0100 ne produit aucun son.
 *
 * La ROM d'amorçage déclenche le canal 1 pour son carillon pendant le
 * défilement du logo, avec une enveloppe décroissante de période trois : quinze
 * crans en sept dixièmes de seconde, quand le défilement en dure plusieurs. Le
 * carillon est donc éteint depuis longtemps quand le jeu prend la main.
 *
 * L'état HLE reproduisait le volume *initial* déclaré par NR12 au lieu du
 * volume atteint, et laissait le condensateur déchargé devant un convertisseur
 * qui repose déjà sur un niveau non nul. Il en sortait un créneau à pleine
 * amplitude pendant huit dixièmes de seconde, ouvert par une marche de tout un
 * quart de la dynamique : un bruit avant la musique du titre, à chaque partie.
 *
 * La vérification exige aussi que le canal reste **signalé allumé**, faute de
 * quoi elle serait satisfaite en éteignant simplement l'APU — ce que le
 * matériel ne fait pas, et ce qui casserait la lecture de NR52.
 */
void hle_post_boot_silence_test() {
    Apu apu(gb::HardwareMode::dmg);
    apu.initialize_hle_post_boot();
    check((apu.read(0xff26) & 0x01) != 0,
          "le canal 1 doit rester signalé allumé dans NR52 après l'amorçage");

    Runner runner(apu);
    int peak = 0;
    // Une seconde émulée, bien au-delà des sept dixièmes qu'aurait duré le
    // carillon fautif.
    for (int frame = 0; frame < 60; ++frame) peak = std::max(peak, runner.run(70'224));
    check(peak == 0, "l'entrée en $0100 doit être parfaitement silencieuse");
}

/**
 * Et le canal redevient audible dès que le jeu le demande.
 *
 * Sans cette moitié, la précédente serait satisfaite par un APU muet. Les deux
 * ensemble disent ce qu'on veut : silencieux tant que personne ne joue, sonore
 * dès la première note.
 */
void post_boot_channel_still_audible_test() {
    constexpr int frequency = 1750; // environ 440 Hz
    Apu apu(gb::HardwareMode::dmg);
    apu.initialize_hle_post_boot();
    Runner runner(apu);
    check(runner.run(70'224) == 0, "la trame qui précède la première note est muette");

    apu.write(0xff11, 0x80); // rapport cyclique de moitié
    apu.write(0xff12, 0xf0); // volume 15, sans enveloppe
    apu.write(0xff13, frequency & 0xff);
    apu.write(0xff14, 0x80 | ((frequency >> 8) & 7)); // déclenchement
    check(runner.run(70'224) > 1000,
          "le canal doit redevenir audible dès que le jeu le déclenche");
}

/**
 * L'initialisation audio type d'un jeu ne fait aucun bruit.
 *
 * Un jeu commence presque toujours par couper l'APU, le rallumer, poser ses
 * volumes et son aiguillage, puis allumer le convertisseur d'un canal — le
 * tout avant de jouer la moindre note.
 *
 * Rien de cette séquence ne doit s'entendre. Le niveau continu est le même
 * avant la coupure et après le rallumage, et la charge gardée pendant la
 * coupure le rejoint exactement : chaque moitié de la fenêtre de reconnexion
 * vaut zéro. Leur **moyenne**, elle, ne valait zéro que par accident, et
 * filtrée contre la charge elle sortait une impulsion d'un seul échantillon,
 * jusqu'à un sixième de la dynamique. C'était le tic entendu à l'ouverture
 * d'une partie, après que le carillon d'amorçage eut cessé d'être rejoué.
 *
 * Les avances ne sont volontairement pas des multiples de la fenêtre de cent
 * vingt-huit cycles : les écritures tombent ainsi au milieu d'une fenêtre,
 * ce qui est le cas courant et le seul qui révèle le défaut.
 */
void game_apu_init_is_silent_test() {
    Apu apu(gb::HardwareMode::dmg);
    apu.initialize_hle_post_boot();
    Runner runner(apu);
    int peak = runner.run(70'224);

    apu.write(0xff26, 0x00); // coupure de l'APU
    peak = std::max(peak, runner.run(7'000));
    apu.write(0xff26, 0x80); // rallumage
    peak = std::max(peak, runner.run(7'000));
    apu.write(0xff24, 0x77); // volumes maîtres
    apu.write(0xff25, 0xff); // aiguillage complet
    peak = std::max(peak, runner.run(7'000));
    apu.write(0xff12, 0xf0); // convertisseur du canal 1 allumé, volume 15
    peak = std::max(peak, runner.run(7'000));
    check(peak == 0, "l'initialisation audio d'un jeu doit être silencieuse");

    // Et la première note s'entend : le silence obtenu n'est pas celui d'un
    // APU resté muet.
    apu.write(0xff11, 0x80);
    apu.write(0xff13, 0xd6);
    apu.write(0xff14, 0x86); // déclenchement, environ 440 Hz
    check(runner.run(70'224) > 1000, "la première note doit s'entendre");
}

void hle_post_boot_register_test() {
    Apu apu(gb::HardwareMode::cgb_native);
    apu.initialize_hle_post_boot();
    check(apu.read(0xff10) == 0x80 && apu.read(0xff11) == 0xbf &&
          apu.read(0xff12) == 0xf3 && apu.read(0xff24) == 0x77 &&
          apu.read(0xff25) == 0xf3 && apu.read(0xff26) == 0xf1,
          "registres APU HLE post-boot différents de l'état observable à $0100");
}

void wave_ram_access_test() {
    Apu dmg(gb::HardwareMode::dmg);
    dmg.write(0xff30, 0x12);
    dmg.write(0xff1a, 0x80);
    dmg.write(0xff1d, 0xfe);
    dmg.write(0xff1e, 0x87); // période 2046, trigger
    check(dmg.read(0xff30) == 0xff,
          "wave RAM DMG accessible hors de la fenêtre de lecture du canal");
    dmg.tick(4);
    check(dmg.read(0xff3f) == 0x12,
          "accès wave RAM DMG actif non redirigé vers l'octet courant");
    dmg.write(0xff3f, 0x34);
    dmg.tick(2);
    check(dmg.read(0xff30) == 0xff,
          "fenêtre d'accès wave RAM DMG reste ouverte plus de deux cycles");
    dmg.write(0xff1a, 0x00);
    check(dmg.read(0xff30) == 0x34,
          "écriture redirigée dans l'octet wave RAM courant absente");

    Apu cgb(gb::HardwareMode::cgb_native);
    cgb.write(0xff30, 0x12);
    cgb.write(0xff1a, 0x80);
    cgb.write(0xff1d, 0xfe);
    cgb.write(0xff1e, 0x87);
    cgb.write(0xff3f, 0x56);
    check(cgb.read(0xff30) == 0x56 && cgb.read(0xff3f) == 0x56,
          "accès wave RAM CGB actif non redirigé vers l'octet courant");
    cgb.write(0xff1a, 0x00);
    check(cgb.read(0xff30) == 0x56 && cgb.read(0xff3f) == 0x00,
          "accès wave RAM CGB non redevenu direct après arrêt du canal");
}

void sweep_negate_clear_test() {
    Apu apu(gb::HardwareMode::dmg);
    apu.write(0xff10, 0x09); // calcul négatif, shift 1
    apu.write(0xff11, 0x80);
    apu.write(0xff12, 0xf0);
    apu.write(0xff13, 0xe8); // fréquence 1000
    apu.write(0xff14, 0x83);
    check((apu.read(0xff26) & 1) != 0, "canal 1 non déclenché pour le test sweep");
    apu.write(0xff10, 0x01); // retire negate après un calcul négatif
    check((apu.read(0xff26) & 1) == 0,
          "retrait du bit negate après calcul sweep n'a pas coupé le canal 1");
}

void powered_off_length_counter_test() {
    Apu dmg(gb::HardwareMode::dmg);
    dmg.write(0xff26, 0x00);
    dmg.write(0xff11, 0x3f); // compteur longueur = 1, encore câblé sur DMG
    dmg.write(0xff26, 0x80);
    dmg.write(0xff12, 0xf0);
    dmg.write(0xff14, 0xc0);
    check((dmg.read(0xff26) & 1) != 0, "canal DMG non déclenché après écriture longueur NR52 off");
    dmg.clock_divider_falling_edge();
    check((dmg.read(0xff26) & 1) == 0,
          "compteur longueur écrit NR52 off sur DMG n'a pas expiré au premier clock");

    Apu cgb(gb::HardwareMode::cgb_native);
    cgb.write(0xff26, 0x00);
    cgb.write(0xff11, 0x3f); // doit être ignoré sur CGB
    cgb.write(0xff26, 0x80);
    cgb.write(0xff12, 0xf0);
    cgb.write(0xff14, 0xc0);
    cgb.clock_divider_falling_edge();
    check((cgb.read(0xff26) & 1) != 0,
          "CGB a accepté une écriture de longueur alors que NR52 était coupé");
}

void div_driven_frame_sequencer_test() {
    Apu apu(gb::HardwareMode::dmg);
    apu.write(0xff11, 0x3f); // longueur = 1
    apu.write(0xff12, 0xf0);
    apu.write(0xff14, 0xc0);
    apu.tick(8192 * 3);
    check((apu.read(0xff26) & 1) != 0,
          "l'APU cadence encore la longueur avec un minuteur autonome");
    apu.clock_divider_falling_edge();
    check((apu.read(0xff26) & 1) == 0,
          "front descendant DIV-APU non transmis au séquenceur de trame");

    Apu powered_off(gb::HardwareMode::dmg);
    powered_off.write(0xff26, 0x00);
    powered_off.clock_divider_falling_edge(); // DIV-APU avance vers l'étape 1
    powered_off.write(0xff26, 0x80);
    powered_off.write(0xff11, 0x3f);
    powered_off.write(0xff12, 0xf0);
    powered_off.write(0xff14, 0x80);
    powered_off.write(0xff14, 0x40);
    check((powered_off.read(0xff26) & 1) == 0,
          "NR52 off a réinitialisé à tort la phase du compteur DIV-APU");
}

void extra_length_clock_test() {
    Apu enable_edge(gb::HardwareMode::dmg);
    enable_edge.write(0xff11, 0x3f); // longueur = 1, désactivée
    enable_edge.write(0xff12, 0xf0);
    enable_edge.write(0xff14, 0x80); // trigger sans activer la longueur
    enable_edge.clock_divider_falling_edge(); // prochaine étape = 1, sans longueur
    enable_edge.write(0xff14, 0x40);          // activation => clock immédiat
    check((enable_edge.read(0xff26) & 1) == 0,
          "activation longueur hors étape n'a pas produit le clock supplémentaire");

    Apu trigger_reload(gb::HardwareMode::cgb_native);
    trigger_reload.write(0xff12, 0xf0);
    trigger_reload.clock_divider_falling_edge(); // prochaine étape = 1
    trigger_reload.write(0xff14, 0xc0);          // longueur 0 => reload à 63
    for (int edge = 0; edge < 124; ++edge) trigger_reload.clock_divider_falling_edge();
    check((trigger_reload.read(0xff26) & 1) != 0,
          "reload longueur raccourci a expiré avant son 63e clock");
    trigger_reload.clock_divider_falling_edge();
    trigger_reload.clock_divider_falling_edge();
    check((trigger_reload.read(0xff26) & 1) == 0,
          "trigger hors étape a rechargé 64 au lieu de 63");
}

/** Niveau le plus haut atteint par le canal 1 sur un cycle de rapport cyclique. */
[[nodiscard]] int peak_square1(Apu& apu, int frequency) {
    int peak = 0;
    const int step_cycles = (2048 - frequency) * 4;
    for (int cycle = 0; cycle < step_cycles * 8; ++cycle) {
        apu.tick(1);
        peak = std::max(peak, apu.read_pcm12() & 0x0f);
    }
    return peak;
}

/** Idem pour le canal de bruit, lu dans les bits hauts de PCM34. */
[[nodiscard]] int peak_noise(Apu& apu, int cycles) {
    int peak = 0;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        apu.tick(1);
        peak = std::max(peak, (apu.read_pcm34() >> 4) & 0x0f);
    }
    return peak;
}

/**
 * Une période d'enveloppe nulle ne fait pas bouger le volume.
 *
 * La documentation matérielle énonce deux règles qu'il est tentant de
 * confondre : « le compteur d'enveloppe traite une période de 0 comme 8 » et
 * « quand le compteur produit un top **et que la période n'est pas nulle**,
 * un nouveau volume est calculé ». La première ne concerne que l'instant du
 * top ; c'est la seconde qui décide si le volume change.
 *
 * Ne retenir que la première fait décroître le volume d'un cran toutes les
 * huit périodes. Cette vérification échoue alors dès le soixante-quatrième
 * top.
 */
void period_zero_envelope_test() {
    Apu apu(gb::HardwareMode::dmg);
    apu.write(0xff11, 0xc0); // duty 75 %, étape 1 haute
    apu.write(0xff12, 0x18); // volume 1, addition, période 0 = pas d'enveloppe
    apu.write(0xff13, 0xff);
    apu.write(0xff14, 0x87);
    apu.tick(4); // quitte la première étape forcée à zéro
    for (int edge = 0; edge < 8 * 64; ++edge) apu.clock_divider_falling_edge();
    check((apu.read_pcm12() & 0x0f) == 1,
          "une période d'enveloppe nulle a fait bouger le volume");
}

/**
 * Une note à volume fixe tient, et une note à enveloppe descend toujours.
 *
 * Les deux moitiés comptent, et elles se contredisent : la première seule
 * serait satisfaite en supprimant l'enveloppe, la seconde seule l'était par le
 * défaut qu'on corrige ici. Ensemble, elles fixent le comportement.
 *
 * La durée est choisie pour dépasser franchement ce que coûtait le défaut :
 * quinze crans à retirer, à un cran toutes les huit tops d'enveloppe, soit
 * neuf cent soixante pas de séquenceur, à peine moins de deux secondes
 * émulées. Une tenue ou une ligne de basse de cette longueur n'a rien
 * d'exceptionnel dans un morceau Game Boy : elle disparaissait en route.
 */
void sustained_fixed_volume_test() {
    constexpr int frequency = 1750; // environ 440 Hz
    Apu apu(gb::HardwareMode::dmg);
    apu.write(0xff26, 0x80); // alimentation
    apu.write(0xff24, 0x77); // volumes maîtres au maximum
    apu.write(0xff25, 0x11); // canal 1 des deux côtés
    apu.write(0xff11, 0x80); // rapport cyclique de moitié, longueur non armée
    apu.write(0xff12, 0xf0); // volume 15, décroissante, période 0
    apu.write(0xff13, frequency & 0xff);
    apu.write(0xff14, 0x80 | ((frequency >> 8) & 7));
    check(peak_square1(apu, frequency) == 15, "la note ne part pas au volume programmé");

    for (int step = 0; step < 1024; ++step) apu.clock_divider_falling_edge();
    check(peak_square1(apu, frequency) == 15,
          "une note à volume fixe s'est éteinte toute seule");

    // Le même canal, avec une enveloppe réelle, doit bien descendre.
    Apu fading(gb::HardwareMode::dmg);
    fading.write(0xff26, 0x80);
    fading.write(0xff24, 0x77);
    fading.write(0xff25, 0x11);
    fading.write(0xff11, 0x80);
    fading.write(0xff12, 0xf1); // volume 15, décroissante, période 1
    fading.write(0xff13, frequency & 0xff);
    fading.write(0xff14, 0x80 | ((frequency >> 8) & 7));
    // Quinze crans à un top chacun, soit cent vingt pas de séquenceur.
    for (int step = 0; step < 8 * 20; ++step) fading.clock_divider_falling_edge();
    check(peak_square1(fading, frequency) == 0,
          "une enveloppe de période non nulle ne descend plus");
}

/** Le canal de bruit suit la même règle : sa percussion ne doit pas fondre. */
void sustained_fixed_volume_noise_test() {
    Apu apu(gb::HardwareMode::dmg);
    apu.write(0xff26, 0x80);
    apu.write(0xff24, 0x77);
    apu.write(0xff25, 0x88); // canal 4 des deux côtés
    apu.write(0xff20, 0x00); // longueur non armée
    apu.write(0xff21, 0xf0); // volume 15, décroissante, période 0
    apu.write(0xff22, 0x00); // cadence de décalage la plus rapide
    apu.write(0xff23, 0x80); // déclenchement
    check(peak_noise(apu, 4096) == 15, "le bruit ne part pas au volume programmé");

    for (int step = 0; step < 1024; ++step) apu.clock_divider_falling_edge();
    check(peak_noise(apu, 4096) == 15, "un bruit à volume fixe s'est éteint tout seul");
}

void trigger_envelope_delay_and_zombie_test() {
    Apu delayed(gb::HardwareMode::cgb_native);
    for (int edge = 0; edge < 7; ++edge) delayed.clock_divider_falling_edge();
    delayed.write(0xff11, 0xc0);
    delayed.write(0xff12, 0x19); // volume 1, addition, période 1
    delayed.write(0xff13, 0xff);
    delayed.write(0xff14, 0x87);
    delayed.tick(4);
    delayed.clock_divider_falling_edge(); // étape 7 : premier clock envelope
    check((delayed.read_pcm12() & 0x0f) == 1,
          "trigger juste avant l'envelope n'a pas ajouté un au timer rechargé");
    for (int edge = 0; edge < 8; ++edge) delayed.clock_divider_falling_edge();
    check((delayed.read_pcm12() & 0x0f) == 2,
          "envelope retardée n'a pas repris au clock suivant");

    Apu zombie(gb::HardwareMode::dmg);
    zombie.write(0xff11, 0xc0);
    zombie.write(0xff12, 0x18);
    zombie.write(0xff13, 0xff);
    zombie.write(0xff14, 0x87);
    zombie.tick(4);
    zombie.write(0xff12, 0x08);
    check((zombie.read_pcm12() & 0x0f) == 2,
          "cas zombie commun $V8->$08 n'a pas incrémenté le volume actif");
}

void dmg_wave_retrigger_corruption_test() {
    const auto prepare = [](Apu& apu) {
        for (int index = 0; index < 16; ++index) apu.write(0xff30 + index, 0x10 + index);
        apu.write(0xff1a, 0x80);
        apu.write(0xff1d, 0xfe);
        apu.write(0xff1e, 0x87); // période = 4 dots
        apu.tick(40);            // octet courant 5, bloc aligné 4..7
        apu.write(0xff1e, 0x87); // retrigger actif
        apu.write(0xff1a, 0x00);
    };

    Apu dmg(gb::HardwareMode::dmg);
    prepare(dmg);
    for (int index = 0; index < 4; ++index) {
        check(dmg.read(0xff30 + index) == 0x14 + index,
              "retrigger wave DMG n'a pas copié le bloc courant vers les octets 0..3");
    }

    Apu cgb(gb::HardwareMode::cgb_native);
    prepare(cgb);
    for (int index = 0; index < 4; ++index) {
        check(cgb.read(0xff30 + index) == 0x10 + index,
              "corruption de retrigger wave DMG appliquée à tort au CGB");
    }


    Apu outside_read(gb::HardwareMode::dmg);
    for (int index = 0; index < 16; ++index) outside_read.write(0xff30 + index, 0x20 + index);
    outside_read.write(0xff1a, 0x80);
    outside_read.write(0xff1d, 0xfe);
    outside_read.write(0xff1e, 0x87);
    outside_read.tick(42); // deux dots après la fenêtre de lecture de l'octet 5
    outside_read.write(0xff1e, 0x87);
    outside_read.write(0xff1a, 0x00);
    for (int index = 0; index < 4; ++index) {
        check(outside_read.read(0xff30 + index) == 0x20 + index,
              "wave RAM DMG corrompue alors que le canal ne lisait aucun octet");
    }
}

void noise_shift_disconnect_test() {
    const auto run = [](int shift) {
        Apu apu(gb::HardwareMode::dmg);
        apu.write(0xff21, 0xf0);
        apu.write(0xff22, shift << 4); // diviseur 8
        apu.write(0xff23, 0x80);
        apu.tick((8 << shift) * 15);
        return apu.read_pcm34() & 0xf0;
    };
    check(run(13) == 0xf0, "précondition LFSR : quinze clocks n'ont pas atteint un bit bas nul");
    check(run(14) == 0x00, "NR43 shift 14 a cadencé le LFSR alors que son horloge est coupée");
    check(run(15) == 0x00, "NR43 shift 15 a cadencé le LFSR alors que son horloge est coupée");
}

void dac_polarity_and_disconnect_test() {
    Apu apu(gb::HardwareMode::dmg);
    apu.write(0xff11, 0xc0);
    apu.write(0xff12, 0xf0);
    apu.write(0xff13, 0xff);
    apu.write(0xff14, 0x87);
    apu.tick(128);
    std::array<std::int16_t, 2> samples{};
    check(apu.read_samples(samples) == 2 && samples[0] < 0 && samples[1] < 0,
          "pente DAC inversée : la valeur numérique 15 ne produit pas une tension négative");

    apu.write(0xff12, 0x00);
    apu.tick(128);
    check(apu.read_samples(samples) == 2 && samples[0] == 0 && samples[1] == 0,
          "sortie APU non nulle alors que les quatre DAC sont déconnectés");
}

void first_duty_step_and_idle_clock_test() {
    Apu apu(gb::HardwareMode::cgb_native);
    apu.write(0xff11, 0xc0); // duty 75 % : position 0 basse, position 1 haute
    apu.write(0xff12, 0xf0);
    apu.write(0xff13, 0xff);
    apu.tick(8192 * 6);      // le compteur duty doit rester arrêté avant trigger
    apu.write(0xff14, 0x87);
    check((apu.read_pcm12() & 0x0f) == 0,
          "première étape duty après power-on non forcée à zéro");
    apu.tick(4);
    check((apu.read_pcm12() & 0x0f) == 0x0f,
          "compteur duty a avancé avant le premier trigger ou n'a pas démarré après");
}

void apu_state_mid_wave_access_test() {
    Apu source(gb::HardwareMode::dmg);
    source.write(0xff30, 0xab);
    source.write(0xff1a, 0x80);
    source.write(0xff1d, 0xfe);
    source.write(0xff1e, 0x87);
    source.tick(4);

    detail::BinaryWriter writer;
    source.save(writer);
    const auto state = std::move(writer).take();
    Apu restored(gb::HardwareMode::dmg);
    detail::BinaryReader reader(state);
    restored.load(reader);
    check(reader.exhausted() && restored.read(0xff3f) == 0xab,
          "fenêtre wave RAM APU non restaurée par save state");
    detail::BinaryWriter restored_writer;
    restored.save(restored_writer);
    check(std::move(restored_writer).take() == state,
          "accumulateurs audio APU perdus immédiatement après restauration");
    source.tick(2);
    restored.tick(2);
    check(source.read(0xff30) == restored.read(0xff30),
          "timing APU divergent après restauration en lecture wave RAM");
}

void apu_state_layout_rejection_test() {
    Apu source(gb::HardwareMode::dmg);
    detail::BinaryWriter writer;
    source.save(writer);
    auto state = std::move(writer).take();
    state[0] = 2;
    expect_failure<SaveStateError>(
        [&] {
            Apu restored(gb::HardwareMode::dmg);
            detail::BinaryReader reader(state);
            restored.load(reader);
        },
        "un layout APU incompatible a été chargé silencieusement");
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    hle_post_boot_register_test();
    hle_post_boot_silence_test();
    post_boot_channel_still_audible_test();
    game_apu_init_is_silent_test();
    wave_ram_access_test();
    sweep_negate_clear_test();
    powered_off_length_counter_test();
    div_driven_frame_sequencer_test();
    extra_length_clock_test();
    period_zero_envelope_test();
    sustained_fixed_volume_test();
    sustained_fixed_volume_noise_test();
    trigger_envelope_delay_and_zombie_test();
    dmg_wave_retrigger_corruption_test();
    noise_shift_disconnect_test();
    dac_polarity_and_disconnect_test();
    first_duty_step_and_idle_clock_test();
    apu_state_mid_wave_access_test();
    apu_state_layout_rejection_test();
    return 0;
}
