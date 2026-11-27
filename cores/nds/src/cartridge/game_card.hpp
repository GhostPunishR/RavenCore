#pragma once

#include "cartridge/header.hpp"
#include "cartridge/save_memory.hpp"
#include "interrupts/interrupt_controller.hpp"
#include "machine/scheduler.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Bus de la cartouche DS (GBATEK, « DS Cartridge I/O Ports », « DS Cartridge
// Protocol ») : AUXSPICNT (0x040001A0), ROMCTRL (0x040001A4), commande de
// 8 octets (0x040001A8, premier octet à l'adresse basse), mots reçus à
// 0x04100010. Le CPU qui a le slot (EXMEMCNT bit 11) seul y accède.
//
// Après le démarrage direct, la cartouche est en mode de lecture principal :
// - B7 lit les données à partir de l'adresse des octets 1 à 4 (poids fort en
//   tête). Le flux reste dans son bloc de 4 Kio ; au-delà de la puce, les
//   adresses bouclent ; 0 à 0x7FFF lisent 0x8000 + (adresse & 0x1FF) ;
// - B8 rend l'identifiant de la puce, répété ;
// - toute autre commande rend des 0.
// Le chiffrement KEY2 n'est pas modélisé : les données arrivent en clair,
// comme le DS les rend une fois déchiffrées. Les graines (0x040001B0-BB) sont
// ignorées.
//
// Temps, en périodes d'horloge de 5 ou 8 cycles système (ROMCTRL bit 27) :
// 8 octets de commande, gap1 (bits 0-12), puis 4 octets par mot, et gap2
// (bits 16-21) tous les 0x200 octets. Un mot prêt lève ROMCTRL bit 23 (DRQ,
// qui déclenche la DMA « cartouche ») ; le suivant part quand il est lu.
// Le dernier mot lu, ROMCTRL bit 31 retombe et l'IRQ 19 part si AUXSPICNT
// bit 14 la demande.
// Sauvegarde : en mode SPI (AUXSPICNT bits 13 et 15), chaque écriture
// d'AUXSPIDATA échange un octet avec la puce (save_memory.hpp) et la laisse
// sélectionnée si AUXSPICNT bit 6 est levé ; le transfert occupe le bus
// (bit 7) 8 bits à 4 MHz >> débit (bits 0-1), soit 64 << débit cycles.
// Pas encore : protocole avant le mode principal (KEY1, zone sécurisée),
// écriture des cartouches NAND.
namespace ravenemu::nds {

// Identifiant de puce annoncé pour une ROM : fabricant C2h (Macronix), puis
// taille en Mio - 1, d'après la capacité de l'en-tête.
std::uint32_t cartridge_chip_id(const CartridgeHeader& header) noexcept;

class GameCard {
public:
    void insert(std::vector<std::uint8_t> rom, std::uint32_t chip_id);
    [[nodiscard]] std::span<const std::uint8_t> rom() const noexcept { return rom_; }
    [[nodiscard]] std::uint32_t chip_id() const noexcept { return chip_id_; }
    [[nodiscard]] SaveMemory& save() noexcept { return save_; }
    [[nodiscard]] const SaveMemory& save() const noexcept { return save_; }

    // Mots d'E/S de 0x040001A0 à 0x040001BF, `offset` depuis 0x040001A0.
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, Cycles now) const noexcept;
    // `instruction` : adresse de l'instruction qui écrit (détection du type
    // de la puce de sauvegarde).
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now, Scheduler& scheduler,
               std::uint32_t instruction = 0);
    // 0x04100010 : le mot reçu. Lire un mot prêt lance le suivant, ou finit
    // le transfert.
    std::uint32_t read_data(Cycles now, Scheduler& scheduler, InterruptController& irq);
    // Event::card : rend true quand un mot est prêt (DRQ) ; un transfert sans
    // données se termine là.
    bool on_event(InterruptController& irq);

private:
    [[nodiscard]] Cycles clock() const noexcept { return (rom_control_ & 0x0800'0000U) != 0 ? 8U : 5U; }
    [[nodiscard]] std::uint8_t stream_byte(std::uint32_t index) const noexcept;
    void start(Cycles now, Scheduler& scheduler);
    void finish(InterruptController& irq) noexcept;
    void write_spi_control(std::uint32_t value, std::uint32_t mask);
    void spi_transfer(std::uint8_t value, Cycles now, std::uint32_t instruction);

    std::vector<std::uint8_t> rom_;
    std::uint32_t chip_id_{};
    std::uint32_t chip_mask_{};
    std::uint32_t spi_control_{};  // AUXSPICNT
    std::uint32_t rom_control_{};  // ROMCTRL
    std::array<std::uint8_t, 8> command_{};
    std::uint32_t length_{};    // octets du transfert en cours
    std::uint32_t position_{};  // octets déjà mis dans un mot
    std::uint32_t data_{};      // dernier mot reçu
    SaveMemory save_;
    std::uint8_t spi_data_{};   // AUXSPIDATA : dernier octet reçu de la puce
    Cycles spi_busy_until_{};
};

} // namespace ravenemu::nds
