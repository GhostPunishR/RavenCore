#include "ppu/ppu.hpp"

#include "check.hpp"
#include "synthetic_roms.hpp"

#include <array>
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

/**
 * Fenêtre 0 sur tout l'écran, qui affiche BG0 et les objets mais coupe les
 * effets spéciaux (bit 5 de `WININ` à 0), comme sur la carte de Pokémon
 * Émeraude. BG0 est bleu, l'objet 8x8 en (0,0) est rouge ; BG0 est 2e cible
 * de `BLDCNT`, à parts égales dans `BLDALPHA`.
 */
void setup_window_without_effects(Fixture& fixture, int object_mode, int blend_control) {
    fixture.reg(0x00, 0x3140);                          // mode 0, BG0, objets, fenêtre 0, mappage 1D
    fixture.reg(0x08, 0x1080);                          // BG0 : 8 bpp, carte en 0x8000 (tuile 0 partout)
    for (int offset = 0; offset < 64; ++offset) fixture.bus.vram[static_cast<std::size_t>(offset)] = 1;
    for (int offset = 0; offset < 64; ++offset) fixture.bus.vram[static_cast<std::size_t>(0x10000 + offset)] = 1;
    fixture.palette(1, 0x7c00);                         // BG0 bleu
    fixture.palette(257, 0x001f);                       // objet rouge
    fixture.oam(0, 0x2000 | object_mode << 10);         // y = 0, 8 bpp
    fixture.oam(2, 0x0000);                             // x = 0, 8x8
    fixture.oam(4, 0x0000);                             // tuile 0, priorité 0
    fixture.reg(0x40, 0x00f0);                          // WIN0H : colonnes 0 à 239
    fixture.reg(0x44, 0x00a0);                          // WIN0V : lignes 0 à 159
    fixture.reg(0x48, 0x0011);                          // WININ : BG0 et objets, sans effets
    fixture.reg(0x4a, 0x0000);
    fixture.reg(0x50, blend_control);
    fixture.reg(0x52, 0x0808);                          // BLDALPHA : 8/16 et 8/16
}

/**
 * Un objet semi-transparent est toujours 1re cible de l'alpha blending
 * (GBATEK). Le bit qui coupe les effets d'une fenêtre ne l'en empêche pas :
 * sans cette règle, le brouillard de Pokémon Émeraude couvre tout le décor. Le même objet en mode normal, désigné 1re cible par
 * `BLDCNT`, reste en revanche opaque dans cette fenêtre.
 */
void un_objet_semi_transparent_se_melange_dans_une_fenetre_sans_effets() {
    Fixture semi;
    setup_window_without_effects(semi, 1, 0x0100);      // 2e cible BG0, aucun effet
    semi.render_frame();
    check(semi.pixel(0, 0) == static_cast<std::int32_t>(0xff7f007fU), "l'objet semi-transparent n'est pas mélangé au plan");
    check(semi.pixel(8, 0) == static_cast<std::int32_t>(0xff0000ffU), "le plan seul n'est plus bleu à côté de l'objet");

    Fixture normal;
    setup_window_without_effects(normal, 0, 0x0150);    // 1re cible objets, alpha, 2e cible BG0
    normal.render_frame();
    check(normal.pixel(0, 0) == static_cast<std::int32_t>(0xffff0000U), "la fenêtre sans effets n'empêche plus le mélange d'un objet normal");
}

/** Couleur affichée d'une entrée BGR555 (GBATEK) : 5 bits étendus à 8. */
std::int32_t argb(int color) {
    const auto expand = [](int value) { return static_cast<std::uint32_t>((value << 3) | (value >> 2)); };
    return static_cast<std::int32_t>(0xff000000U | expand(color & 31) << 16U | expand((color >> 5) & 31) << 8U |
                                     expand((color >> 10) & 31));
}
std::int32_t rgb(int red, int green, int blue) {
    return static_cast<std::int32_t>(0xff000000U | static_cast<std::uint32_t>(red) << 16U |
                                     static_cast<std::uint32_t>(green) << 8U | static_cast<std::uint32_t>(blue));
}
/** Entrée de carte de BG0 en colonne `column` de la première rangée (carte en 0x8000). */
void map_entry(Fixture& fixture, int column, int entry) {
    fixture.bus.vram[static_cast<std::size_t>(0x8000 + column * 2)] = static_cast<std::uint8_t>(entry);
    fixture.bus.vram[static_cast<std::size_t>(0x8000 + column * 2 + 1)] = static_cast<std::uint8_t>(entry >> 8);
}

/**
 * Une couleur changée en cours d'image ne vaut que pour les lignes suivantes,
 * même écrite droit en mémoire, comme le ferait un DMA ou un code de triche :
 * la palette convertie se relit avant chaque ligne tracée.
 */
void une_couleur_changee_en_cours_d_image_vaut_pour_les_lignes_suivantes() {
    Fixture fixture;
    setup_text_background(fixture, false, 0);
    constexpr int lines = 80;
    fixture.ppu.tick(lines * 1232);  // lignes 0 à 79 tracées
    fixture.palette(1, 0x7fff);
    fixture.ppu.tick(cycles_per_frame - lines * 1232);
    check(fixture.pixel(0, 72) == argb(1), "couleur changée après la ligne 79 appliquée à une ligne déjà tracée");
    check(fixture.pixel(0, 80) == argb(0x7fff), "couleur changée en mémoire ignorée par les lignes suivantes");
    check(fixture.pixel(1, 80) == argb(2), "une couleur inchangée a changé avec sa voisine");
}

/**
 * Plan texte 4 bpp : les huit points d'une ligne de tuile dans l'ordre, puis en
 * miroir, puis dans une autre banque de palette ; une ligne de tuile vide
 * laisse voir le fond. Défilé de 3 points, chaque tuile se coupe en deux
 * segments de la ligne, sans changer ce qui s'affiche.
 */
void un_plan_texte_4bpp_suit_miroir_banque_et_transparence() {
    Fixture fixture;
    fixture.reg(0x00, 0x0100);  // mode 0, BG0 actif
    fixture.reg(0x08, 0x1000);  // 4 bpp, tuiles en 0, carte en 0x8000
    // Tuile 1 : points 1 à 8 sur la ligne 0, ligne 1 vide, point 1 ailleurs.
    const std::array<std::uint8_t, 4> first_row{0x21, 0x43, 0x65, 0x87};
    for (std::size_t index = 0; index < 4; ++index) fixture.bus.vram[32 + index] = first_row[index];
    for (std::size_t index = 8; index < 32; ++index) fixture.bus.vram[32 + index] = 0x11;
    map_entry(fixture, 0, 0x0001);  // tuile 1
    map_entry(fixture, 1, 0x0401);  // tuile 1 en miroir horizontal
    map_entry(fixture, 2, 0x2001);  // tuile 1, banque de palette 2
    for (int index = 1; index <= 8; ++index) {
        fixture.palette(index, index * 3);
        fixture.palette(32 + index, 0x7c00 | index);
    }
    constexpr int backdrop = 0x03e0;
    fixture.palette(0, backdrop);
    const auto expected = [](int effective_x) {
        const auto px = effective_x % 8;
        switch (effective_x / 8) {
        case 0: return argb((px + 1) * 3);
        case 1: return argb((8 - px) * 3);
        case 2: return argb(0x7c00 | (px + 1));
        default: return argb(backdrop);
        }
    };
    fixture.render_frame();
    for (int x = 0; x < 32; ++x) {
        check(fixture.pixel(x, 0) == expected(x), "plan 4 bpp : ordre, miroir ou banque des points faux");
        check(fixture.pixel(x, 1) == argb(backdrop), "plan 4 bpp : une ligne de tuile vide masque le fond");
    }
    fixture.reg(0x10, 3);  // BG0HOFS : 3 points
    fixture.render_frame();
    for (int x = 0; x < 29; ++x)
        check(fixture.pixel(x, 0) == expected(x + 3), "plan 4 bpp défilé : segments de tuile mal raccordés");
}

/**
 * Plan texte 8 bpp dont les tuiles sont en 0xC000 : la tuile 767 finit sur le
 * dernier octet de la VRAM (0x17FFF) et s'affiche ; la tuile 768 est tout
 * entière au-delà, ses points se lisent 0, transparents.
 */
void une_ligne_8bpp_hors_de_la_vram_reste_transparente() {
    Fixture fixture;
    fixture.reg(0x00, 0x0100);  // mode 0, BG0 actif
    fixture.reg(0x08, 0x108c);  // 8 bpp, tuiles en 0xC000, carte en 0x8000
    for (std::size_t index = 0; index < 64; ++index) fixture.bus.vram[0xc000 + 767 * 64 + index] = 5;
    map_entry(fixture, 0, 767);
    map_entry(fixture, 1, 768);
    fixture.palette(5, 0x7fff);
    fixture.palette(0, 0x001f);
    fixture.render_frame();
    check(fixture.pixel(0, 0) == argb(0x7fff) && fixture.pixel(7, 7) == argb(0x7fff),
          "plan 8 bpp : la dernière tuile de la VRAM n'est pas affichée");
    check(fixture.pixel(8, 0) == argb(0x001f) && fixture.pixel(15, 7) == argb(0x001f),
          "plan 8 bpp : une tuile hors de la VRAM est affichée");
}

/**
 * Fenêtre 0 de la colonne 200 à la colonne 40, par le bord droit de l'écran, et
 * des lignes 10 à 19 : BG0 n'apparaît que dedans.
 */
void une_fenetre_qui_passe_par_le_bord_garde_ses_colonnes() {
    Fixture fixture;
    setup_text_background(fixture, false, 0);
    fixture.reg(0x00, 0x2100);             // mode 0, BG0 et fenêtre 0
    fixture.reg(0x40, (200 << 8) | 40);    // WIN0H
    fixture.reg(0x44, (10 << 8) | 20);     // WIN0V
    fixture.reg(0x48, 0x0001);             // WININ : BG0
    fixture.reg(0x4a, 0x0000);             // WINOUT : rien
    constexpr int backdrop = 0x7c00;       // bleu, absent du dégradé
    fixture.palette(0, backdrop);
    fixture.render_frame();
    const auto shown = [&](int x, int y) { return fixture.pixel(x, y) != argb(backdrop); };
    check(shown(0, 15) && shown(39, 15) && shown(200, 15) && shown(239, 15),
          "fenêtre par le bord de l'écran : colonnes du dedans masquées");
    check(!shown(40, 15) && !shown(120, 15) && !shown(199, 15), "fenêtre par le bord de l'écran : colonnes du dehors affichées");
    check(!shown(0, 9) && !shown(0, 20) && shown(0, 10) && shown(0, 19), "fenêtre : bornes verticales non respectées");
}

/**
 * BLDY : éclaircir ou assombrir de 8/16 un point dont les trois canaux valent
 * 8, soit 66 une fois étendus : 66 + (255 - 66) × 8 / 16 = 160, puis
 * 66 - 66 × 8 / 16 = 33 (GBATEK, « BLDY »).
 */
void l_eclaircissement_et_l_assombrissement_suivent_bldy() {
    Fixture fixture;
    setup_text_background(fixture, false, 0);
    fixture.palette(1, 0x2108);
    fixture.reg(0x54, 8);       // EVY = 8
    fixture.reg(0x50, 0x0081);  // BG0 première cible, éclaircir
    fixture.render_frame();
    check(fixture.pixel(0, 0) == rgb(160, 160, 160), "éclaircissement BLDY faux");
    fixture.reg(0x50, 0x00c1);  // assombrir
    fixture.render_frame();
    check(fixture.pixel(0, 0) == rgb(33, 33, 33), "assombrissement BLDY faux");
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
    un_objet_semi_transparent_se_melange_dans_une_fenetre_sans_effets();
    une_couleur_changee_en_cours_d_image_vaut_pour_les_lignes_suivantes();
    un_plan_texte_4bpp_suit_miroir_banque_et_transparence();
    une_ligne_8bpp_hors_de_la_vram_reste_transparente();
    une_fenetre_qui_passe_par_le_bord_garde_ses_colonnes();
    l_eclaircissement_et_l_assombrissement_suivent_bldy();
    return 0;
}
