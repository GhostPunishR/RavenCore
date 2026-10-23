# RavenCore

[![CI](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml/badge.svg)](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml)
[![Anthropic](https://img.shields.io/badge/Anthropic-Claude-D97757?logo=anthropic&logoColor=white)](https://www.anthropic.com/)
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

1. Téléchargez l'archive de votre système depuis les [releases](https://github.com/GhostPunishR/RavenCore/releases). Pour la version en développement, prenez l'artefact d'une [exécution CI](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml).

   | Système | Archive |
   | --- | --- |
   | Linux x86-64 | `ravenemu-libretro-vX.Y.Z-linux-x86_64.tar.gz` |
   | Windows x86-64 | `ravenemu-libretro-vX.Y.Z-windows-x86_64.zip` |
   | macOS 11 ou plus récent, Apple Silicon et Intel | `ravenemu-libretro-vX.Y.Z-macos-universal.zip` |
   | Android 64 bits | `ravenemu-libretro-vX.Y.Z-android-arm64-v8a.zip` |
   | Android 32 bits | `ravenemu-libretro-vX.Y.Z-android-armeabi-v7a.zip` |

2. Installez les cœurs :
   - **Linux, Windows, macOS** : copiez le contenu de `cores/` dans le dossier des cœurs, et celui de `info/` dans le dossier des informations des cœurs (RetroArch les indique dans **Paramètres > Dossiers**). Sous macOS, si RetroArch refuse un cœur téléchargé, retirez la quarantaine du dossier des cœurs : `xattr -dr com.apple.quarantine <dossier>`.
   - **Android** : dans RetroArch, **Charger un cœur > Installer ou restaurer un cœur**, puis choisissez chaque fichier de `cores/`. Les fichiers de `info/` sont facultatifs : ils donnent le nom complet du cœur.
3. Chargez le cœur voulu :

| Cœur | Console |
| --- | --- |
| `ravenemu_gb_libretro` | Game Boy (DMG) |
| `ravenemu_gbc_libretro` | Game Boy Color, compatible Game Boy |
| `ravenemu_gba_libretro` | Game Boy Advance |

Les cœurs gèrent la vibration (MBC5 Rumble, Drill Dozer, WarioWare: Twisted!) et les capteurs des cartouches, pilotés au stick analogique gauche :

| Capteur | Jeux | Commande |
| --- | --- | --- |
| Accéléromètre MBC7 | Kirby Tilt 'n' Tumble, Command Master | Stick gauche |
| Inclinaison GBA | Yoshi Topsy-Turvy, Koro Koro Puzzle | Stick gauche |
| Gyroscope GBA | WarioWare: Twisted! | Stick gauche, axe horizontal (à droite : sens horaire) |
| Capteur solaire GBA | Boktai 1 à 3 | Option `ravenemu_gba_solar_light` |

La sauvegarde et l'horloge d'un jeu GBA sont détectées automatiquement. La détection RTC couvre aussi les pilotes Pokémon/CFRU sans marqueur Seiko, à partir de leur identité interne et de références GPIO ([détails et limites](cores/gba/tests/rtc_compatibility.md)). Si la détection se trompe, deux options du cœur les imposent.

Les options s'affichent en français, rangées en deux catégories, **Image** et **Cartouche**. Elles s'appliquent pendant le jeu, sauf le type de sauvegarde et l'horloge, appliqués au chargement :

| Option | Clé | Cœurs | Valeurs |
| --- | --- | --- | --- |
| Palette | `ravenemu_gb_palette` | RavenGB | Vert (Game Boy) par défaut, ou gris (Game Boy Pocket) |
| Correction des couleurs LCD | `ravenemu_gbc_color_correction`, `ravenemu_gba_color_correction` | RavenGBC, RavenGBA | Désactivée par défaut. Activée, elle adoucit les couleurs vives comme l'écran de la console, avec le réglage de RavenEmu Android |
| Type de sauvegarde | `ravenemu_gba_save_type` | RavenGBA | `auto`, `sram`, `flash64`, `flash128`, `eeprom512`, `eeprom8k`, `none` |
| Horloge (RTC) | `ravenemu_gba_rtc` | RavenGBA | `auto`, `enabled`, `disabled` |
| Lumière du capteur solaire | `ravenemu_gba_solar_light` | RavenGBA | Pourcentage de plein soleil : `50` par défaut, de `0` à `100` par pas de 10 |

Un jeu refusé affiche sa raison à l'écran et dans le journal de RetroArch. Les images que RetroArch n'affiche pas (anticipation « run-ahead », avance rapide) ne sont pas dessinées par le cœur GBA ni converties par aucun cœur.

Les cœurs déclarent à RetroArch la mémoire que surveille RetroAchievements : les succès s'activent comme avec les autres cœurs, et RetroArch applique lui-même les règles du mode Hardcore. RetroAchievements n'a pas encore testé RavenCore.

> Les cœurs ne sont pas encore proposés par le téléchargeur intégré de RetroArch.

## Intégrer dans un projet

| Interface | En-tête | Usage |
| --- | --- | --- |
| C++ | [`core.hpp`](cores/common/include/ravenemu/core.hpp) | Accès direct aux moteurs |
| SDK C++ | [`sdk.hpp`](sdk/include/ravenemu/sdk.hpp) | Fabrique simplifiée |
| C | [`bridge.h`](sdk/include/ravenemu/bridge.h) | Frontends non C++ |

Après `cmake --install`, utilisez le paquet CMake :

```cmake
find_package(RavenCore 0.12 REQUIRED)
target_link_libraries(mon_app PRIVATE RavenCore::ravenemu_c_api)  # ou RavenCore::ravencore en C++
```

L'API C couvre :

- le chargement de ROM, l'image, l'audio et les boutons ;
- les sauvegardes batterie et les états instantanés ;
- la vibration, et le type de sauvegarde et l'horloge d'un jeu GBA ;
- les capteurs des cartouches : lumière, rotation et inclinaison ;
- le câble link entre consoles d'une même application (deux jeux sur un même appareil) ;
- le câble link vers une console d'un autre appareil, l'application transportant les messages (voir [docs/remote_link.md](docs/remote_link.md)) ;
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
  - le mode UART et l'adaptateur sans fil ;
  - l'exception d'instruction indéfinie.
- **Capteurs** : ceux des cartouches GBA suivent les mesures publiées par GBATEK, mais n'ont pas encore été essayés en jeu. Le sens des axes d'inclinaison (GBA et MBC7) et l'échelle du gyroscope restent à confirmer.
- **RetroAchievements** : les cœurs Libretro exposent une copie de la mémoire du jeu, rafraîchie à chaque image, vérifiée avec le code de rcheevos qu'utilise RetroArch mais pas encore en jeu dans RetroArch. Sur Game Boy, les deux fenêtres de ROM suivent la banque choisie par le jeu et les registres d'E/S sont exposés ; restent hors carte la RAM du MBC7 et, pour le MBC2, les bits hauts que relit le CPU. Le détail est dans [docs/retroachievements.md](docs/retroachievements.md). Les codes de triche ne sont pas pris en charge. L'intégration de [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu) est distincte.
- **Câble link distant** : deux consoles seulement, prévu pour un réseau local. La console qui cadence un transfert attend un aller-retour réseau : un jeu GBA qui enchaîne les transferts peut ralentir. Essayé entre un téléphone et un PC : un échange Pokémon Émeraude ↔ Saphir va au bout, mais lentement. Détails dans [docs/remote_link.md](docs/remote_link.md).
- **États instantanés** : un état ne se recharge pas dans une version plus ancienne que celle qui l'a créé. Les changements de format sont listés dans le [journal des modifications](CHANGELOG.md).

## Versions

Version actuelle : **0.12.0**. Chaque version est publiée sur la page des [releases](https://github.com/GhostPunishR/RavenCore/releases) avec les cœurs Libretro pour Linux, Windows, macOS et Android. Ses évolutions sont détaillées dans le [journal des modifications](CHANGELOG.md).

Avant la 1.0, une version peut changer l'API ou le format des états ; le journal le signale à chaque fois.

## Contribuer

Les contributions sont les bienvenues. Lisez le [guide de contribution](CONTRIBUTING.md), qui couvre l'organisation du code, les règles et les pull requests, ainsi que le [code de conduite](CODE_OF_CONDUCT.md). L'essentiel :

- une correction matérielle s'accompagne d'un test de régression ;
- aucun code repris d'un autre émulateur, aucune ROM ni aucun BIOS ;
- tout changement notable va dans la section « Non publié » du [journal](CHANGELOG.md).

## Licence

Projet développé par **GhostPunishR** sous licence [GNU AGPL-3.0](LICENSE). Le fichier `libretro.h` conserve sa licence MIT. Voir [NOTICE.md](NOTICE.md) pour les attributions.

Aucune ROM ni aucun BIOS commercial n'est fourni. RavenCore n'est affilié ni à Nintendo, ni à RetroArch, ni à RetroAchievements.
