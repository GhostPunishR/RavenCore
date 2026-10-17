# RavenCore

[![CI](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml/badge.svg)](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/GhostPunishR/RavenCore?label=Version)](https://github.com/GhostPunishR/RavenCore/releases)
[![Licence AGPL-3.0](https://img.shields.io/badge/Licence-AGPL--3.0-663399?logo=gnu&logoColor=white)](LICENSE)

Moteurs d'émulation **Game Boy, Game Boy Color et Game Boy Advance** écrits en **C++20**, sans cœur tiers.

Ce sont les cœurs maison de [RavenEmu](https://github.com/GhostPunishR/RavenEmu). Ce dépôt permet de les utiliser hors de l'application Android : dans **RetroArch** (Libretro), ou dans votre propre projet via une **API C** ou **C++**.

| Cœur | Console | État |
| --- | --- | --- |
| **RavenGB** | Game Boy | Avancé |
| **RavenGBC** | Game Boy Color | Avancé |
| **RavenGBA** | Game Boy Advance | Expérimental |

**Sommaire :** [Compiler](#compiler) · [RetroArch](#utiliser-avec-retroarch) · [Intégrer](#intégrer-dans-un-projet) · [Tests](#tests) · [Limites](#état-et-limites) · [Versions](#versions) · [Contribuer](#contribuer) · [Licence](#licence)

## Compiler

Prérequis : **CMake 3.22.1+** et un compilateur **C++20**.

```bash
git clone https://github.com/GhostPunishR/RavenCore.git
cd RavenCore
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Pour ne compiler que les moteurs, ajoutez `-DRAVENEMU_BUILD_LIBRETRO=OFF -DBUILD_TESTING=OFF`.

## Utiliser avec RetroArch

1. Téléchargez `ravenemu-libretro-vX.Y.Z-linux-x86_64.tar.gz` depuis les [releases](https://github.com/GhostPunishR/RavenCore/releases). Pour la version en développement, prenez l'artefact d'une [exécution CI](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml).
2. Copiez `cores/*.so` dans le dossier des cœurs de RetroArch, et `info/*.info` dans son dossier `info`.
3. Chargez le cœur voulu :

| Fichier | Console |
| --- | --- |
| `ravenemu_gb_libretro.so` | Game Boy (DMG) |
| `ravenemu_gbc_libretro.so` | Game Boy Color, compatible Game Boy |
| `ravenemu_gba_libretro.so` | Game Boy Advance |

Les cœurs gèrent la vibration (MBC5 Rumble, Drill Dozer, WarioWare: Twisted!) et les capteurs des cartouches, pilotés au stick analogique gauche :

| Capteur | Jeux | Commande |
| --- | --- | --- |
| Accéléromètre MBC7 | Kirby Tilt 'n' Tumble, Command Master | Stick gauche |
| Inclinaison GBA | Yoshi Topsy-Turvy, Koro Koro Puzzle | Stick gauche |
| Gyroscope GBA | WarioWare: Twisted! | Stick gauche, axe horizontal (à droite : sens horaire) |
| Capteur solaire GBA | Boktai 1 à 3 | Option `ravenemu_gba_solar_light` |

La sauvegarde et l'horloge d'un jeu GBA sont détectées automatiquement. Si la détection se trompe, deux options du cœur les imposent ; elles s'appliquent au chargement du jeu. La lumière du capteur solaire, elle, se règle à tout moment :

| Option | Valeurs |
| --- | --- |
| `ravenemu_gba_save_type` | `auto`, `sram`, `flash64`, `flash128`, `eeprom512`, `eeprom8k`, `none` |
| `ravenemu_gba_rtc` | `auto`, `enabled`, `disabled` |
| `ravenemu_gba_solar_light` | Pourcentage de plein soleil : `50` par défaut, de `0` à `100` par pas de 10 |

> Les binaires publiés visent uniquement **Linux x86-64**. Les cœurs ne sont pas proposés par le téléchargeur intégré de RetroArch.

## Intégrer dans un projet

| Interface | En-tête | Usage |
| --- | --- | --- |
| C++ | [`core.hpp`](cores/common/include/ravenemu/core.hpp) | Accès direct aux moteurs |
| SDK C++ | [`sdk.hpp`](sdk/include/ravenemu/sdk.hpp) | Fabrique simplifiée |
| C | [`bridge.h`](sdk/include/ravenemu/bridge.h) | Frontends non C++ |

Après `cmake --install`, utilisez le paquet CMake :

```cmake
find_package(RavenCore 0.5 REQUIRED)
target_link_libraries(mon_app PRIVATE RavenCore::ravenemu_c_api)  # ou RavenCore::ravencore en C++
```

L'API C couvre :

- le chargement de ROM, l'image, l'audio et les boutons ;
- les sauvegardes batterie et les états instantanés ;
- la vibration, et le type de sauvegarde et l'horloge d'un jeu GBA ;
- les capteurs des cartouches : lumière, rotation et inclinaison ;
- le câble link entre consoles d'une même application (deux jeux sur un même appareil) ;
- la lecture de la mémoire surveillée par RetroAchievements.

Détails dans le [README du SDK](sdk/README.md).

## Tests

Les tests de base n'utilisent aucune ROM. La CI exécute en plus des suites de test publiques, téléchargées à la volée et vérifiées par SHA-256. Aucune ROM n'est stockée dans le dépôt.

| Console | Suites | Documentation |
| --- | --- | --- |
| GB / GBC | Mooneye Test Suite, Blargg, dmg-acid2, cgb-acid2 (198 tests) | [Tests GB/GBC](cores/gb/tests/README.md) |
| GBA | jsmolka/gba-tests, FuzzARM | [Tests GBA](cores/gba/tests/README.md) |

Pour les lancer en local :

```bash
cores/gb/tests/fetch_test_roms.sh build/gb-test-roms
cores/gba/tests/fetch_test_roms.sh build/gba-test-roms
cmake -S . -B build -DRAVENEMU_GB_TEST_ROMS=build/gb-test-roms -DRAVENEMU_GBA_TEST_ROMS=build/gba-test-roms
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## État et limites

- **GB / GBC** : les 198 tests publics retenus passent. Des suites plus exigeantes (Mealybug Tearoom, SameSuite, gbmicrotest) échouent encore en partie.
- **GBA** : expérimental. Les suites gba-tests et FuzzARM passent, mais le cœur n'a pas encore été essayé sur un large panel de jeux. Non émulés pour l'instant :
  - le câble link avec une autre application ou une autre machine, le mode UART et l'adaptateur sans fil ;
  - l'exception d'instruction indéfinie.
- **Capteurs** : ceux des cartouches GBA suivent les mesures publiées par GBATEK, mais n'ont pas encore été essayés en jeu. Le sens des axes d'inclinaison (GBA et MBC7) et l'échelle du gyroscope restent à confirmer.
- **RetroAchievements** : pas encore activé dans les cœurs Libretro, et le mode Hardcore n'est pas pris en charge. L'API C expose déjà la mémoire nécessaire. L'intégration de [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu) est distincte.
- **États instantanés** : un état ne se recharge pas dans une version plus ancienne que celle qui l'a créé. Les changements de format sont listés dans le [journal des modifications](CHANGELOG.md).

## Versions

Version actuelle : **0.5.2**. Chaque version est publiée sur la page des [releases](https://github.com/GhostPunishR/RavenCore/releases) avec les cœurs Libretro Linux. Ses évolutions sont détaillées dans le [journal des modifications](CHANGELOG.md).

Avant la 1.0, une version peut changer l'API ou le format des états ; le journal le signale à chaque fois.

## Contribuer

Les contributions sont les bienvenues. Lisez le [guide de contribution](CONTRIBUTING.md), qui couvre l'organisation du code, les règles et les pull requests, ainsi que le [code de conduite](CODE_OF_CONDUCT.md). L'essentiel :

- une correction matérielle s'accompagne d'un test de régression ;
- aucun code repris d'un autre émulateur, aucune ROM ni aucun BIOS ;
- tout changement notable va dans la section « Non publié » du [journal](CHANGELOG.md).

## Licence

Projet développé par **GhostPunishR** sous licence [GNU AGPL-3.0](LICENSE). Le fichier `libretro.h` conserve sa licence MIT. Voir [NOTICE.md](NOTICE.md) pour les attributions.

Aucune ROM ni aucun BIOS commercial n'est fourni. RavenCore n'est affilié ni à Nintendo, ni à RetroArch, ni à RetroAchievements.
