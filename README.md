# RavenEmu Cores

**Cœurs d'émulation maison Game Boy, Game Boy Color et Game Boy Advance**, écrits en C++20 et extraits de [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu).

[![CI C++](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml/badge.svg)](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml)

Les cœurs sont indépendants d'Android et de JNI. Le dépôt les conserve tels quels dans `cores/`, avec leurs tests natifs. Le code original a été importé depuis le commit [RavenEmu 312e4af](https://github.com/GhostPunishR/RavenEmu/commit/312e4af765e2005e1f21713f4caf79e96c6f4da5). Un adaptateur Libretro distinct permet d'exécuter les jeux dans les frontends compatibles, sans remplacer le CPU, le PPU, l'APU ni la logique de cartouche des moteurs.

## Composants

| Cœur | Bibliothèque CMake | Niveau actuel |
|---|---|---|
| RavenGB | `gb_raven_core` | GB/DMG, moteur mature mais pas cycle-perfect |
| RavenGBC | `gbc_raven_core` | CGB natif, façade sur les composants GB partagés |
| RavenGBA | `gba_raven_core` | GBA, moteur encore expérimental |

L'API C++ commune est [`cores/common/include/ravenemu/core.hpp`](cores/common/include/ravenemu/core.hpp). Le petit en-tête [`sdk/include/ravenemu/sdk.hpp`](sdk/include/ravenemu/sdk.hpp) propose une fabrique pratique ; ce n'est **pas** une ABI C stabilisée.

## Compiler les cœurs et les tests (Linux/macOS/Windows)

Prérequis : CMake ≥ 3.22.1 et compilateur C++20.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Pour compiler exclusivement les moteurs sans Libretro :

```sh
cmake -S . -B build/cores -DRAVENEMU_BUILD_LIBRETRO=OFF -DBUILD_TESTING=OFF
cmake --build build/cores --parallel
```

Le binaire `ravenemu_minimal_player` est un exemple C++ en ligne de commande, **pas** un frontend graphique :

```sh
./build/ravenemu_minimal_player gb chemin/vers/jeu.gb
./build/ravenemu_minimal_player gbc chemin/vers/jeu.gbc
./build/ravenemu_minimal_player gba chemin/vers/jeu.gba
```

## Libretro / RetroArch

Les bibliothèques partagées générées sont :

- `ravenemu_gb_libretro` : GB, modèle automatique.
- `ravenemu_gbc_libretro` : GBC, matériel CGB imposé, avec mode de compatibilité GB.
- `ravenemu_gba_libretro` : GBA, expérimental.

Sous Linux leurs fichiers sont `.so`, sous Windows `.dll`, sous macOS `.dylib`. Le nom final dépend de la plateforme et de CMake. La construction et les tests de l'adaptateur sont automatisés dans [la CI](.github/workflows/ci.yml).

L'adaptateur transmet les commandes, les images, l'audio, les sauvegardes batterie et les save states des moteurs RavenEmu au frontend. Les jeux doivent être chargés depuis la mémoire du frontend. Aucune ROM de jeu n'est incluse.

### Tester dans RetroArch sur Linux x86-64

Ouvrir [la dernière exécution CI réussie](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml), télécharger l'artefact `ravenemu-libretro-linux-x86_64` et l'extraire. Copier les fichiers `cores/*.so` dans le dossier `cores` de RetroArch et les fichiers `info/*.info` dans son dossier `info`. Dans RetroArch, ouvrir **Charger un cœur** puis sélectionner RavenGB, RavenGBC ou RavenGBA, et charger une ROM obtenue légalement.

L'artefact est **compilé pour Linux x86-64 uniquement** ; il ne peut pas être installé tel quel dans RetroArch Android, Windows ou macOS. Les autres plateformes exigent une compilation adaptée et des tests spécifiques. Ce dépôt n'est pas encore inclus dans le téléchargeur officiel de cœurs RetroArch.

**RetroAchievements :** l'adaptateur Libretro ne déclare *pas encore* le support RetroAchievements ; les pointeurs RAM directs et les cartes mémoire exigés par les frontends doivent être implémentés et testés correctement avant toute activation. La prise en charge Hardcore et les classements compétitifs ne sont ni revendiqués ni approuvés. L'intégration RetroAchievements native à l'application Android relève du dépôt principal.

## Relation au projet Android

`GhostPunishR/RavenEmu` garde ses moteurs actuels : ce dépôt est une **distribution indépendante**, non une migration destructrice. Les futures corrections doivent être répercutées avec un commit source identifié afin de ne pas maintenir des divergences involontaires. La PR RetroAchievements #105 du dépôt Android reste indépendante.

## Licence et attribution

Code des moteurs et nouveaux adaptateurs sous **AGPL-3.0** ; voir [LICENSE](LICENSE) et [NOTICE.md](NOTICE.md). L'en-tête public `libretro.h` est sous licence MIT avec ses propres mentions de copyright. Les anciennes versions de RavenEmu distribuées sous MIT conservent leurs conditions d'origine.

Le dépôt n'est ni affilié à Nintendo ni approuvé par RetroArch ou RetroAchievements. Aucune ROM ni BIOS protégé n'est inclus.
