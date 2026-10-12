# RavenEmu Cores

**Moteurs d'émulation Game Boy, Game Boy Color et Game Boy Advance, développés pour RavenEmu en C++20.**

[![CI — Cœurs et Libretro](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml/badge.svg)](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml)
[![Licence : AGPL-3.0](https://img.shields.io/badge/Licence-AGPL--3.0-blue.svg)](LICENSE)

**RavenEmu-Cores** regroupe les cœurs d'émulation maison du projet [RavenEmu](https://github.com/GhostPunishR/RavenEmu) dans un dépôt indépendant de l'application Android.

Les moteurs ne sont pas des adaptations de cœurs tiers : ils conservent leur propre implémentation du CPU, des composants graphiques et audio, de la mémoire et des cartouches. Ils peuvent être intégrés à d'autres applications C++ ou utilisés à travers les adaptateurs **Libretro** fournis ici.

> **État du projet :** GB/GBC sont en développement avancé ; GBA reste expérimental. Une compilation réussie ou un test synthétique validé ne garantit pas la compatibilité avec tous les jeux.

## Les trois cœurs

| Cœur | Console | Bibliothèque C++ | Adaptateur Libretro | État |
| --- | --- | --- | --- | --- |
| **RavenGB** | Game Boy | `gb_raven_core` | `ravenemu_gb_libretro` | Avancé, non cycle-perfect |
| **RavenGBC** | Game Boy Color | `gbc_raven_core` | `ravenemu_gbc_libretro` | Avancé, différences matérielles à valider |
| **RavenGBA** | Game Boy Advance | `gba_raven_core` | `ravenemu_gba_libretro` | Expérimental |

RavenGB et RavenGBC partagent les composants matériels communs. **RavenGBC** est une bibliothèque distincte qui utilise ces composants et sélectionne un matériel CGB, y compris le mode de compatibilité pour les jeux Game Boy. Le moteur GBA possède sa propre implémentation.

L'adaptateur **RavenGB Libretro** sélectionne volontairement le modèle DMG (Game Boy monochrome). Pour jouer à une ROM Game Boy Color, choisissez RavenGBC.

## Fonctionnalités

Les cœurs exposent notamment :

- l'exécution des jeux, la gestion des boutons, l'image et le son ;
- les sauvegardes batterie et les états instantanés ;
- les fonctions matérielles propres à chaque console, selon leur degré d'implémentation ;
- une API C++ commune, sans dépendance à Android ou à JNI.

Les adaptateurs Libretro relient ces moteurs aux fonctions standard du frontend : affichage, audio, manette, chargement de jeu, sauvegarde batterie et états instantanés.

**Limitations connues :** des comportements matériels rares restent à valider sur GB/GBC ; le cœur GBA présente encore des limites de précision, de performances et de compatibilité selon les jeux. Le support des périphériques particuliers n'est pas exhaustif. Les moteurs ne sont pas présentés comme entièrement conformes au matériel d'origine.

## Compilation

### Prérequis

- **CMake 3.22.1** ou version ultérieure ;
- un compilateur compatible **C++20** ;
- un environnement de construction natif adapté au système cible.

### Compiler les moteurs, les adaptateurs Libretro et les tests

```bash
git clone https://github.com/GhostPunishR/RavenEmu-Cores.git
cd RavenEmu-Cores

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Compiler uniquement les moteurs C++

```bash
cmake -S . -B build/cores -DRAVENEMU_BUILD_LIBRETRO=OFF -DBUILD_TESTING=OFF
cmake --build build/cores --parallel
```

Les bibliothèques C++ sont construites à partir de `cores/`. Les adaptateurs Libretro, lorsqu'ils sont activés, sont générés dans `build/libretro/`.

| Option CMake | Par défaut | Utilité |
| --- | --- | --- |
| `RAVENEMU_BUILD_LIBRETRO` | `ON` | Construire les trois adaptateurs Libretro |
| `BUILD_TESTING` | `ON` | Compiler et exécuter les tests avec CTest |

La CI exécute les tests des moteurs et des adaptateurs sur Linux. Les tests utilisent des données synthétiques et ne nécessitent aucune ROM commerciale.

## Utiliser les cœurs dans RetroArch

Les adaptateurs fournissent des bibliothèques compatibles avec l'interface [Libretro](https://www.libretro.com/). Ils peuvent être chargés manuellement dans un frontend compatible, notamment RetroArch.

### Installation manuelle — Linux x86-64

1. Ouvrez les [exécutions GitHub Actions](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml) et choisissez une exécution **réussie**.
2. Téléchargez l'artefact `ravenemu-libretro-linux-x86_64`.
3. Extrayez les fichiers `cores/*_libretro.so` vers le répertoire des cœurs de RetroArch, puis les fichiers `info/*_libretro.info` vers son répertoire `info`.
4. Dans RetroArch, sélectionnez **Charger un cœur**, puis RavenGB, RavenGBC ou RavenGBA. Chargez ensuite votre jeu.

| Fichier du cœur (Linux) | Usage |
| --- | --- |
| `ravenemu_gb_libretro.so` | Game Boy, mode DMG |
| `ravenemu_gbc_libretro.so` | Game Boy Color / compatibilité GB |
| `ravenemu_gba_libretro.so` | Game Boy Advance, expérimental |

Les bibliothèques sont produites au format natif du système (`.so`, `.dll` ou `.dylib` selon la plateforme). **L'artefact CI fourni ici cible Linux x86-64** : il n'est pas directement installable sur Android, Windows ou macOS. Ces plateformes exigent une compilation et une validation adaptées.

Ces cœurs ne figurent pas automatiquement dans le téléchargeur officiel RetroArch. Leur présence dans un frontend compatible ne constitue pas une approbation officielle de RetroArch.

## RetroAchievements

L'application [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu) dispose de sa propre intégration RetroAchievements.

**Les adaptateurs Libretro de ce dépôt ne déclarent pas encore la prise en charge de RetroAchievements.** Ils n'exposent pas actuellement toutes les zones de mémoire nécessaires à une intégration vérifiée ; ils annoncent donc explicitement cette fonctionnalité comme indisponible.

Il n'y a **aucune revendication de prise en charge Hardcore ou d'homologation RetroAchievements** pour ces adaptateurs. La compatibilité mémoire doit être implémentée et testée avant d'envisager son activation.

## Architecture

```text
RavenEmu-Cores/
├── cores/
│   ├── common/       # API et utilitaires partagés
│   ├── gb/           # Moteur Game Boy
│   ├── gbc/          # Composants et façade Game Boy Color
│   └── gba/          # Moteur Game Boy Advance
├── sdk/
│   └── include/      # Interface C++ de commodité
├── libretro/
│   ├── include/      # En-tête officiel Libretro
│   ├── src/          # Adaptateurs des trois moteurs
│   ├── info/         # Métadonnées pour les frontends
│   └── tests/        # Tests des adaptateurs
├── .github/workflows/
│   └── ci.yml
├── CMakeLists.txt
├── LICENSE
└── NOTICE.md
```

- **`cores/common/`** contient les composants partagés nécessaires aux moteurs. Ce n'est pas un quatrième émulateur.
- **`cores/`** reste indépendant de l'interface utilisateur et d'Android.
- **`sdk/include/ravenemu/sdk.hpp`** propose une fabrique C++ pratique ; ce n'est pas une ABI stable destinée aux plugins binaires.
- **`libretro/`** adapte les moteurs existants sans remplacer leur implémentation.

L'interface native centrale est [`cores/common/include/ravenemu/core.hpp`](cores/common/include/ravenemu/core.hpp).

## Sauvegardes et compatibilité

Les sauvegardes batterie et les états instantanés sont deux mécanismes distincts. La compatibilité des **états instantanés** entre différentes versions du cœur n'est pas garantie : certains anciens formats sont volontairement refusés pour éviter une restauration incorrecte.

Pour la GBA, l'identification automatique d'une EEPROM ne permet pas toujours de déterminer sa capacité à partir de la seule signature du jeu. Les moteurs disposent de mécanismes de détection et de configuration adaptés, mais les cas particuliers doivent être vérifiés.

En cas de changement de version, **conservez une copie de vos sauvegardes batterie** avant de remplacer un cœur.

## Provenance et développement

Les fichiers initiaux de `cores/` proviennent de [RavenEmu, commit `312e4af`](https://github.com/GhostPunishR/RavenEmu/commit/312e4af765e2005e1f21713f4caf79e96c6f4da5). Ils ont ensuite pu recevoir des corrections propres à ce dépôt.

RavenEmu Android et RavenEmu-Cores sont deux dépôts indépendants : modifier l'un ne met **pas automatiquement** l'autre à jour. Les correctifs doivent être synchronisés et testés explicitement.

La construction et les tests automatisés sont consultables dans [GitHub Actions](https://github.com/GhostPunishR/RavenEmu-Cores/actions/workflows/ci.yml).

## Licence

**RavenEmu-Cores est distribué sous [GNU AGPL-3.0](LICENSE).** Les moteurs sont issus du projet RavenEmu de **GhostPunishR**. La réutilisation et la redistribution, y compris commerciale, sont possibles sous réserve du respect des obligations de cette licence.

L'en-tête `libretro/include/libretro.h` possède sa propre licence MIT et ses mentions de copyright, conservées dans le fichier. Consultez [NOTICE.md](NOTICE.md) pour la provenance et les détails.

Aucune ROM commerciale ni aucun BIOS protégé n'est inclus. Game Boy, Game Boy Color et Game Boy Advance sont des marques de Nintendo. Ce projet n'est affilié ni à Nintendo, ni à RetroArch, ni à RetroAchievements.
