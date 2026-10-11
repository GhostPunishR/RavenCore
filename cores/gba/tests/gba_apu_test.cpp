#include "apu/apu.hpp"

#include "check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * Repliement du spectre dans le mélangeur du cœur Game Boy Advance.
 *
 * ### Ce que ces vérifications mesurent, et pourquoi
 *
 * Les quatre canaux changent d'état bien plus vite que le débit de sortie : un
 * canal carré à sa fréquence la plus haute bascule tous les quatre cycles,
 * quand un échantillon en dure cent vingt-huit. Prélever la valeur qui se
 * trouve là à l'instant du prélèvement — ce que ce mélangeur faisait — replie
 * toutes les harmoniques au-dessus de la moitié du débit dans l'audible, où
 * elles s'entendent comme des sifflements sans rapport avec le morceau.
 *
 * Ce qu'il faut prélever est la **moyenne sur la fenêtre**. Elle ne demande ni
 * filtre ni fenêtre glissante : il suffit de cumuler chaque valeur le temps
 * qu'elle dure, ce que le cœur Game Boy fait depuis toujours.
 *
 * Les vérifications ci-dessous sont écrites pour **échouer sur l'ancien
 * comportement**. Une moyenne et une valeur instantanée se ressemblent tant que
 * le canal est lent ; elles ne se séparent qu'aux fréquences hautes, et c'est
 * donc là que tout se joue.
 */
namespace ravenemu::gba::testing {

using ravenemu::testing::check;

namespace {

/** Un échantillon dure cent vingt-huit cycles du générateur. */
constexpr int cycles_per_sample = 128;
/** Le mélangeur reçoit des cycles processeur : quatre pour un cycle audio. */
constexpr int cpu_cycles_per_audio_cycle = 4;

/** Mélangeur allumé, quatre canaux routés des deux côtés, volume au maximum. */
[[nodiscard]] Apu powered_mixer() {
    Apu apu;
    apu.write_register(0x84, 0x80);   // alimentation générale
    // Les quatre canaux vers les deux voies, et les deux volumes au maximum.
    apu.write_register(0x80, 0xff77);
    // Rapport des canaux internes au maximum, voies directes coupées.
    apu.write_register(0x82, 0x0002);
    return apu;
}

/** Fait tourner le mélangeur et rend les échantillons de la voie gauche. */
[[nodiscard]] std::vector<int> run_left(Apu& apu, int samples) {
    std::vector<int> left;
    left.reserve(static_cast<std::size_t>(samples));
    std::array<std::int16_t, 512> block{};
    while (static_cast<int>(left.size()) < samples) {
        apu.tick(cycles_per_sample * cpu_cycles_per_audio_cycle);
        const auto count = apu.read_samples(block);
        for (std::size_t i = 0; i + 1 < count; i += 2) {
            left.push_back(block[i]);
        }
    }
    left.resize(static_cast<std::size_t>(samples));
    return left;
}

/**
 * Un canal carré à sa fréquence la plus haute rend une valeur stable.
 *
 * À la période minimale, le canal bascule trente-deux fois par échantillon.
 * Sa moyenne est donc la même d'un échantillon à l'autre — c'est un signal
 * continu, pas un son. Prélevé à l'instant, il alternerait au contraire entre
 * ses deux extrêmes et produirait un ton de seize kilohertz que le matériel
 * n'émet pas.
 */
void un_carre_tres_rapide_ne_produit_pas_de_ton() {
    // Référence : le même canal assez lent pour que ses paliers soient plus
    // longs qu'un échantillon. Le sommet de sa course est le niveau « allumé »,
    // celui que le canal atteint quand sa sortie est haute tout du long.
    auto lent = powered_mixer();
    lent.write_register(0x62, 0x80 | (0xf << 12) | (1 << 11));
    lent.write_register(0x64, 1024 | 0x8000);
    const auto reference = run_left(lent, 64);
    const auto allume = *std::max_element(reference.begin() + 8, reference.end());
    check(allume > 0, "le niveau de référence est atteint");

    auto apu = powered_mixer();
    // Rapport cyclique de moitié — le champ vaut deux —, volume quinze,
    // fréquence maximale.
    apu.write_register(0x62, 0x80 | (0xf << 12) | (1 << 11));
    apu.write_register(0x64, 2047 | 0x8000);

    const auto left = run_left(apu, 64);
    // Les huit premiers échantillons couvrent l'amorçage du canal ; ce sont les
    // suivants qui portent le régime établi.
    const auto begin = left.begin() + 8;
    const auto lowest = *std::min_element(begin, left.end());
    const auto highest = *std::max_element(begin, left.end());

    // Une valeur instantanée alternerait entre zéro et le niveau allumé ;
    // l'écart vaudrait tout le signal. Une moyenne ne bouge que d'une marche.
    check(
        highest - lowest <= allume / 8,
        "la sortie est stable d'un échantillon à l'autre"
    );
    // Et elle est stable **à la bonne hauteur**. Sans cette borne, un canal
    // resté bloqué sur un seul palier — ce que faisait l'avance par bonds de
    // huit pas d'un coup — passerait pour un signal parfaitement lisse, muet
    // ou saturé selon la phase où il s'est figé.
    // Une moitié de cycle de service donne la moitié du niveau allumé. La
    // fourchette laisse la marche de quantification, pas davantage.
    check(highest > allume * 3 / 8, "et elle n'est pas restée muette");
    check(highest < allume * 5 / 8, "ni saturée : c'est bien la moitié");
}

/**
 * La moyenne d'un rapport cyclique se lit dans la hauteur du signal continu.
 *
 * Deux rapports cycliques différents, à la même fréquence très haute, donnent
 * deux niveaux continus dans le rapport de leurs cycles de service. C'est la
 * signature d'une moyenne : une valeur instantanée ne saurait pas les
 * distinguer autrement que par le hasard de l'instant prélevé.
 *
 * Les rapports sont ceux du matériel : un huitième, un quart, une moitié.
 */
void la_moyenne_suit_le_rapport_cyclique() {
    const auto niveau = [](int duty) {
        auto apu = powered_mixer();
        apu.write_register(0x62, (duty << 6) | (0xf << 12) | (1 << 11));
        apu.write_register(0x64, 2047 | 0x8000);
        const auto left = run_left(apu, 64);
        // Le régime établi, moyenné sur la fin de la course.
        long long total = 0;
        for (auto i = left.begin() + 32; i != left.end(); ++i) total += *i;
        return static_cast<double>(total) / 32.0;
    };

    const auto huitieme = niveau(0);
    const auto quart = niveau(1);
    const auto moitie = niveau(2);

    check(huitieme > 0.0, "le rapport le plus court produit un niveau");
    check(quart > huitieme, "un quart porte plus qu'un huitième");
    check(moitie > quart, "une moitié porte plus qu'un quart");
    // Les rapports sont exacts à la marche de quantification près : un quart
    // vaut deux huitièmes, une moitié en vaut quatre.
    check(std::abs(quart - 2.0 * huitieme) < huitieme * 0.2, "un quart vaut deux huitièmes");
    check(std::abs(moitie - 4.0 * huitieme) < huitieme * 0.3, "une moitié en vaut quatre");
}

/**
 * Le bruit à sa cadence la plus rapide ne devient pas un souffle métallique.
 *
 * Son spectre monte jusqu'à la fréquence de décalage, très au-dessus de la
 * moitié du débit de sortie. Sans moyenne, il se replie et son amplitude reste
 * celle des extrêmes ; avec la moyenne, elle se resserre autour du niveau
 * moyen, ce qu'un bruit blanc filtré fait naturellement.
 */
void le_bruit_rapide_se_resserre_autour_de_sa_moyenne() {
    auto apu = powered_mixer();
    // Volume quinze, enveloppe fixe, puis la cadence de décalage la plus rapide.
    apu.write_register(0x78, (0xf << 12) | (1 << 11));
    apu.write_register(0x7c, 0x8000);

    const auto left = run_left(apu, 128);
    const auto begin = left.begin() + 16;
    long long total = 0;
    for (auto i = begin; i != left.end(); ++i) total += *i;
    const auto average = static_cast<double>(total) / static_cast<double>(left.end() - begin);
    check(average > 0.0, "le bruit produit quelque chose");

    double worst = 0.0;
    for (auto i = begin; i != left.end(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(*i) - average));
    }
    // Prélevé à l'instant, chaque échantillon vaudrait zéro ou le maximum :
    // l'écart au niveau moyen vaudrait ce niveau tout entier.
    check(worst < average * 0.75, "les écarts au niveau moyen restent contenus");
}

/**
 * L'extinction du mélangeur ne verse pas la fenêtre précédente dans la suite.
 *
 * Les cumuls appartiennent à la fenêtre qui s'achève. Les garder en réserve
 * ferait sortir, au rallumage, un échantillon composé de deux moitiés
 * étrangères l'une à l'autre — un claquement, exactement là où un jeu coupe
 * puis rallume son mélangeur.
 */
void l_extinction_ne_garde_pas_la_fenetre_precedente() {
    auto apu = powered_mixer();
    apu.write_register(0x62, 0x80 | (0xf << 12) | (1 << 11));
    apu.write_register(0x64, 2047 | 0x8000);
    static_cast<void>(run_left(apu, 8));

    // Coupure du mélangeur : les canaux sont remis à zéro par le matériel.
    apu.write_register(0x84, 0x00);
    static_cast<void>(run_left(apu, 4));

    // Rallumage, sans reprogrammer aucun canal : rien ne doit sortir.
    apu.write_register(0x84, 0x80);
    const auto after = run_left(apu, 8);
    const auto highest = *std::max_element(after.begin(), after.end());
    check(highest == 0, "aucun résidu ne franchit l'extinction");
}

} // namespace

} // namespace ravenemu::gba::testing

int main() {
    using namespace ravenemu::gba::testing;
    un_carre_tres_rapide_ne_produit_pas_de_ton();
    la_moyenne_suit_le_rapport_cyclique();
    le_bruit_rapide_se_resserre_autour_de_sa_moyenne();
    l_extinction_ne_garde_pas_la_fenetre_precedente();
    return 0;
}
