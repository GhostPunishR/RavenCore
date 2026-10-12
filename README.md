# RavenCore

[![CI](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml/badge.svg)](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/GhostPunishR/RavenCore?label=Version)](https://github.com/GhostPunishR/RavenCore/releases)
[![Licence AGPL-3.0](https://img.shields.io/badge/Licence-AGPL--3.0-663399?logo=gnu&logoColor=white)](LICENSE)

**RavenCore** rassemble les moteurs d'émulation maison de [RavenEmu](https://github.com/GhostPunishR/RavenEmu), développés en **C++20**. Ce dépôt permet de les utiliser indépendamment de l'application Android, notamment avec **Libretro et RetroArch**.

## Les cœurs

| Cœur | Console | État |
| --- | --- | --- |
| **RavenGB** | Game Boy | Avancé |
| **RavenGBC** | Game Boy Color | Avancé |
| **RavenGBA** | Game Boy Advance | Expérimental |

Les moteurs disposent de leur propre émulation CPU, graphique, audio et mémoire. GB et GBC partagent certains composants, sans utiliser de cœur tiers.

Le dossier `cores/common/` contient uniquement les outils et interfaces communs aux trois moteurs.

## Versions

La version actuelle est la **0.1.1**. Chaque version est publiée sur la page des [releases](https://github.com/GhostPunishR/RavenCore/releases), avec ses notes et les cœurs Libretro Linux x86-64. Le [journal des modifications](CHANGELOG.md) détaille les évolutions de chaque version, ainsi que les changements de format des états instantanés.

Avant la version 1.0, une version peut changer l'API ou le format des états : le journal le signale à chaque fois. Pour publier une version :

1. mettre à jour `project(VERSION)` dans `CMakeLists.txt`, la section correspondante du journal et la version indiquée ci-dessus ;
2. pousser le tag `vX.Y.Z`.

Le workflow `Release` vérifie alors la version, compile, teste et publie la release.

## Compilation

Prérequis : **CMake 3.22.1+** et un compilateur **C++20**.

```bash
git clone https://github.com/GhostPunishR/RavenCore.git
cd RavenCore
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Pour compiler les moteurs sans Libretro :

```bash
cmake -S . -B build/cores -DRAVENEMU_BUILD_LIBRETRO=OFF -DBUILD_TESTING=OFF
cmake --build build/cores --parallel
```

Les tests n'utilisent aucune ROM commerciale. Pour exécuter aussi les ROMs de test GBA publiques (jsmolka/gba-tests et FuzzARM), comme le fait la CI :

```bash
cores/gba/tests/fetch_test_roms.sh build/gba-test-roms
cmake -S . -B build -DRAVENEMU_GBA_TEST_ROMS=build/gba-test-roms
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Le script télécharge chaque ROM depuis un commit figé et vérifie son SHA-256. Voir les [tests GBA](cores/gba/tests/README.md).

L'interface C++ publique est définie dans [`core.hpp`](cores/common/include/ravenemu/core.hpp). Un en-tête [SDK](sdk/include/ravenemu/sdk.hpp) fournit une fabrique simplifiée, et [`bridge.h`](sdk/include/ravenemu/bridge.h) une API C pour les frontends non C++. Après `cmake --install`, les deux s'utilisent via `find_package(RavenCore)` (voir le [README du SDK](sdk/README.md)).

L'API C couvre le chargement de ROM, l'image, l'audio, les boutons, les sauvegardes batterie et les états instantanés. Elle permet aussi :
- d'imposer le type de sauvegarde et l'horloge d'un jeu GBA ;
- de savoir si le moteur de vibration de la cartouche tourne ;
- de lire, sans effet sur l'émulation, la mémoire surveillée par RetroAchievements.

## Libretro / RetroArch

Trois adaptateurs indépendants utilisent directement les moteurs RavenEmu :

| Fichier Linux | Console |
| --- | --- |
| `ravenemu_gb_libretro.so` | GB, matériel DMG |
| `ravenemu_gbc_libretro.so` | GBC, compatible GB |
| `ravenemu_gba_libretro.so` | GBA |

**Tester sur Linux x86-64 :** télécharger l'archive `ravenemu-libretro-vX.Y.Z-linux-x86_64.tar.gz` d'une [release](https://github.com/GhostPunishR/RavenCore/releases), ou l'artefact `ravenemu-libretro-linux-x86_64` d'une [exécution CI réussie](https://github.com/GhostPunishR/RavenCore/actions/workflows/ci.yml) pour la version en cours de développement. Copier les fichiers `cores/*.so` dans le dossier des cœurs de RetroArch et les fichiers `info/*.info` dans son dossier `info`. Charger ensuite le cœur souhaité.

Les adaptateurs nomment leurs boutons dans RetroArch, transmettent la vibration des cartouches à la manette (MBC5 Rumble, Drill Dozer, WarioWare: Twisted!) et pilotent l'accéléromètre des cartouches MBC7 (Kirby Tilt 'n' Tumble, Command Master) avec le stick analogique gauche. Le sens de cette inclinaison découle de la physique de l'accéléromètre et reste à confirmer en jeu.

Le cœur GBA propose deux options, prises en compte au chargement du jeu, pour les cas où la détection automatique se trompe :

| Option | Valeurs |
| --- | --- |
| `ravenemu_gba_save_type` (type de sauvegarde) | `auto`, `sram`, `flash64`, `flash128`, `eeprom512`, `eeprom8k`, `none` |
| `ravenemu_gba_rtc` (horloge de cartouche) | `auto`, `enabled`, `disabled` |

Les fichiers Linux, de la release comme de la CI, ne fonctionnent pas directement sur Android, Windows ou macOS. Les adaptateurs ne sont pas distribués par le téléchargeur officiel de RetroArch.

## RetroAchievements et limites

**RetroAchievements n'est pas encore activé dans les adaptateurs Libretro.** Ils ne déclarent pas encore leur carte mémoire à RetroArch, et le mode Hardcore n'est pas pris en charge. L'API C expose en revanche cette mémoire avec `ravenemu_v3_read_achievement_memory()` :
- GB/GBC : la carte mémoire de la console, la RAM de cartouche et les banques de WRAM CGB ;
- GBA : IWRAM, EWRAM puis sauvegarde.

Les lectures n'avancent pas l'émulation.

GB et GBC ne sont pas parfaitement fidèles à chaque détail matériel.

GBA reste expérimental. Il réussit les suites de tests publiques [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests) et [FuzzARM](https://github.com/DenSinH/FuzzARM) (voir les [tests GBA](cores/gba/tests/README.md)), mais n'a pas encore été essayé sur un large panel de jeux du commerce. Ne sont pas encore émulés : les capteurs solaire, gyroscopique et d'inclinaison de certaines cartouches, le câble link GBA et le mode UART du port série, le tampon de préchargement de la cartouche et l'exception d'instruction indéfinie. La sauvegarde est détectée par la signature de la bibliothèque du jeu, sans base de données par jeu ; les options ci-dessus corrigent une détection erronée.

Les sauvegardes d'état peuvent être incompatibles entre versions :

- depuis le format 11, GB et GBC stockent les images de l'écran sur 16 bits par pixel (un état passe d'environ 268 à 176 ko) et relisent toujours les états au format 10 ;
- le format GBA 12 ajoute l'état du pipeline du CPU, du bus et du port série ; les états GBA au format 11 se rechargent toujours.

Un état ne se recharge pas dans une version antérieure à celle qui l'a créé.

L'intégration RetroAchievements de [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu) est distincte de celle de Libretro.

## Contribuer

Les contributions sont les bienvenues. Consultez le [guide de contribution](CONTRIBUTING.md) et le [code de conduite](CODE_OF_CONDUCT.md) avant d'ouvrir une issue ou une pull request. Les évolutions de chaque version sont listées dans le [journal des modifications](CHANGELOG.md).

## Licence

Projet développé par **GhostPunishR**, distribué sous [GNU AGPL-3.0](LICENSE). Le fichier `libretro.h` conserve sa licence MIT. Voir [NOTICE.md](NOTICE.md) pour les attributions et la provenance des sources.

Aucune ROM ni aucun BIOS commercial n'est fourni. RavenCore n'est affilié ni à Nintendo, ni à RetroArch, ni à RetroAchievements.
