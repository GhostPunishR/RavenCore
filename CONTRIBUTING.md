# Contribuer à RavenCore

Merci de contribuer aux cœurs maison GB, GBC et GBA de RavenCore. Les petites corrections documentées et les tests de régression sont aussi utiles que les nouvelles fonctions.

Avant de contribuer, consultez le [code de conduite](CODE_OF_CONDUCT.md), la [licence AGPL-3.0](LICENSE) et les [attributions](NOTICE.md).

## Par où commencer ?

- **Un bug ou un jeu incompatible ?** Ouvrez une issue avec le modèle correspondant, après avoir recherché les signalements existants.
- **Une idée de fonctionnalité ?** Décrivez le besoin dans une issue avant une modification importante.
- **Une correction ?** Proposez une pull request courte, ciblée et accompagnée de tests quand cela est pertinent.

Ne publiez jamais de ROM commerciale, de BIOS, de sauvegarde personnelle, de clés ou de contenu protégé dans une issue, une PR ou les tests. Les ROMs de conformité doivent être fournies séparément et utilisées dans le respect de leurs licences.

## Préparer l'environnement

Prérequis : CMake 3.22.1 ou plus, Git et un compilateur C++20.

```bash
git clone https://github.com/GhostPunishR/RavenCore.git
cd RavenCore
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Pour ne compiler que les moteurs, désactivez Libretro avec `-DRAVENEMU_BUILD_LIBRETRO=OFF`. La CI compile et teste les moteurs et les deux adaptateurs Libretro (RavenGBC, RavenGBA) sur Linux (GCC, et sous ASan/UBSan), Windows (MSVC) et macOS (Apple Clang), et vérifie la compilation Android (NDK, arm64-v8a et armeabi-v7a).

Les tests GB/GBC disposent aussi d'un [runner de conformité](cores/gbc/tests/README.md). Ses ROMs de test sont externes au dépôt. N'annoncez pas une suite comme réussie sans l'avoir exécutée et sans indiquer les résultats.

## Organisation du code

| Dossier | Contenu |
| --- | --- |
| `cores/common/` | Contrats et outils C++ partagés ; `cores/common/arm/` : CPU ARM commun (ARMv4T, ARMv5TE), en en-têtes |
| `cores/gbc/` | Moteur Game Boy Color, qui émule aussi la Game Boy : un seul cœur pour les deux consoles |
| `cores/gba/` | Moteur Game Boy Advance |
| `cores/nds/` | Moteur Nintendo DS, en construction : pas encore d'API ni de cœur Libretro ([plan](docs/nds_melonds_map.md)) |
| `libretro/` | Adaptateurs, métadonnées et tests Libretro |
| `sdk/` | API C (`bridge.h`), fabrique C++ et paquet CMake |

Conservez la séparation entre la logique matérielle et les frontends. Les dossiers `cores/` ne doivent pas dépendre d'Android, de JNI ou de RetroArch.

### Cœur GB/GBC : un fichier par composant

Le cœur GB/GBC est fait d'en-têtes, un dossier par composant matériel de `cores/gbc/src/` (`cpu/`, `ppu/`, `apu/`, `memory/`, `timer/`, `cartridge/`…). Un composant qui en regroupe plusieurs a un fichier pour chacun :

| Composant | Fichiers |
| --- | --- |
| PPU | `ppu.hpp` (état et accès simples), `line_timing.hpp` (lignes, modes, STAT, LYC), `video_bus.hpp` (accès du CPU à la VRAM, à l'OAM et à la RAM couleur), `oam_port.hpp` (scan du mode 2, DMA OAM, `oam_corruption.hpp`), `pixel_transfer.hpp` (mode 3), `background_fetcher.hpp`, `object_fetcher.hpp`, `pixel_fifo.hpp`, `pixel_renderer.hpp`, `color_ram.hpp`, `cgb_color.hpp`, `compatibility_palettes.hpp`, `ppu_state.hpp` et `frame_codec.hpp` (états instantanés) |
| APU | `apu.hpp` (registres, séquenceur, mixeur), `square_channel.hpp`, `wave_channel.hpp`, `noise_channel.hpp`, `sampling.hpp` |
| Bus mémoire | `memory_bus.hpp` (carte mémoire et registres d'E/S), `oam_dma.hpp`, `hdma.hpp`, `bus.hpp` (interface vue par le CPU) |

La classe `Ppu` garde l'état et déclare toutes ses fonctions ; chaque fichier de composant définit les siennes (`inline`), et `ppu.hpp` les inclut à sa fin. Placez une nouvelle fonction dans le fichier de son composant ; un fichier qui dépasse 800 lignes se découpe d'abord (voir [AGENTS.md](AGENTS.md)). L'ordre des champs des états instantanés est fixé par les fonctions `save` et `load` : un refactor ne doit pas le changer.

## Règles de contribution

1. Écrivez du C++20 lisible, cohérent avec le module modifié et sans nouvelle dépendance injustifiée.
2. Pour une correction de CPU, PPU, APU, timer, DMA, cartouche ou sauvegarde, ajoutez un test de régression ou expliquez pourquoi ce n'est pas possible.
3. Documentez les changements de compatibilité, de timings, de sauvegardes batterie et de format des états instantanés, notamment dans la section « Non publié » du [journal des modifications](CHANGELOG.md). Évitez de casser les anciennes sauvegardes sans avertissement.
4. N'intégrez pas de code copié, traduit ou adapté d'un autre émulateur. Utilisez des spécifications et documentations matérielles publiques, avec leurs références lorsque cela aide à vérifier un comportement.
5. Pour Libretro, préservez l'interface C et vérifiez le chargement, la vidéo, l'audio, les entrées et les sauvegardes. N'annoncez pas RetroAchievements ou Hardcore sans une implémentation et une validation effectives.
6. Ne joignez aucun fichier de jeu, BIOS, firmware propriétaire ou donnée personnelle. Un nom de jeu et une empreinte SHA-256 facultative peuvent aider à reproduire un bug.

## Proposer une pull request

Créez une branche dédiée à partir de `main`, limitez les changements au problème traité et expliquez ce que vous avez testé. Utilisez le [modèle de PR](.github/PULL_REQUEST_TEMPLATE.md).

Une PR doit idéalement mentionner : le moteur concerné, le comportement avant et après, les tests exécutés et les limites restantes. Signalez si le changement mérite aussi d'être synchronisé avec le dépôt [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu).

Les changements sont examinés avant intégration. Les tests automatisés doivent réussir, mais ne remplacent pas la validation des jeux et du matériel concernés.

## Publier une version (mainteneurs)

1. Mettez à jour `project(VERSION)` dans `CMakeLists.txt`, la version indiquée dans le README, et renommez la section « Non publié » du [journal](CHANGELOG.md) en `## X.Y.Z - AAAA-MM-JJ`.
2. Poussez le tag `vX.Y.Z`.

Le workflow `Release` vérifie que la version et le journal concordent, compile, teste, puis publie la release avec ses notes et les cœurs Libretro Linux.
