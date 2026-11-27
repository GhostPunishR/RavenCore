#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Puce de sauvegarde d'une cartouche DS, sur le SPI auxiliaire (GBATEK, « DS
// Cartridge Backup », « DS Firmware Serial Flash Memory ») : EEPROM, FRAM ou
// Flash. Un octet par transfert ; le premier octet après la sélection est la
// commande, puis viennent l'adresse (poids fort en tête) et les données.
// - Toutes : WREN (06h), WRDI (04h), RDSR (05h, état répété), RDID (9Fh :
//   identifiant JEDEC sur la Flash, FFh ailleurs).
// - EEPROM de 0,5 Kio : adresse d'un octet, RDLO/RDHI (03h/0Bh) et WRLO/WRHI
//   (02h/0Ah) pour 000h-0FFh/100h-1FFh ; état à bits 4-7 levés.
// - EEPROM de 8 à 64 Kio et FRAM : adresse de 2 octets ; 128 Kio : 3 octets ;
//   RD (03h), WR (02h), WRSR (01h) pour les bits WP (2-3) et SRWD (7).
// - Flash : adresse de 3 octets ; READ (03h), FAST (0Bh, un octet muet), PW
//   (0Ah, écrit), PP (02h, ne fait que passer des bits à 0), PE (DBh, page de
//   0x100 octets), SE (D8h, secteur de 0x10000 octets), DP/RDP (B9h/ABh).
// L'écriture se fait sans délai (WIP toujours à 0), boucle dans la page (16,
// 32, 128 ou 256 octets ; pas de limite sur la FRAM), et exige WEL, que la fin
// de la commande remet à 0. Les adresses bouclent sur la taille de la puce.
// L'EEPROM de 128 Kio a des pages de 256 octets : GBATEK n'en donne pas la
// taille, choix de RavenCore.
namespace ravenemu::nds {

enum class SaveType : std::uint8_t {
    none,
    eeprom_512,
    eeprom_8k,
    eeprom_64k,
    eeprom_128k,
    fram_8k,
    fram_32k,
    flash_256k,
    flash_512k,
    flash_1m,
    flash_8m,
};

// Taille en octets d'une puce, et le type qu'annonce la taille d'un fichier
// de sauvegarde (8 Kio : EEPROM ; none si la taille ne correspond à rien).
[[nodiscard]] std::size_t save_size(SaveType type) noexcept;
[[nodiscard]] SaveType save_type_for_size(std::size_t size) noexcept;

class SaveMemory {
public:
    // Puce vierge (0xFF) du type donné.
    void set_type(SaveType type);
    [[nodiscard]] SaveType type() const noexcept { return type_; }
    // Fichier de sauvegarde : son type vient de sa taille quand aucun n'est
    // choisi ; sinon il doit avoir la taille de la puce. Rend false s'il est refusé.
    bool load(std::span<const std::uint8_t> data);
    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return data_; }
    // Vrai après une écriture, jusqu'à `clear_dirty` (l'hôte enregistre alors).
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void clear_dirty() noexcept { dirty_ = false; }

    // Un octet sur le bus : rend l'octet reçu de la puce.
    std::uint8_t transfer(std::uint8_t value);
    // Fin de la commande (chip select relâché).
    void deselect() noexcept;

private:
    [[nodiscard]] bool flash() const noexcept;
    [[nodiscard]] unsigned address_bytes() const noexcept;
    [[nodiscard]] std::uint32_t page_size() const noexcept;
    [[nodiscard]] std::uint8_t status() const noexcept;
    [[nodiscard]] bool protected_address(std::uint32_t address) const noexcept;
    void start(std::uint8_t command) noexcept;
    std::uint8_t data_byte(std::uint8_t value);
    void write_byte(std::uint8_t value, bool program_only);
    void erase(std::uint32_t size);

    SaveType type_{SaveType::none};
    std::vector<std::uint8_t> data_;
    bool selected_{};
    bool powered_down_{};
    bool write_enable_{};    // WEL
    std::uint8_t protect_{}; // bits WP (2-3) et SRWD (7) de l'état
    std::uint8_t command_{};
    unsigned received_{};    // octets reçus après la commande
    std::uint32_t address_{};
    bool dirty_{};
};

} // namespace ravenemu::nds
