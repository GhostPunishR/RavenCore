# RavenEmu Cores

[![CI](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml/badge.svg)](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml)
[![Licence AGPL-3.0](https://img.shields.io/badge/Licence-AGPL--3.0-663399?logo=gnu&logoColor=white)](LICENSE)

**RavenEmu Cores** rassemble les moteurs d'émulation maison de [RavenEmu](https://github.com/GhostPunishR/RavenEmu), développés en **C++20**. Ce dépôt permet de les utiliser indépendamment de l'application Android, notamment avec **Libretro et RetroArch**.

## Les cœurs

| Cœur | Console | État |
| --- | --- | --- |
| **RavenGB** | Game Boy | Avancé |
| **RavenGBC** | Game Boy Color | Avancé |
| **RavenGBA** | Game Boy Advance | Expérimental |

Les moteurs disposent de leur propre émulation CPU, graphique, audio et mémoire. GB et GBC partagent certains composants, sans utiliser de cœur tiers.

Le dossier `cores/common/` contient uniquement les outils et interfaces communs aux trois moteurs.

## Compilation

Prérequis : **CMake 3.22.1+** et un compilateur **C++20**.

```bash
git clone https://github.com/GhostPunishR/RavenEmu-Cores.git
cd RavenEmu-Cores
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Pour compiler les moteurs sans Libretro :

```bash
cmake -S . -B build/cores -DRAVENEMU_BUILD_LIBRETRO=OFF -DBUILD_TESTING=OFF
cmake --build build/cores --parallel
```

L'interface C++ publique est définie dans [`core.hpp`](cores/common/include/ravenemu/core.hpp). Un en-tête [SDK](sdk/include/ravenemu/sdk.hpp) fournit une fabrique simplifiée, et [`bridge.h`](sdk/include/ravenemu/bridge.h) une API C pour les frontends non C++. Après `cmake --install`, les deux s'utilisent via `find_package(RavenEmuCores)` (voir le [README du SDK](sdk/README.md)).

## Libretro / RetroArch

Trois adaptateurs indépendants utilisent directement les moteurs RavenEmu :

| Fichier Linux | Console |
| --- | --- |
| `ravenemu_gb_libretro.so` | GB, matériel DMG |
| `ravenemu_gbc_libretro.so` | GBC, compatible GB |
| `ravenemu_gba_libretro.so` | GBA |

**Tester sur Linux x86-64 :** télécharger l'artefact `ravenemu-libretro-linux-x86_64` depuis une [exécution CI réussie](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml). Copier les fichiers `cores/*.so` dans le dossier des cœurs de RetroArch et les fichiers `info/*.info` dans son dossier `info`. Charger ensuite le cœur souhaité.

Les adaptateurs nomment leurs boutons dans RetroArch, transmettent la vibration des cartouches à la manette (MBC5 Rumble, Drill Dozer, WarioWare: Twisted!) et pilotent l'accéléromètre des cartouches MBC7 (Kirby Tilt 'n' Tumble, Command Master) avec le stick analogique gauche. Le sens de cette inclinaison découle de la physique de l'accéléromètre et reste à confirmer en jeu.

Le cœur GBA propose deux options, prises en compte au chargement du jeu, pour les cas où la détection automatique se trompe :

| Option | Valeurs |
| --- | --- |
| `ravenemu_gba_save_type` (type de sauvegarde) | `auto`, `sram`, `flash64`, `flash128`, `eeprom512`, `eeprom8k`, `none` |
| `ravenemu_gba_rtc` (horloge de cartouche) | `auto`, `enabled`, `disabled` |

Les fichiers CI Linux ne fonctionnent pas directement sur Android, Windows ou macOS. Les adaptateurs ne sont pas distribués par le téléchargeur officiel de RetroArch.

## RetroAchievements et limites

**RetroAchievements n'est pas encore activé dans les adaptateurs Libretro.** Leur exposition mémoire doit être complétée et vérifiée. Le mode Hardcore n'est pas pris en charge.

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

Aucune ROM ni aucun BIOS commercial n'est fourni. RavenEmu Cores n'est affilié ni à Nintendo, ni à RetroArch, ni à RetroAchievements.
