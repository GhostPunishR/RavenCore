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

L'interface C++ publique est définie dans [`core.hpp`](cores/common/include/ravenemu/core.hpp). Un en-tête [SDK](sdk/include/ravenemu/sdk.hpp) fournit une fabrique simplifiée.

## Libretro / RetroArch

Trois adaptateurs indépendants utilisent directement les moteurs RavenEmu :

| Fichier Linux | Console |
| --- | --- |
| `ravenemu_gb_libretro.so` | GB, matériel DMG |
| `ravenemu_gbc_libretro.so` | GBC, compatible GB |
| `ravenemu_gba_libretro.so` | GBA |

**Tester sur Linux x86-64 :** télécharger l'artefact `ravenemu-libretro-linux-x86_64` depuis une [exécution CI réussie](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml). Copier les fichiers `cores/*.so` dans le dossier des cœurs de RetroArch et les fichiers `info/*.info` dans son dossier `info`. Charger ensuite le cœur souhaité.

Les fichiers CI Linux ne fonctionnent pas directement sur Android, Windows ou macOS. Les adaptateurs ne sont pas distribués par le téléchargeur officiel de RetroArch.

## RetroAchievements et limites

**RetroAchievements n'est pas encore activé dans les adaptateurs Libretro.** Leur exposition mémoire doit être complétée et vérifiée. Le mode Hardcore n'est pas pris en charge.

GB et GBC ne sont pas parfaitement fidèles à chaque détail matériel. GBA reste expérimental, avec des limites de compatibilité et de performances. Les sauvegardes d'état peuvent être incompatibles entre versions.

L'intégration RetroAchievements de [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu) est distincte de celle de Libretro.

## Contribuer

Les contributions sont les bienvenues. Consultez le [guide de contribution](CONTRIBUTING.md) et le [code de conduite](CODE_OF_CONDUCT.md) avant d'ouvrir une issue ou une pull request.

## Licence

Projet développé par **GhostPunishR**, distribué sous [GNU AGPL-3.0](LICENSE). Le fichier `libretro.h` conserve sa licence MIT. Voir [NOTICE.md](NOTICE.md) pour les attributions et la provenance des sources.

Aucune ROM ni aucun BIOS commercial n'est fourni. RavenEmu Cores n'est affilié ni à Nintendo, ni à RetroArch, ni à RetroAchievements.
