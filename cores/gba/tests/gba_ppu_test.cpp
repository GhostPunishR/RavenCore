#include "ppu/ppu.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <cstdint>
#include <memory>
#include <vector>

/**
 * Mosaïque de l'affichage Game Boy Advance.
 *
 * ### Ce que ces vérifications mesurent, et pourquoi
 *
 * Le registre `MOSAIC` (0x4000004c) découpe l'image en blocs et fait recopier à
 * chaque bloc la couleur de son coin haut-gauche. Les jeux s'en servent pour les
 * fondus « pixelisés » d'entrée et de sortie de combat, pour les transitions de
 * carte, et pour signaler l'étourdissement d'un personnage. Tant que le registre
 * n'est pas lu, l'effet demandé par le jeu ne se produit tout simplement pas :
 * l'écran reste net là où le matériel le rendrait grossier.
 *
 * Trois activations distinctes existent, et une seule des trois suffirait à
 * donner l'illusion que l'effet marche : le bit 6 de `BGxCNT` pour un plan, le
 * bit 12 de l'attribut 0 pour un objet, et les quatre quartets de `MOSAIC` qui
 * donnent séparément les tailles horizontale et verticale des plans et des
 * objets. Les vérifications ci-dessous éprouvent chacune de ces trois portes,
 * et croisent volontairement les quartets — mosaïque de plan neutre pendant que
 * celle des objets agit, et l'inverse — pour qu'une inversion des champs ne
 * puisse pas passer.
 *
 * Le motif de référence est une tuile dont chaque point porte un index de
 * palette différent : deux points voisins ne peuvent donc être de la même
 * couleur que si la mosaïque les a réunis.
 */
namespace ravenemu::gba::testing {

using ravenemu::testing::check;

namespace {

constexpr int cycles_per_frame = 1232 * 228;
constexpr int screen_width = Ppu::screen_width;

/** Cœur vidéo nu : une cartouche minimale, un bus, un PPU. */
struct Fixture {
    Fixture()
        : image(std::make_shared<const std::vector<std::uint8_t>>(ravenemu::testing::minimal_gba_rom())),
          cartridge(image, GbaSaveType::none, false, [] { return std::int64_t{0}; }),
          bus(cartridge),
          ppu(bus) {
        bus.ppu = &ppu;
        // Une OAM vierge décrit 128 objets valides empilés en (0,0) : on les
        // écarte tous, les vérifications qui en veulent un le rétablissent.
        for (int sprite = 0; sprite < 128; ++sprite) bus.oam[static_cast<std::size_t>(sprite * 8 + 1)] = 0x02;
    }

    void reg(int offset, int value) {
        bus.io[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value);
        bus.io[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>(value >> 8);
    }
    void palette(int index, int color) {
        bus.palette[static_cast<std::size_t>(index * 2)] = static_cast<std::uint8_t>(color);
        bus.palette[static_cast<std::size_t>(index * 2 + 1)] = static_cast<std::uint8_t>(color >> 8);
    }
    void oam(int offset, int value) {
        bus.oam[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value);
        bus.oam[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>(value >> 8);
    }
    /**
     * Tuile 8 bpp de 64 octets à [address] dont chaque point porte son propre
     * index de palette, de 1 à 64 : aucun voisin n'a la même couleur.
     */
    void write_gradient_tile(int address) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                bus.vram[static_cast<std::size_t>(address + y * 8 + x)] = static_cast<std::uint8_t>(y * 8 + x + 1);
            }
        }
    }
    void write_gradient_palette(int base) {
        for (int index = 1; index <= 64; ++index) palette(base + index, index);
    }
    [[nodiscard]] std::int32_t pixel(int x, int y) const {
        return ppu.frame[static_cast<std::size_t>(y * screen_width + x)];
    }
    void render_frame() { ppu.tick(cycles_per_frame); }

    RomImage image;
    Cartridge cartridge;
    Bus bus;
    Ppu ppu;
};

/** BG0 en mode texte 8 bpp, carte entière sur la tuile dégradée. */
void setup_text_background(Fixture& fixture, bool mosaic, int horizontal_scroll) {
    fixture.reg(0x00, 0x0100);                          // mode 0, BG0 actif
    fixture.reg(0x08, 0x1080 | (mosaic ? 0x40 : 0));    // 8 bpp, carte en 0x8000, mosaïque
    fixture.reg(0x10, horizontal_scroll);               // BG0HOFS
    fixture.write_gradient_tile(0);
    fixture.write_gradient_palette(0);
}

/** BG2 affine identité, carte 128x128 répétée, sur la même tuile dégradée. */
void setup_affine_background(Fixture& fixture, bool mosaic) {
    fixture.reg(0x00, 0x0402);                          // mode 2, BG2 actif
    fixture.reg(0x0c, 0x2004 | (mosaic ? 0x40 : 0));    // répétition, tuiles en 0x4000
    fixture.reg(0x20, 0x0100);                          // PA = 1
    fixture.reg(0x22, 0);                               // PB = 0
    fixture.reg(0x24, 0);                               // PC = 0
    fixture.reg(0x26, 0x0100);                          // PD = 1
    fixture.write_gradient_tile(0x4000);
    fixture.write_gradient_palette(0);
}

/** Objet 32x32 en 8 bpp posé en (0,0), toutes ses tuiles sur le dégradé. */
void setup_sprite(Fixture& fixture, bool mosaic) {
    fixture.reg(0x00, 0x1040);                          // objets actifs, mappage 1D
    fixture.oam(0, 0x2000 | (mosaic ? 0x1000 : 0));     // y = 0, 8 bpp, mosaïque
    fixture.oam(2, 0x8000);                             // x = 0, taille 32x32
    fixture.oam(4, 0x0000);                             // tuile 0, priorité 0
    for (int tile = 0; tile < 16; ++tile) fixture.write_gradient_tile(0x10000 + tile * 64);
    fixture.write_gradient_palette(256);
}

/**
 * Le même objet, mais posé sur une scène qui affiche déjà un plan : `DISPCNT`
 * garde alors ses deux couches. L'objet couvre le haut de l'écran, le plan
 * reste seul en dessous, ce qui permet de lire les deux dans une même image.
 */
void setup_sprite_over_background(Fixture& fixture, bool mosaic) {
    setup_sprite(fixture, mosaic);
    fixture.reg(0x00, 0x1140);                          // mode 0, BG0 et objets
    fixture.write_gradient_palette(0);
}

/**
 * Sans le bit d'activation, le seul contenu de `MOSAIC` ne doit rien changer :
 * c'est ce qui distingue une mosaïque pilotée d'une mosaïque toujours active.
 */
void un_plan_sans_son_bit_de_mosaique_reste_net() {
    Fixture fixture;
    setup_text_background(fixture, false, 0);
    fixture.reg(0x4c, 0x0033);                          // plans 4x4, objets 1x1
    fixture.render_frame();
    check(fixture.pixel(1, 0) != fixture.pixel(0, 0), "MOSAIC seul a groupé des colonnes d'un plan net");
    check(fixture.pixel(0, 1) != fixture.pixel(0, 0), "MOSAIC seul a groupé des lignes d'un plan net");
}

/**
 * Le bloc entier prend la couleur de son coin haut-gauche, et le bloc suivant
 * en change : sans cette seconde moitié, un plan uniformément gris passerait.
 */
void la_mosaique_d_un_plan_texte_repete_le_coin_du_bloc() {
    Fixture fixture;
    setup_text_background(fixture, true, 0);
    fixture.reg(0x4c, 0x0033);                          // plans 4x4, objets 1x1
    fixture.render_frame();
    const auto corner = fixture.pixel(0, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            check(fixture.pixel(x, y) == corner, "un point du bloc ne reprend pas son coin haut-gauche");
        }
    }
    check(fixture.pixel(4, 0) != corner, "le bloc horizontal suivant garde la couleur du précédent");
    check(fixture.pixel(0, 4) != corner, "le bloc vertical suivant garde la couleur du précédent");
}

/**
 * Les deux tailles sont indépendantes : une mosaïque large d'un point et haute
 * de quatre ne doit grouper que des lignes. Une lecture croisée des quartets
 * donnerait ici des blocs carrés.
 */
void les_deux_tailles_d_un_plan_sont_independantes() {
    Fixture fixture;
    setup_text_background(fixture, true, 0);
    fixture.reg(0x4c, 0x0030);                          // plans 1 de large, 4 de haut
    fixture.render_frame();
    check(fixture.pixel(1, 0) != fixture.pixel(0, 0), "une mosaïque large d'un point a groupé des colonnes");
    check(fixture.pixel(0, 3) == fixture.pixel(0, 0), "une mosaïque haute de quatre n'a pas groupé les lignes");
    check(fixture.pixel(0, 4) != fixture.pixel(0, 0), "la bande verticale déborde de sa hauteur");
}

/**
 * La grille des blocs appartient à l'écran, pas au plan : le repère est ramené
 * au coin du bloc **avant** l'ajout du défilement. Découper après l'addition
 * ferait glisser la grille avec le décor, et les quatre points d'un bloc ne
 * seraient plus identiques dès que le défilement n'est pas un multiple de la
 * taille du bloc, ce que ce décalage de deux points met en évidence.
 */
void la_grille_de_mosaique_ne_defile_pas_avec_le_plan() {
    Fixture reference;
    setup_text_background(reference, false, 0);
    reference.render_frame();
    const auto expected = reference.pixel(2, 0);        // colonne source du bloc

    Fixture fixture;
    setup_text_background(fixture, true, 2);
    fixture.reg(0x4c, 0x0033);
    fixture.render_frame();
    for (int x = 0; x < 4; ++x) {
        check(fixture.pixel(x, 0) == expected, "le bloc décalé ne montre pas la colonne source attendue");
    }
    check(fixture.pixel(4, 0) != expected, "le bloc suivant montre encore la colonne du précédent");
}

/**
 * Sur un plan affine le parcours avance point par point : la mosaïque ne peut
 * pas se contenter d'arrondir une coordonnée d'écran, il faut retenir celle du
 * début du bloc. Verticalement, cela revient à figer le point de référence
 * interne, qui lui continue d'avancer à chaque ligne.
 */
void la_mosaique_d_un_plan_affine_fige_le_bloc() {
    Fixture net;
    setup_affine_background(net, false);
    net.render_frame();
    check(net.pixel(1, 0) != net.pixel(0, 0), "précondition : le plan affine net n'est pas dégradé");
    check(net.pixel(0, 1) != net.pixel(0, 0), "précondition : le plan affine net ne varie pas en hauteur");

    Fixture fixture;
    setup_affine_background(fixture, true);
    fixture.reg(0x4c, 0x0033);
    fixture.render_frame();
    const auto corner = fixture.pixel(0, 0);
    check(corner == net.pixel(0, 0), "le coin du bloc affine ne montre pas le point d'origine");
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            check(fixture.pixel(x, y) == corner, "un point du bloc affine ne reprend pas son coin");
        }
    }
    check(fixture.pixel(4, 0) != corner, "le bloc affine suivant garde la couleur du précédent");
    check(fixture.pixel(0, 4) != corner, "la bande affine suivante garde la couleur de la précédente");
}

/**
 * L'objet a son propre bit d'activation et ses propres quartets. La mosaïque
 * des plans est ici volontairement neutre : si les quartets étaient croisés,
 * l'objet resterait net.
 */
void la_mosaique_d_un_objet_repete_le_coin_du_bloc() {
    Fixture net;
    setup_sprite(net, false);
    net.reg(0x4c, 0x3300);
    net.render_frame();
    check(net.pixel(1, 0) != net.pixel(0, 0), "précondition : l'objet sans son bit de mosaïque est déjà groupé");

    Fixture fixture;
    setup_sprite(fixture, true);
    fixture.reg(0x4c, 0x3300);                          // objets 4x4, plans 1x1
    fixture.render_frame();
    const auto corner = fixture.pixel(0, 0);
    check(corner == net.pixel(0, 0), "le coin du bloc de l'objet ne montre pas le point d'origine");
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            check(fixture.pixel(x, y) == corner, "un point de l'objet ne reprend pas son coin haut-gauche");
        }
    }
    check(fixture.pixel(4, 0) != corner, "le bloc suivant de l'objet garde la couleur du précédent");
    check(fixture.pixel(0, 4) != corner, "la bande suivante de l'objet garde la couleur de la précédente");
}

/**
 * Les quartets des plans et ceux des objets sont dans le même registre : une
 * mosaïque d'objet active ne doit pas grossir les plans, et réciproquement.
 */
void les_mosaiques_des_plans_et_des_objets_ne_se_melangent_pas() {
    Fixture par_les_objets;
    setup_text_background(par_les_objets, true, 0);
    setup_sprite_over_background(par_les_objets, true);
    par_les_objets.reg(0x4c, 0x3300);                   // objets 4x4, plans 1x1
    par_les_objets.render_frame();
    check(par_les_objets.pixel(1, 0) == par_les_objets.pixel(0, 0), "les quartets des objets n'ont pas groupé l'objet");
    check(par_les_objets.pixel(1, 100) != par_les_objets.pixel(0, 100), "les quartets des objets ont groupé le plan");

    Fixture par_les_plans;
    setup_text_background(par_les_plans, true, 0);
    setup_sprite_over_background(par_les_plans, true);
    par_les_plans.reg(0x4c, 0x0033);                    // plans 4x4, objets 1x1
    par_les_plans.render_frame();
    check(par_les_plans.pixel(1, 0) != par_les_plans.pixel(0, 0), "les quartets des plans ont groupé l'objet");
    check(par_les_plans.pixel(1, 100) == par_les_plans.pixel(0, 100), "les quartets des plans n'ont pas groupé le plan");
}

} // namespace

} // namespace ravenemu::gba::testing

int main() {
    using namespace ravenemu::gba::testing;
    un_plan_sans_son_bit_de_mosaique_reste_net();
    la_mosaique_d_un_plan_texte_repete_le_coin_du_bloc();
    les_deux_tailles_d_un_plan_sont_independantes();
    la_grille_de_mosaique_ne_defile_pas_avec_le_plan();
    la_mosaique_d_un_plan_affine_fige_le_bloc();
    la_mosaique_d_un_objet_repete_le_coin_du_bloc();
    les_mosaiques_des_plans_et_des_objets_ne_se_melangent_pas();
    return 0;
}
