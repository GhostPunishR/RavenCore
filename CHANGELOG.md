# Journal des modifications

Ce fichier recense les évolutions notables de RavenEmu Cores. Son format suit [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/) et le projet applique le [versionnage sémantique](https://semver.org/lang/fr/). Avant la version 1.0, une version mineure peut changer l'API ou le format des états instantanés : chaque changement de ce type est signalé ici.

## Non publié

Travaux pour sortir RavenGBA de l'état expérimental ([#9](https://github.com/GhostPunishR/RavenEmu-Cores/pull/9)). Le cœur reste marqué expérimental tant qu'il n'a pas été essayé sur un large panel de jeux du commerce.

### Ajouté

#### Validation GBA

- `gba_conformance_runner` exécute une ROM de test publique jusqu'à sa boucle finale, puis vérifie un registre de résultat et des mots mémoire. Il refuse une ROM dont le SHA-256 diffère de celui attendu.
- `external_test_roms.txt` fige 15 ROMs publiques sur un commit et un SHA-256 : les 10 de [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests) (MIT) et les 5 de [FuzzARM](https://github.com/DenSinH/FuzzARM) (GPL-3.0, 10 000 tests ARM et Thumb aléatoires chacune). `fetch_test_roms.sh` les télécharge ; la CI Linux et la passe ASan/UBSan les exécutent. Aucune ROM n'est stockée dans le dépôt.
- `gba_cpu_test` et `gba_hardware_test` vérifient chaque comportement corrigé ; chaque test échoue si l'on retire la fonctionnalité qu'il couvre.

#### Cartouches GBA

- Vibration de Drill Dozer et de WarioWare: Twisted! (broche 3 du GPIO), reconnus par leur code de jeu. Elle est transmise à la manette par l'adaptateur Libretro et exposée par `ravenemu_v3_rumble_active()`.
- Port série sans câble : les transferts en horloge interne et en mode multijoueur se terminent et lèvent leur interruption. Les jeux qui attendaient la fin d'un transfert ne restent plus bloqués.

#### Libretro / RetroArch

- Options du cœur GBA, appliquées au chargement du jeu : type de sauvegarde (`ravenemu_gba_save_type`) et horloge de cartouche (`ravenemu_gba_rtc`), pour les jeux dont la détection automatique échoue.

#### API C

- `ravenemu_v3_set_gba_save_type()` et `ravenemu_v3_set_gba_rtc()` imposent le type de sauvegarde et l'horloge d'un jeu GBA ; le réglage prend effet au prochain chargement ou reset. `ravenemu_v3_rumble_active()` indique si le moteur de vibration de la cartouche (GB MBC5 Rumble ou GBA) tourne. Ces ajouts ne changent pas `RAVENEMU_V3_API_VERSION`.

### Modifié

- **Format d'état GBA 12.** Il ajoute le verrou de lecture du BIOS, le pipeline du CPU, la valeur du bus ouvert, le transfert série en cours et l'accès séquentiel en attente du bus. Les états au format 11 se rechargent toujours ; un état au format 12 ne se recharge pas dans la version 0.1.0.
- **Performances GBA.** Une image où le CPU tourne sans arrêt passe de 7,4 ms à 2,5 ms environ sur PC (code Thumb : 1,8 ms). Les périphériques avancent par lots jusqu'au prochain événement, les accès mémoire alignés vont directement au tableau concerné, et les temps d'attente viennent d'une table. Chaque optimisation a été vérifiée au bit près : l'image, le son et l'état de chaque frame sont identiques à ceux du cœur précédent sur les ROMs de test.

### Corrigé

#### CPU ARM7TDMI et bus GBA

- CMP, CMN, TST et TEQ avec S et Rd = PC copient SPSR dans CPSR.
- STR, STRH et STM de PC écrivent l'adresse de l'instruction + 12 (STM Thumb : + 6).
- LDM et STM avec une liste vide transfèrent PC et déplacent la base de 0x40. STM écrit la nouvelle base si celle-ci n'est pas le premier registre de la liste. LDM et STM avec S, sans PC, transfèrent les registres du mode utilisateur.
- LDRH non aligné effectue une rotation de 8 bits ; LDRSH non aligné charge un octet signé.
- Un pipeline de deux instructions : une écriture sur les instructions suivantes ne change plus ce qui s'exécute (code automodifiant, jeux NES Classic).
- Protection du BIOS : hors du BIOS, sa lecture renvoie le dernier mot que le pipeline y a lu.
- Une zone non câblée renvoie la dernière instruction préchargée (bus ouvert) ; une lecture au-delà de la fin de la ROM renvoie la valeur du bus d'adresse.
- Une SRAM ou une EEPROM vierge et une puce absente se lisent 0xFF. SRAM et Flash sont sur un bus 8 bits.
- MUL, MLA, UMULL et SMULL prennent un nombre de cycles qui dépend de l'opérande ; une opération avec décalage par registre coûte un cycle de plus.

#### BIOS haut niveau, PPU et DMA GBA

- Nouveaux appels : GetBiosChecksum, SoundBias, MidiKey2Freq, HardReset et CustomHalt. MultiBoot signale un échec (pas de câble) ; SoundDriverVSyncOff et On sont sans effet.
- SoftReset efface 0x03007E00 à 0x03007FFF et repart en EWRAM si l'octet 0x03007FFA est non nul (jeux MultiBoot).
- Écrire HALTCNT arrête le CPU. Un CPU arrêté se réveille sur l'événement exact qui lève son interruption, et non plus par pas de 64 cycles.
- Les sprites se dessinent dans la limite de 1210 cycles par ligne (954 si l'accès VRAM pendant le H-Blank est libéré).
- Le DMA3 en mode spécial capture la vidéo des lignes 2 à 161.
- Un état instantané repartait quelques cycles à côté après son chargement ; la reprise est désormais exacte au cycle près.

## 0.1.0 - 2026-10-12

Première version de RavenEmu Cores : les moteurs GB, GBC et GBA de [RavenEmu](https://github.com/GhostPunishR/RavenEmu), importés depuis le commit `312e4af` et rendus utilisables sans l'application Android. Ils sont livrés avec trois adaptateurs Libretro, une API C et un paquet CMake.

### Ajouté

#### Moteurs C++20

- **RavenGB** (Game Boy) et **RavenGBC** (Game Boy Color, mode natif et mode de compatibilité pour les cartouches GB) :
  - CPU précis au M-cycle, PPU à pipeline de pixels (FIFO), APU, timer, port série, infrarouge CGB et double vitesse.
  - Cartouches ROM seule, MBC1, MBC2, MBC3 avec horloge, MBC5 avec vibration, MBC6, MBC7 avec accéléromètre, HuC1, HuC3 et MMM01.
  - Démarrage sans boot ROM ; une boot ROM DMG/CGB acquise légalement peut être fournie.
- **RavenGBA** (Game Boy Advance, expérimental) :
  - CPU ARM7TDMI, PPU, APU (canaux PSG et Direct Sound), DMA, timers et BIOS haut niveau : aucun fichier BIOS n'est nécessaire.
  - Sauvegardes SRAM, Flash 64 et 128 Kio, EEPROM 512 o et 8 Kio, et horloge Seiko, détectées automatiquement ou imposées par l'appelant.
- Interface commune `ravenemu::Core` ([`core.hpp`](cores/common/include/ravenemu/core.hpp)) : vidéo, audio stéréo, boutons, sauvegardes batterie avec suivi des générations, états instantanés, câble link, infrarouge et lecture de la mémoire pour les succès.
- Harnais de conformité GB/GBC pour exécuter des ROMs de test externes, sans les ajouter au dépôt.

#### Libretro / RetroArch

- Trois cœurs : `ravenemu_gb_libretro`, `ravenemu_gbc_libretro` et `ravenemu_gba_libretro`, avec leurs fichiers `.info` pour RetroArch.
- Noms des boutons dans l'écran de configuration des touches de RetroArch ([#7](https://github.com/GhostPunishR/RavenEmu-Cores/pull/7)).
- Vibration des cartouches MBC5 Rumble transmise à la manette ([#7](https://github.com/GhostPunishR/RavenEmu-Cores/pull/7)).
- Inclinaison des cartouches MBC7 (Kirby Tilt 'n' Tumble, Command Master) pilotée par le stick analogique gauche ([#7](https://github.com/GhostPunishR/RavenEmu-Cores/pull/7)).

#### API C et SDK

- API C `bridge.h` pour les frontends non C++ : chargement de ROM, image ARGB8888, audio PCM16, boutons, sauvegardes batterie et états instantanés ([#3](https://github.com/GhostPunishR/RavenEmu-Cores/pull/3)).
- Vérification de version : `ravenemu_v3_api_version()`, `RAVENEMU_V3_API_VERSION` et `ravenemu_v3_version()` ([#7](https://github.com/GhostPunishR/RavenEmu-Cores/pull/7)).
- Suivi des écritures batterie : `ravenemu_v3_battery_dirty()`, `ravenemu_v3_copy_battery_generation()` et `ravenemu_v3_acknowledge_battery()`. Un frontend n'écrit la sauvegarde que si elle a changé ([#7](https://github.com/GhostPunishR/RavenEmu-Cores/pull/7)).
- En-tête C++ `sdk.hpp` avec une fabrique simplifiée.
- Paquet CMake installable : `find_package(RavenEmuCores)`, puis les cibles `RavenEmu::ravenemu_cores` ou `RavenEmu::ravenemu_c_api` ([#5](https://github.com/GhostPunishR/RavenEmu-Cores/pull/5)).

#### Build, CI et documentation

- Le build se fait en Release par défaut quand ce dépôt est le projet principal ([#5](https://github.com/GhostPunishR/RavenEmu-Cores/pull/5)).
- Une seule source de version, `project(VERSION)` de CMake : elle alimente les cœurs Libretro et leurs fichiers `.info` ([#7](https://github.com/GhostPunishR/RavenEmu-Cores/pull/7)).
- CI sur Linux, Windows (MSVC), macOS (Apple Clang) et Android (arm64-v8a et armeabi-v7a), avec une passe ASan/UBSan et l'installation du paquet CMake ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4), [#6](https://github.com/GhostPunishR/RavenEmu-Cores/pull/6)).
- Les actions GitHub tierces sont épinglées sur un commit précis ([#6](https://github.com/GhostPunishR/RavenEmu-Cores/pull/6)).
- Test de mutation des états instantanés : des états GB, GBC et GBA altérés doivent être refusés proprement ou s'exécuter sans erreur ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4)).
- README, guide de contribution, code de conduite, politique de sécurité, modèles d'issue et de pull request, et `NOTICE.md` sur la provenance des sources.

### Corrigé

#### Sécurité : états instantanés et sauvegardes non fiables

Un état instantané ou une sauvegarde peut venir d'un tiers. Ces corrections empêchent un fichier forgé de planter ou de corrompre l'application hôte.

- Canaux audio GB/GBC : les valeurs restaurées sont bornées, et un filtre audio non fini est refusé ([#1](https://github.com/GhostPunishR/RavenEmu-Cores/pull/1)).
- FIFO d'arrière-plan du PPU GB/GBC : une couleur ou une palette hors limites est refusée ([#2](https://github.com/GhostPunishR/RavenEmu-Cores/pull/2)).
- Sprites retenus par le PPU GB/GBC : un index forgé lisait hors de l'OAM et plantait l'hôte à la frame suivante ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4)).
- Timers GBA : un rechargement de `0x10000` provoquait une division par zéro ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4)).
- Horloge GBA : la position de transfert, restaurée sans contrôle, indexait hors du tampon ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4)).
- MBC1, MBC2, MBC3 et MBC5 : un numéro de banque extrême débordait l'arithmétique signée. L'horloge MBC3 calcule désormais le temps écoulé sans débordement ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4)).
- Registres restants bornés aux valeurs que le matériel peut produire ([#5](https://github.com/GhostPunishR/RavenEmu-Cores/pull/5)) :
  - GB : SP, PC, IF, IE, timer et compteurs du PPU.
  - GBA : touches, IE, IF et GPIO.
- Les états existants restent chargeables.

#### Sauvegardes, audio et horloge

- GBA : les états conservent maintenant les canaux PSG, les FIFO Direct Sound et les échantillons en attente. Le son reprend exactement après un chargement ([#1](https://github.com/GhostPunishR/RavenEmu-Cores/pull/1)).
- GBA : une EEPROM détectée par sa signature réserve 8 Kio dès le chargement, car la signature ne distingue pas 512 o de 8 Kio. Les anciens fichiers de 512 o restent importables ([#1](https://github.com/GhostPunishR/RavenEmu-Cores/pull/1)).
- GBA : la lecture audio ne coupe plus une trame stéréo en deux. Avant, une capacité impaire inversait la gauche et la droite pour toute la suite ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4)).
- MBC3 : sauvegarder la batterie ne remplace plus les registres d'horloge verrouillés par le jeu ([#2](https://github.com/GhostPunishR/RavenEmu-Cores/pull/2)).

#### Build et portabilité

- L'en-tête public `ravenemu/gba/core.hpp` ne compilait pas ([#4](https://github.com/GhostPunishR/RavenEmu-Cores/pull/4)).
- La lecture des entiers big-endian ne dépend plus de l'ordre d'évaluation du compilateur : avec MSVC, elle pouvait rendre tous les états illisibles ([#5](https://github.com/GhostPunishR/RavenEmu-Cores/pull/5)).
- Les cœurs Libretro n'exportent plus que les fonctions `retro_*`. Avant, deux cœurs chargés dans le même frontend pouvaient lier les symboles C++ l'un de l'autre ([#5](https://github.com/GhostPunishR/RavenEmu-Cores/pull/5)).
- Les avertissements stricts (`-Wconversion`, `/W4`…) ne s'imposent plus aux projets qui utilisent les cœurs ([#5](https://github.com/GhostPunishR/RavenEmu-Cores/pull/5)).
- Windows : les tests des moteurs disposent d'une pile de 16 Mio, car la pile par défaut de 1 Mio débordait ([#6](https://github.com/GhostPunishR/RavenEmu-Cores/pull/6)).
- Cibles 32 bits (armeabi-v7a, x86) : la lecture de la mémoire des succès ne produit plus d'avertissements de conversion ([#6](https://github.com/GhostPunishR/RavenEmu-Cores/pull/6)).
- MSVC : les tests Libretro sont compilés en C++20 ([#6](https://github.com/GhostPunishR/RavenEmu-Cores/pull/6)).
- Métadonnées du cœur Libretro Game Boy Color corrigées.

### Performances

Ces gains ont été mesurés sur Linux x86-64, en gardant la meilleure de 7 séries ([#8](https://github.com/GhostPunishR/RavenEmu-Cores/pull/8)).

| Cas | Avant | Après |
| --- | --- | --- |
| Taille d'un état GB/GBC | 268 ko | 176 ko |
| Sauvegarde d'un état GB/GBC | 170 µs | 60 µs |
| Chargement d'un état GB/GBC | 85 µs | 45 µs |
| Chargement d'un état GBA (ROM de 32 Mio) | 17,3 ms | 88 µs |
| API C : `state_size` puis `save_state` | 354 µs | 65 à 90 µs |

Voici l'origine de ces gains :

- Les entiers d'un état sont écrits et vérifiés en une seule fois, et non plus octet par octet.
- Les images de l'écran GB/GBC sont stockées sur 16 bits par pixel.
- La ROM GBA n'est analysée qu'au chargement, et non plus à chaque reset ou chargement d'état.
- L'API C ne sérialise plus deux fois.
- Libretro ne recopie la batterie dans `SAVE_RAM` que si le jeu l'a modifiée, et non plus à chaque frame.

### Compatibilité des états instantanés

- **GB/GBC : format 11.** Les états au format 10 se rechargent toujours. Un état au format 11 ne se recharge pas dans une version antérieure ([#8](https://github.com/GhostPunishR/RavenEmu-Cores/pull/8)).
- **GBA : format 11.** Il inclut désormais l'audio. Les états GBA au format 10 sont refusés, mais les sauvegardes batterie restent utilisables ([#1](https://github.com/GhostPunishR/RavenEmu-Cores/pull/1)).
- **API C : version 0.** C'est une préversion : son ABI n'est pas encore stabilisée.

### Limites connues

- RetroAchievements n'est pas activé dans les adaptateurs Libretro.
- GBA reste expérimental : la compatibilité et les performances sont encore limitées.
- Le sens de l'inclinaison MBC7 découle de la physique de l'accéléromètre, mais il n'a pas été vérifié en jeu.
- La vibration est échantillonnée une fois par frame : les impulsions plus courtes ne sont pas reproduites.
- Les fichiers Libretro produits par la CI ne fonctionnent que sur Linux x86-64. Ils ne sont pas distribués par le téléchargeur officiel de RetroArch.
