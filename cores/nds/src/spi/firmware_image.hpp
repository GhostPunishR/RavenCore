#pragma once

#include "machine/user_settings.hpp"

#include <cstdint>
#include <vector>

// Contenu de la Flash du firmware (256 Kio), généré par RavenCore (GBATEK,
// « DS Firmware Header », « DS Firmware Wifi Calibration Data », « DS
// Firmware User Settings ») : aucun octet du firmware de Nintendo, et pas de
// code de démarrage, inutile au démarrage direct.
// - En-tête : identifiant « RAVN », taille de la puce (256 Kio), console DS
//   (1Dh = FFh), réglages utilisateur à 3FE00h (020h = 7FC0h).
// - Calibrage Wi-Fi : longueur 138h, adresse MAC locale (02h en tête), canaux
//   1 à 13, puce radio de type 2 ; CRC16 de valeur initiale 0.
// - Réglages utilisateur en deux exemplaires (3FE00h, compteur 0 ; 3FF00h,
//   compteur 1, donc le plus récent), CRC16 des 70h premiers octets. GBATEK
//   ne donne pas la valeur initiale de ce CRC : 0xFFFF, choix de RavenCore.
// Le reste vaut FFh, comme une Flash effacée.
namespace ravenemu::nds {

std::vector<std::uint8_t> firmware_image(const UserSettings& settings);

} // namespace ravenemu::nds
