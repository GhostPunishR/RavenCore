#include "gpu/geometry/geometry_engine.hpp"
#include "gpu/geometry/matrix.hpp"

#include "check.hpp"

#include <cstdint>
#include <initializer_list>

// Moteur de géométrie de l'ARM9, d'après GBATEK (« DS 3D Geometry
// Commands », « DS 3D Matrix Load/Multiply », « DS 3D Matrix Stack », « DS
// 3D Status », « DS 3D Tests »), piloté par ses registres comme le ferait le
// CPU. Nombres en virgule fixe : 1,0 = 1000h.
namespace {

using namespace ravenemu::nds;
using namespace ravenemu::nds::geometry;
using ravenemu::testing::check;

constexpr std::uint32_t all = 0xffff'ffffU;
constexpr std::int32_t fixed(double value) { return static_cast<std::int32_t>(value * 4096); }
constexpr std::uint32_t fx(double value) { return static_cast<std::uint32_t>(fixed(value)); }  // paramètre envoyé

// Ports des commandes : (commande × 4) + 400h, depuis 0x04000400.
constexpr std::uint32_t port(unsigned command) { return command * 4U; }
constexpr unsigned mtx_mode = 0x10, mtx_push = 0x11, mtx_pop = 0x12, mtx_store = 0x13, mtx_restore = 0x14,
                   mtx_identity = 0x15, mtx_load_4x4 = 0x16, mtx_load_4x3 = 0x17, mtx_mult_3x3 = 0x1a, mtx_scale = 0x1b,
                   mtx_trans = 0x1c,
                   swap_buffers = 0x50, box_test = 0x70, pos_test = 0x71, vec_test = 0x72;
constexpr std::uint32_t gxstat = 0x200;
constexpr Cycles later = 10'000;  // assez pour finir les commandes envoyées

struct Bench {
    GeometryEngine engine;
    Cycles now{};

    Cycles send(unsigned command, std::initializer_list<std::uint32_t> parameters = {}) {
        Cycles resume = now;
        if (parameters.size() == 0) resume = engine.write(port(command), 0, all, now);
        for (const auto parameter : parameters) resume = engine.write(port(command), parameter, all, now);
        return resume;
    }
    std::uint32_t status() { return engine.read(gxstat, now); }
    void wait() {
        now += later;
        engine.run(now);
    }
    std::int32_t clip(unsigned index) { return static_cast<std::int32_t>(engine.read(0x240 + index * 4, now)); }
};

void matrix_maths_test() {
    const auto s = scale(std::initializer_list<std::uint32_t>{fx(2), fx(3), fx(4)});
    const auto t = translation(std::initializer_list<std::uint32_t>{fx(1), fx(2), fx(3)});
    check(multiply(identity(), t) == t && multiply(t, identity()) == t, "identité neutre");
    const auto st = multiply(s, t);
    const auto ts = multiply(t, s);
    check(st[12] == fixed(1) && st[13] == fixed(2) && st[14] == fixed(3) && st[0] == fixed(2),
          "S × T : la translation reste entière");
    check(ts[12] == fixed(2) && ts[13] == fixed(6) && ts[14] == fixed(12), "T × S : la translation est mise à l'échelle");
    auto tiny = identity();
    tiny[0] = 1;
    check(transform({-1, 0, 0, 0}, tiny)[0] == -1 && transform({1, 0, 0, 0}, tiny)[0] == 0 &&
              multiply(s, s)[0] == fixed(4),
          "produits de 24 bits de fraction, décalés de 12 (arrondi vers le bas)");
}

void matrix_commands_test() {
    Bench bench;
    bench.send(mtx_mode, {1});
    bench.send(mtx_scale, {fx(2), fx(2), fx(2)});
    bench.send(mtx_trans, {fx(1), 0, 0});
    bench.wait();
    check(bench.clip(12) == fixed(2) && bench.clip(0) == fixed(2), "C = M × C : translation après l'échelle, mise à l'échelle");
    bench.send(mtx_mode, {0});
    bench.send(mtx_trans, {0, fx(1), 0});
    bench.wait();
    check(bench.clip(13) == fixed(1) && bench.clip(12) == fixed(2), "découpage = position × projection");

    Bench both;
    both.send(mtx_mode, {2});
    both.send(mtx_scale, {fx(2), fx(2), fx(2)});
    both.send(mtx_mult_3x3, {0, fx(1), 0, fx(-1), 0, 0, 0, 0, fx(1)});  // quart de tour
    both.send(mtx_trans, {fx(5), 0, 0});
    both.wait();
    const auto vector = [&](unsigned index) { return static_cast<std::int32_t>(both.engine.read(0x280 + index * 4, both.now)); };
    check(vector(1) == fixed(1) && vector(3) == fixed(-1) && vector(0) == 0 && vector(8) == fixed(1),
          "mode 2 : rotation sur la matrice des vecteurs, sans l'échelle ; VECMTX en 3 × 3");
    // La dernière commande s'applique la première au sommet : translation, puis
    // rotation et échelle.
    check(both.clip(1) == fixed(2) && both.clip(12) == 0 && both.clip(13) == fixed(10),
          "mode 2 : la position prend l'échelle et la translation");
    both.send(mtx_mode, {3});
    both.send(mtx_identity);
    both.wait();
    check(both.clip(1) == fixed(2), "mode 3 : la texture ne touche pas le découpage");

    Bench load;
    load.send(mtx_mode, {1});
    load.send(mtx_load_4x3, {fx(2), 0, 0, 0, fx(2), 0, 0, 0, fx(2), fx(1), fx(2), fx(3)});
    load.wait();
    check(load.clip(0) == fixed(2) && load.clip(3) == 0 && load.clip(12) == fixed(1) && load.clip(14) == fixed(3) &&
              load.clip(15) == fixed(1),
          "MTX_LOAD_4x3 : quatrième colonne 0, 0, 0, 1,0");
}

void stack_test() {
    Bench bench;
    const auto level = [&] { return (bench.status() >> 8U) & 31U; };
    bench.send(mtx_mode, {1});
    bench.send(mtx_trans, {fx(1), 0, 0});
    bench.send(mtx_push);
    bench.send(mtx_trans, {fx(1), 0, 0});
    bench.send(mtx_push);
    bench.send(mtx_identity);
    bench.wait();
    check(level() == 2 && bench.clip(12) == 0, "deux PUSH : niveau 2");
    bench.send(mtx_pop, {2});
    bench.wait();
    check(level() == 0 && bench.clip(12) == fixed(1), "POP 2 : la première matrice poussée");
    bench.send(mtx_pop, {0x3f});  // -1
    bench.wait();
    check(level() == 1 && bench.clip(12) == fixed(2), "POP -1 : la matrice libérée revient");
    bench.send(mtx_store, {5});
    bench.send(mtx_identity);
    bench.send(mtx_restore, {5});
    bench.wait();
    check(bench.clip(12) == fixed(2) && level() == 1 && (bench.status() & 0x8000U) == 0, "STORE et RESTORE, pointeur inchangé");
    bench.send(mtx_store, {31});
    bench.wait();
    check((bench.status() & 0x8000U) != 0, "STORE 31 : erreur de pile");
    bench.engine.write(gxstat, 0x8000, all, bench.now);
    check((bench.status() & 0x8000U) == 0, "GXSTAT bit 15 écrit à 1 : erreur effacée");
    for (int push = 0; push < 30; ++push) bench.send(mtx_push);
    bench.wait();
    check(level() == 31 && (bench.status() & 0x8000U) == 0, "31 entrées : pas d'erreur");
    bench.send(mtx_push);
    bench.wait();
    check((bench.status() & 0x8000U) != 0, "PUSH sur l'entrée 31 : erreur");

    Bench projection;
    projection.send(mtx_mode, {0});
    projection.send(mtx_trans, {fx(3), 0, 0});
    projection.send(mtx_push);
    projection.send(mtx_identity);
    projection.wait();
    check((projection.status() & 0x2000U) != 0 && (projection.status() & 0x8000U) == 0, "projection : une entrée, niveau 1");
    projection.send(mtx_push);
    projection.wait();
    check((projection.status() & 0xa000U) == 0x8000U, "deuxième PUSH : erreur, le pointeur de 1 bit revient à 0");
    projection.send(mtx_pop, {7});  // paramètre ignoré
    projection.wait();
    check(projection.clip(12) == 0 && (projection.status() & 0x2000U) != 0, "POP de projection : un niveau, quel que soit le paramètre");
    projection.engine.write(gxstat, 0x8000, all, projection.now);
    check((projection.status() & 0xa000U) == 0, "acquittement : erreur effacée, pointeur de projection à 0");
    projection.send(mtx_pop, {1});
    projection.wait();
    check((projection.status() & 0xa000U) == 0xa000U, "POP de la pile de projection vide : erreur, pointeur à 1");

    Bench busy;
    busy.send(mtx_push);
    check((busy.status() & 0x4000U) != 0, "PUSH en cours : GXSTAT bit 14");
    busy.now += 17;
    check((busy.status() & 0x4000U) == 0, "PUSH fini (17 cycles)");
}

void packed_commands_test() {
    Bench bench;
    for (const auto word : {0x001c'0510U, 1U, fx(1), fx(2), fx(3), 0U}) bench.engine.write(0, word, all, bench.now);
    bench.wait();
    check(bench.clip(12) == fixed(1) && bench.clip(13) == fixed(2) && bench.clip(14) == fixed(3) && bench.status() == 0x0600'0000U,
          "GXFIFO groupé : MTX_MODE, commande 05h ignorée, MTX_TRANS ; mot nul ignoré");
    for (const auto word : {0x1515'1115U, 0x0000'1214U, 0U, 1U}) bench.engine.write(0x3c, word, all, bench.now);  // miroir
    bench.wait();
    check(bench.clip(12) == 0 && ((bench.status() >> 8U) & 31U) == 0, "commandes sans paramètre groupées, puis RESTORE et POP");
    bench.engine.write(0, 0x0000'0505U, all, bench.now);
    check(bench.status() == 0x0600'0000U, "commandes invalides : rien dans le FIFO");
}

void timing_test() {
    Bench bench;
    for (unsigned index = 0; index < 15; ++index) bench.engine.write(port(mtx_load_4x4), fx(1), all, bench.now);
    check(((bench.status() >> 16U) & 0x1ffU) == 11 && (bench.status() & 0x0800'0000U) != 0,
          "15 paramètres sur 16 : 4 dans le PIPE, 11 dans le FIFO, moteur occupé");
    bench.engine.write(port(mtx_load_4x4), fx(1), all, bench.now);
    bench.now += 33;
    check((bench.status() & 0x0800'0000U) != 0 && (bench.status() & 0x0400'0000U) != 0, "MTX_LOAD_4x4 : 34 cycles");
    bench.now += 1;
    check((bench.status() & 0x0800'0000U) == 0, "fini au 34e cycle");
    bench.send(mtx_mode, {2});
    bench.send(mtx_trans, {0, 0, 0});
    bench.now += 1 + 22 + 30 - 1;
    check((bench.status() & 0x0800'0000U) != 0, "MTX_TRANS en mode 2 : 30 cycles de plus");
    bench.now += 1;
    check((bench.status() & 0x0800'0000U) == 0, "fini");

    Bench full;
    for (unsigned index = 1; index <= 261; ++index)
        check(full.engine.write(port(mtx_identity), 0, all, full.now) == full.now, "place libre : pas d'attente");
    check(((full.status() >> 16U) & 0x1ffU) == 256 && (full.status() & 0x0100'0000U) != 0, "GXSTAT : 256 entrées, plein");
    check((full.status() & 0x0200'0000U) == 0 && !full.engine.wants_dma(), "plus de la moitié : ni bit 25, ni DMA");
    // À 19 cycles, la 2e commande laisse 3 entrées au PIPE : rien ne quitte le
    // FIFO (il faut moins de 3). À 38, la 3e en laisse 2 : 2 entrées passent.
    check(full.engine.write(port(mtx_identity), 0, all, full.now) == 38,
          "FIFO et PIPE pleins (260) : la 262e écriture attend qu'une place se libère");
}

void irq_test() {
    Bench bench;
    bench.engine.write(gxstat, 0x8000'0000U, all, bench.now);  // IRQ quand le FIFO est vide
    check(bench.engine.irq_condition() && (bench.status() >> 30U) == 2, "FIFO vide : condition vraie, bits 30-31 relus");
    for (int index = 0; index < 10; ++index) bench.send(mtx_identity);
    check(!bench.engine.irq_condition(), "FIFO occupé : condition fausse");
    bench.wait();
    check(bench.engine.irq_condition(), "vidé : condition vraie");
    bench.engine.write(gxstat, 0x4000'0000U, all, bench.now);  // moins de la moitié
    for (int index = 0; index < 141; ++index) bench.send(mtx_identity);  // une s'exécute, 4 au PIPE, 136 au FIFO
    check(!bench.engine.irq_condition() && !bench.engine.wants_dma(), "136 entrées dans le FIFO : plus de la moitié");
    bench.now += 19 * 9;
    check(((bench.status() >> 16U) & 0x1ffU) == 128 && !bench.engine.irq_condition() && !bench.engine.wants_dma(),
          "128 entrées : pas moins de la moitié");
    bench.now += 19;
    check(((bench.status() >> 16U) & 0x1ffU) == 126 && bench.engine.irq_condition() && bench.engine.wants_dma(),
          "126 entrées : condition vraie, DMA demandée");
}

void swap_buffers_test() {
    Bench bench;
    bench.send(mtx_mode, {1});
    bench.send(mtx_trans, {fx(1), 0, 0});
    bench.send(swap_buffers, {0});
    bench.send(mtx_identity);
    bench.wait();
    check(bench.clip(12) == fixed(1) && (bench.status() & 0x0800'0000U) != 0, "SWAP_BUFFERS : le moteur attend le V-Blank");
    bench.engine.on_vblank(bench.now);
    bench.now += 391;
    check(bench.clip(12) == fixed(1), "V-Blank, puis 392 cycles");
    bench.now += 1;
    check(bench.clip(12) == 0 && (bench.status() & 0x0800'0000U) != 0, "la commande suivante commence ensuite (effet dès son début)");
    bench.now += 19;
    check((bench.status() & 0x0800'0000U) == 0, "et finit 19 cycles plus tard");
}

void tests_test() {
    Bench bench;
    bench.send(mtx_mode, {2});
    bench.send(mtx_trans, {fx(1), fx(2), fx(3)});
    bench.wait();
    bench.send(pos_test, {fx(0.5) & 0xffffU, 0});
    check((bench.status() & 1U) != 0, "POS_TEST en cours : bit 0");
    bench.wait();
    const auto result = [&](unsigned index) { return static_cast<std::int32_t>(bench.engine.read(0x220 + index * 4, bench.now)); };
    check(result(0) == fixed(1.5) && result(1) == fixed(2) && result(2) == fixed(3) && result(3) == fixed(1) && (bench.status() & 1U) == 0,
          "POS_TEST : (x, y, z, 1) × découpage");
    bench.send(vec_test, {256U | (0x300U << 10U)});  // x = 0,5, y = -0,5 (9 bits de fraction)
    bench.wait();
    check(bench.engine.read(0x230, bench.now) == (0x0800U | (0xf800U << 16U)) && (bench.engine.read(0x234, bench.now) & 0xffffU) == 0,
          "VEC_TEST : 12 bits de fraction, signe sur 4 bits");
    bench.send(mtx_scale, {fx(2), fx(2), fx(2)});  // ne touche pas les vecteurs
    bench.send(mtx_mult_3x3, {fx(2), 0, 0, 0, fx(2), 0, 0, 0, fx(2)});
    bench.send(vec_test, {256U});
    bench.wait();
    check((bench.engine.read(0x230, bench.now) & 0xffffU) == 0xf000U, "VEC_TEST : 1,0 déborde en -1,0, comme le décrit GBATEK");
    bench.send(box_test, {0, 0, 0});
    bench.wait();
    check((bench.status() & 2U) != 0, "BOX_TEST : « dedans », sans découpage encore");
}

} // namespace

int main() {
    matrix_maths_test();
    matrix_commands_test();
    stack_test();
    packed_commands_test();
    timing_test();
    irq_test();
    swap_buffers_test();
    tests_test();
    return 0;
}
