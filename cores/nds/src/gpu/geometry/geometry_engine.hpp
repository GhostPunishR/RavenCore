#pragma once

#include "gpu/geometry/command_fifo.hpp"
#include "gpu/geometry/matrix_unit.hpp"
#include "machine/scheduler.hpp"

#include <array>
#include <cstdint>

// Moteur de géométrie de l'ARM9 (GBATEK, « DS 3D Overview », « DS 3D I/O
// Map », « DS 3D Geometry Commands », « DS 3D Status », « DS 3D Tests ») :
// - commandes envoyées à GXFIFO (0x04000400 à 0x0400043F), groupées ou non,
//   ou à leurs ports (0x04000440 à 0x040005FF : commande = (port - 400h) / 4) ;
// - une commande attend tous ses paramètres, puis s'exécute pendant la
//   durée de la table de GBATEK (cycles de 33,51 MHz) ; MTX_MULT et
//   MTX_TRANS prennent 30 cycles de plus en mode 2. Ses effets valent dès
//   son début ;
// - SWAP_BUFFERS arrête le moteur jusqu'au V-Blank, puis 392 cycles ;
// - un CPU qui écrit dans le FIFO plein attend qu'une place se libère. S'il
//   attend le V-Blank, l'entrée est tout de même gardée (le CPU ne peut pas
//   attendre ici l'affichage), et GXSTAT compte au plus 256 entrées ;
// - GXSTAT (0x04000600) : bit 0 test en cours, bit 1 résultat de BOX_TEST,
//   bits 8-13 niveaux des piles, bit 14 PUSH ou POP en cours, bit 15 erreur
//   de pile, bits 16-26 état du FIFO, bit 27 moteur occupé, bits 30-31 IRQ
//   du FIFO (1 : moins de la moitié, 2 : vide), lue comme un niveau ;
// - POS_TEST et VEC_TEST rendent leurs résultats (0x04000620, 0x04000630) ;
//   CLIPMTX_RESULT et VECMTX_RESULT (0x04000640, 0x04000680) relisent les
//   matrices.
// Les commandes de sommets, de polygones, de textures et de lumières, et
// VIEWPORT, prennent leurs paramètres et leur durée, sans effet encore.
// BOX_TEST répond « dedans » : le découpage n'existe pas encore.
namespace ravenemu::nds::geometry {

class GeometryEngine {
public:
    // E/S de 0x04000400 à 0x040006A3, `offset` depuis 0x04000400. Une
    // écriture rend la date où le CPU peut continuer (au moins `now`).
    Cycles write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now);
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, Cycles now);

    // Exécute les commandes jusqu'à `now`.
    void run(Cycles now);
    void on_vblank(Cycles now);
    // Fin de la commande en cours, sinon jamais.
    [[nodiscard]] Cycles next_event() const noexcept { return busy_until_ > time_ ? busy_until_ : Scheduler::never; }
    // IRQ 21 (niveau) et départ de la DMA de géométrie (moins de la moitié).
    [[nodiscard]] bool irq_condition() const noexcept;
    [[nodiscard]] bool wants_dma() const noexcept { return fifo_.fifo_count() < CommandFifo::fifo_capacity / 2; }

    [[nodiscard]] const MatrixUnit& matrices() const noexcept { return matrices_; }
    [[nodiscard]] std::uint32_t status() const noexcept;

private:
    void push(Entry entry, Cycles now);
    void execute(std::uint8_t command, const std::array<std::uint32_t, 32>& parameters);
    [[nodiscard]] Cycles duration(std::uint8_t command) const noexcept;
    [[nodiscard]] bool busy() const noexcept { return busy_until_ > time_ || waiting_vblank_ || !fifo_.empty(); }

    CommandFifo fifo_;
    PackedCommands packed_;
    MatrixUnit matrices_;
    Cycles time_{};
    Cycles busy_until_{};
    std::uint8_t executing_{};
    bool waiting_vblank_{};
    std::uint32_t irq_mode_{};
    bool box_result_{};
    std::array<std::int32_t, 4> position_result_{};
    std::array<std::uint16_t, 3> vector_result_{};
};

} // namespace ravenemu::nds::geometry
