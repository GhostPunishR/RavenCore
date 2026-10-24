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

Les cœurs gèrent la vibration (MBC5 Rumble, Drill Dozer, WarioWare: Twisted!) et les capteurs des cartouches. Ils les pilotent au stick analogique gauche, et avec les capteurs du téléphone ou de la manette s'il en a :

| Capteur | Jeux | Commande |
| --- | --- | --- |
| Accéléromètre MBC7 | Kirby Tilt 'n' Tumble, Command Master | Stick gauche, accéléromètre de l'appareil |
| Inclinaison GBA | Yoshi Topsy-Turvy, Koro Koro Puzzle | Stick gauche, accéléromètre de l'appareil |
| Gyroscope GBA | WarioWare: Twisted! | Stick gauche, axe horizontal (à droite : sens horaire), gyroscope de l'appareil |
| Capteur solaire GBA | Boktai 1 à 3 | Option `ravenemu_gba_solar_light` : un pourcentage, ou le capteur de lumière de l'appareil |

Les capteurs de l'appareil passent par l'interface de RetroArch (« Capteurs » doit y être activé), avec les conversions de RavenEmu Android :
- **Inclinaison** : posé à plat, l'appareil n'est pas incliné ; tenu debout, il l'est de 1 g. Le jeu se cale souvent lui-même sur la position de départ.
- **Rotation** : un demi-tour par seconde donne la pleine échelle.
- **Lumière** : rien sous 1000 lux (une pièce éclairée), le plein soleil vers 100 000 lux, sur une échelle logarithmique.

Ils ne s'allument que pour un jeu qui en a besoin. L'option **Capteurs de l'appareil** les ignore pour garder le stick seul.

La sauvegarde et l'horloge d'un jeu GBA sont détectées automatiquement. La détection RTC couvre aussi les pilotes Pokémon/CFRU sans marqueur Seiko, à partir de leur identité interne et de références GPIO ([détails et limites](cores/gba/tests/rtc_compatibility.md)). Si la détection se trompe, deux options du cœur les imposent.

Les options s'affichent en français, rangées en quatre catégories : **Image**, **Commandes**, **Système** et **Cartouche**. Elles s'appliquent pendant le jeu, sauf la console émulée, la boot ROM, le BIOS, le type de sauvegarde et l'horloge, appliqués au chargement du jeu :

| Option | Clé | Cœurs | Valeurs |
| --- | --- | --- | --- |
| Palette | `ravenemu_gb_palette` | RavenGB | Vert (Game Boy) par défaut, gris (Game Boy Pocket) ou gris neutre |
| Couleurs des jeux Game Boy | `ravenemu_gbc_dmg_colors` | RavenGBC | Sans boot ROM. « Comme sur la GBC » par défaut : celles que sa boot ROM choisit d'après le titre du jeu (Pokémon Rouge en rouge…), ou ses couleurs par défaut pour un jeu qu'elle ne connaît pas. Ou l'une des douze combinaisons de touches tenues pendant son logo (B + Gauche : gris, B + Droite : négatif…) |
| Correction des couleurs LCD | `ravenemu_gbc_color_correction`, `ravenemu_gba_color_correction` | RavenGBC, RavenGBA | Désactivée par défaut. Activée, elle adoucit les couleurs vives comme l'écran de la console, avec le réglage de RavenEmu Android |
| Mélange d'images | `ravenemu_gb_frame_blending`, `ravenemu_gbc_frame_blending`, `ravenemu_gba_frame_blending` | Tous | Désactivé par défaut. **Mélange** : la moyenne des deux dernières images, pour les jeux qui font clignoter un objet une image sur deux. **Rémanence** : l'image s'efface lentement, comme sur l'écran LCD |
| Directions opposées | `ravenemu_gb_opposing_directions`, `ravenemu_gbc_opposing_directions`, `ravenemu_gba_opposing_directions` | Tous | Bloquées par défaut : haut et bas, ou gauche et droite, tenus ensemble ne sont pas transmis, comme sur la croix de la console. Autorisées, sinon |
| Boot ROM | `ravenemu_gb_boot_rom`, `ravenemu_gbc_boot_rom` | RavenGB, RavenGBC | Utilisée si son fichier est présent (par défaut), ou non |
| Console émulée | `ravenemu_gbc_hardware` | RavenGBC | Game Boy Color par défaut, ou Game Boy Advance : les jeux qui la reconnaissent débloquent leurs bonus (Zelda Oracle, Shantae…) |
| Type de sauvegarde | `ravenemu_gba_save_type` | RavenGBA | `auto`, `sram`, `flash64`, `flash128`, `eeprom512`, `eeprom8k`, `none` |
| Horloge (RTC) | `ravenemu_gba_rtc` | RavenGBA | `auto`, `enabled`, `disabled` |
| Lumière du capteur solaire | `ravenemu_gba_solar_light` | RavenGBA | Pourcentage de plein soleil : `50` par défaut, de `0` à `100` par pas de 10. Ou `sensor` : le capteur de lumière de l'appareil, 50 % s'il n'en a pas |
| Capteurs de l'appareil | `ravenemu_gb_device_sensors`, `ravenemu_gbc_device_sensors`, `ravenemu_gba_device_sensors` | Tous | Utilisés s'il y en a (par défaut) : accéléromètre et gyroscope s'ajoutent au stick gauche. Ou stick gauche seul |
| BIOS | `ravenemu_gba_bios` | RavenGBA | BIOS de RavenCore par défaut, ou vrai BIOS (`gba_bios.bin`), avec ou sans son animation de démarrage |

RavenGB et RavenGBC démarrent un jeu sans boot ROM, dans l'état où elle l'aurait laissé. Ils utilisent celle du dossier système de RetroArch si l'option le permet : `dmg_boot.bin` ou `gb_bios.bin` (Game Boy), `cgb_boot.bin` ou `gbc_bios.bin` (Game Boy Color), `agb_boot.bin` (Game Boy Advance). On retrouve alors l'animation de démarrage. Sans elle, un jeu Game Boy sur RavenGBC prend quand même les couleurs que la boot ROM de la GBC lui aurait choisies, d'après la table relevée par [The Cutting Room Floor](https://tcrf.net/Notes:Game_Boy_Color_Bootstrap_ROM) (CC BY 3.0, voir [NOTICE.md](NOTICE.md)). Aucune boot ROM n'est fournie.

RavenGBA n'a besoin d'aucun BIOS : RavenCore répond lui-même à ses appels. L'option **BIOS** lui fait utiliser `gba_bios.bin` (16 Kio), le BIOS d'une console, depuis le dossier système. Les appels et les interruptions du jeu passent alors par lui, avec ou sans son animation de démarrage. Un état ne se recharge qu'avec le BIOS qui l'a créé. Aucun BIOS n'est fourni.

Un jeu refusé affiche sa raison à l'écran et dans le journal de RetroArch. Les images que RetroArch n'affiche pas (anticipation « run-ahead », avance rapide) ne sont pas dessinées par le cœur GBA ni converties par aucun cœur.

Le menu **Codes de triche** de RetroArch fonctionne avec les trois cœurs. Une entrée peut compter plusieurs lignes, séparées par des « + », des espaces ou des retours à la ligne :

| Cœurs | Formats |
| --- | --- |
| RavenGB, RavenGBC | Game Genie (`ABC-DEF-GHI` ou `ABC-DEF`) et GameShark (8 chiffres, comme `010238CD`), mélangeables |
| RavenGBA | Codebreaker non chiffré (`XAAAAAAA YYYY`), GameShark v1/v2 et Action Replay v3 chiffrés tels que l'appareil les affiche (`XXXXXXXX YYYYYYYY`), reconnus automatiquement |

Un code refusé affiche sa raison. Les patchs de ROM GBA, le changement de chiffrement `DEADFACE` et les codes Codebreaker chiffrés (type 9) ne sont pas pris en charge. Les codes d'activation (« Must Be On ») sont acceptés, mais n'ont pas d'effet : ils ne servent qu'à l'appareil.

Les cœurs déclarent à RetroArch la mémoire que surveille RetroAchievements : les succès s'activent comme avec les autres cœurs, et RetroArch applique lui-même les règles du mode Hardcore. RetroAchievements n'a pas encore testé RavenCore.

**Deux joueurs reliés par câble link.** Chaque cœur déclare le sous-système « Deux joueurs (câble link) ». Dans RetroArch, charger le cœur, puis choisir « Deux joueurs (câble link) » dans **Charger du contenu** : le jeu du joueur 1, puis celui du joueur 2, deux jeux de la même console. Ils démarrent reliés, comme deux consoles et un câble :
- les deux écrans s'affichent côte à côte, le joueur 1 à gauche ;
- le joueur 1 joue sur la manette du port 1, le joueur 2 sur celle du port 2 ;
- chacun garde sa sauvegarde, au nom de son jeu (`Pokémon Rubis.srm`…) : celle d'une partie en solo sert telle quelle ;
- on entend le joueur 1 ;
- les états instantanés, le reset et le mélange d'images couvrent les deux consoles.

RetroAchievements, les codes de triche et les capteurs de l'appareil ne concernent que le joueur 1. Sur GBA, le joueur 1 est la console parente des jeux multijoueurs.

> Les cœurs ne sont pas encore proposés par le téléchargeur intégré de RetroArch.

## Intégrer dans un projet

| Interface | En-tête | Usage |
| --- | --- | --- |
| C++ | [`core.hpp`](cores/common/include/ravenemu/core.hpp) | Accès direct aux moteurs |
| SDK C++ | [`sdk.hpp`](sdk/include/ravenemu/sdk.hpp) | Fabrique simplifiée |
| C | [`bridge.h`](sdk/include/ravenemu/bridge.h) | Frontends non C++ |

Après `cmake --install`, utilisez le paquet CMake :

```cmake
find_package(RavenCore 0.14 REQUIRED)
target_link_libraries(mon_app PRIVATE RavenCore::ravenemu_c_api)  # ou RavenCore::ravencore en C++
```

L'API C couvre :

- le chargement de ROM, l'image, l'audio et les boutons ;
- les sauvegardes batterie et les états instantanés ;
- la vibration, et le type de sauvegarde et l'horloge d'un jeu GBA ;
- les capteurs des cartouches : lumière, rotation et inclinaison ;
- le câble link entre consoles d'une même application (deux jeux sur un même appareil) ;
- le câble link vers une console d'un autre appareil, l'application transportant les messages (voir [docs/remote_link.md](docs/remote_link.md)) ;
- la lecture de la mémoire surveillée par RetroAchievements ;
- les codes de triche, vérifiés un par un pour le joueur puis activés en bloc ;
- les couleurs des jeux Game Boy sur une Game Boy Color ;
- la Game Boy Color d'une Game Boy Advance, pour les bonus des jeux qui la reconnaissent.

Détails dans le [README du SDK](sdk/README.md). En C++, `parse_cheat` décode un code de triche, `Core::set_cheats` active la liste, et `gb::HardwareModel::agb` crée la GBC d'une Game Boy Advance.

## Tests

Les tests de base n'utilisent aucune ROM. La CI exécute en plus des suites de test publiques, téléchargées à la volée et vérifiées par SHA-256. Aucune ROM n'est stockée dans le dépôt.

| Console | Suites | Documentation |
| --- | --- | --- |
| GB / GBC | Mooneye Test Suite, Blargg, dmg-acid2, cgb-acid2 (200 tests) | [Tests GB/GBC](cores/gb/tests/README.md) |
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

- **GB / GBC** : les 200 tests publics retenus passent. Des suites plus exigeantes (Mealybug Tearoom, SameSuite, gbmicrotest) échouent encore en partie. Cartouches non prises en charge : TAMA5, Game Boy Camera, M161 et cartouches non licenciées. Le Super Game Boy n'est pas émulé. En mode Game Boy Advance, seuls les registres de démarrage distinguent la console d'une GBC.
- **GBA** : expérimental. Les suites gba-tests et FuzzARM passent, mais le cœur n'a pas encore été essayé sur un large panel de jeux. Non émulés pour l'instant :
  - le mode UART et l'adaptateur sans fil ;
  - l'exception d'instruction indéfinie.
- **Capteurs** : ceux des cartouches GBA suivent les mesures publiées par GBATEK, mais n'ont pas encore été essayés en jeu. Le sens des axes d'inclinaison (GBA et MBC7) et l'échelle du gyroscope restent à confirmer.
- **RetroAchievements** : les cœurs Libretro exposent une copie de la mémoire du jeu, rafraîchie à chaque image, vérifiée avec le code de rcheevos qu'utilise RetroArch mais pas encore en jeu dans RetroArch. Sur Game Boy, les deux fenêtres de ROM suivent la banque choisie par le jeu et les registres d'E/S sont exposés ; restent hors carte la RAM du MBC7 et, pour le MBC2, les bits hauts que relit le CPU. Le détail est dans [docs/retroachievements.md](docs/retroachievements.md). L'intégration de [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu) est distincte.
- **Codes de triche** : décodés d'après Pan Docs et GBATEK, vérifiés avec des codes fabriqués pour les tests, pas encore avec des codes publiés essayés en jeu. Un code GBA de 16 chiffres est reconnu à son déchiffrement : sur des codes Action Replay fabriqués, environ un sur 40 000 est lu à tort comme un code GameShark.
- **Câble link distant** : deux consoles seulement, prévu pour un réseau local. La console qui cadence un transfert attend un aller-retour réseau : un jeu GBA qui enchaîne les transferts peut ralentir. Essayé entre un téléphone et un PC : un échange Pokémon Émeraude ↔ Saphir va au bout, mais lentement. Détails dans [docs/remote_link.md](docs/remote_link.md).
- **États instantanés** : un état ne se recharge pas dans une version plus ancienne que celle qui l'a créé. Les changements de format sont listés dans le [journal des modifications](CHANGELOG.md).

## Versions

Version actuelle : **0.14.0**. Chaque version est publiée sur la page des [releases](https://github.com/GhostPunishR/RavenCore/releases) avec les cœurs Libretro pour Linux, Windows, macOS et Android. Ses évolutions sont détaillées dans le [journal des modifications](CHANGELOG.md).

Avant la 1.0, une version peut changer l'API ou le format des états ; le journal le signale à chaque fois.

## Contribuer

Les contributions sont les bienvenues. Lisez le [guide de contribution](CONTRIBUTING.md), qui couvre l'organisation du code, les règles et les pull requests, ainsi que le [code de conduite](CODE_OF_CONDUCT.md). L'essentiel :

- une correction matérielle s'accompagne d'un test de régression ;
- aucun code repris d'un autre émulateur, aucune ROM ni aucun BIOS ;
- tout changement notable va dans la section « Non publié » du [journal](CHANGELOG.md).

## Licence

Projet développé par **GhostPunishR** sous licence [GNU AGPL-3.0](LICENSE). Le fichier `libretro.h` conserve sa licence MIT. Voir [NOTICE.md](NOTICE.md) pour les attributions.

Aucune ROM ni aucun BIOS commercial n'est fourni. RavenCore n'est affilié ni à Nintendo, ni à RetroArch, ni à RetroAchievements.
