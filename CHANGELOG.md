# Journal des modifications

Ce fichier recense les évolutions notables de RavenCore. Son format suit [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/) et le projet applique le [versionnage sémantique](https://semver.org/lang/fr/). Avant la version 1.0, une version mineure peut changer l'API ou le format des états instantanés : chaque changement de ce type est signalé ici.

## 0.4.1 - 2026-10-16

Cette version corrige le redémarrage des DMA GBA ([#21](https://github.com/GhostPunishR/RavenCore/pull/21)), qui laissait GTA Advance sur un écran noir au début d'une nouvelle partie. L'API et les formats ne changent pas.

### Corrigé

- **GBA : réécriture du contrôle d'un DMA déjà actif.** Un canal ne recharge ses adresses et ne lance un transfert immédiat que quand le bit 15 de son contrôle passe de 0 à 1, comme le décrit GBATEK. Le cœur relançait le canal à chaque écriture où ce bit valait 1. GTA Advance arrête son DMA de son en réécrivant `0xB600` en `0x8400`, bit 15 gardé, avant d'effacer ce bit : le cœur lançait alors 0x4000 mots depuis FIFO A à travers tous les registres d'E/S, et le jeu restait sur un écran noir après le choix du nom. Une réécriture garde désormais le canal en l'état et ne change que ses réglages.

### Compatibilité des états instantanés

- **GBA : format 13** et **GB/GBC : format 11**, inchangés : les états créés avec la version 0.4.0 se rechargent. L'état d'activation de chaque canal DMA se déduit des registres d'E/S restaurés.
- **API C : version 0**, inchangée. Le paquet CMake se demande toujours avec `find_package(RavenCore 0.4)`.

## 0.4.0 - 2026-10-16

Cette version retire les derniers restes de l'ancien nom, RavenEmu Cores ([#20](https://github.com/GhostPunishR/RavenCore/pull/20)). Les cibles du paquet CMake changent de nom, d'où la nouvelle version mineure. Les moteurs, l'API C et les formats des états ne changent pas.

### Supprimé

- **Ancien nom du paquet CMake** : `find_package(RavenEmuCores)` n'existe plus, il faut écrire `find_package(RavenCore)`. Il était annoncé jusqu'à la version 1.0 ; il part plus tôt, avec le reste de l'ancien nom.
- **Option `RAVENEMU_BUILD_TESTS`** : elle ne faisait que recopier `BUILD_TESTING`, qui décide seul désormais de la construction des tests.

### Modifié

- **Cibles du paquet CMake** : l'espace de noms devient `RavenCore::`. `RavenEmu::ravenemu_cores` devient `RavenCore::ravencore` et `RavenEmu::ravenemu_c_api` devient `RavenCore::ravenemu_c_api`. Un projet qui consomme une installation de RavenCore doit adapter son `target_link_libraries()`.
- **Inclusion par `add_subdirectory()` ou FetchContent** : la cible C++ `ravenemu_cores` devient `ravencore`. `ravenemu_c_api`, `ravenemu_common`, `gb_raven_core`, `gbc_raven_core` et `gba_raven_core` gardent leur nom. Le dossier `cores/` ne déclare plus son propre `project()`, vestige de l'import des moteurs depuis RavenEmu : les variables `ravenemu_cores_*` qu'il créait disparaissent. Un projet qui déclare RavenCore avec FetchContent sous le nom `ravenemu_cores`, comme RavenEmu, garde sa variable `ravenemu_cores_SOURCE_DIR`, qui vient de FetchContent.

Le préfixe `ravenemu` reste celui de l'API : fonctions C `ravenemu_*`, espace de noms C++ `ravenemu::`, en-têtes `ravenemu/`, options `RAVENEMU_*` et cœurs Libretro `ravenemu_*_libretro`.

### Compatibilité des états instantanés

- **GBA : format 13** et **GB/GBC : format 11**, inchangés : les états créés avec la version 0.3 se rechargent.
- **API C : version 0**, inchangée.
- Le paquet CMake se demande désormais avec `find_package(RavenCore 0.4)`.

## 0.3.1 - 2026-10-16

Cette version corrige le rendu des objets semi-transparents GBA ([#19](https://github.com/GhostPunishR/RavenCore/pull/19)), qui masquaient le décor de Pokémon Émeraude sous son brouillard. L'API et les formats ne changent pas.

### Corrigé

- **GBA : objets semi-transparents dans une fenêtre sans effets.** Un objet semi-transparent (mode 1 de l'attribut 0) se mélange désormais à la 2e cible même dans une fenêtre dont le bit « effets spéciaux » de `WININ`/`WINOUT` est à 0. Pokémon Émeraude couvre toute sa carte d'une telle fenêtre, si bien que son brouillard, opaque, masquait entièrement le décor. Les effets normaux restent coupés par la fenêtre.

### Compatibilité des états instantanés

- **GBA : format 13** et **GB/GBC : format 11**, inchangés : les états créés avec la version 0.3.0 se rechargent.
- **API C : version 0**, inchangée. Le paquet CMake se demande toujours avec `find_package(RavenCore 0.3)`.

## 0.3.0 - 2026-10-16

Cette version émule les capteurs des cartouches GBA ([#16](https://github.com/GhostPunishR/RavenCore/pull/16)) et rend jouables les jeux Classic NES Series ([#17](https://github.com/GhostPunishR/RavenCore/pull/17)). Le format des états GBA change, d'où la nouvelle version mineure ; l'API C ne fait que s'enrichir.

### Ajouté

- **Capteurs des cartouches GBA**, d'après GBATEK et reconnus par le code de jeu :
  - capteur solaire de Boktai 1 à 3 : compteur sur le GPIO et comparateur, de E8h dans l'obscurité à 50h en plein soleil ;
  - gyroscope de WarioWare: Twisted! : convertisseur série 12 bits sur le GPIO, 6C0h au repos, aux côtés du moteur de vibration ;
  - inclinaison de Yoshi Topsy-Turvy et Koro Koro Puzzle : registres E008000h à E008500h dans la zone SRAM.
- API C++ : `Core::cartridge_sensors()`, `set_solar_light()`, `set_rotation_speed()` et `set_gba_tilt()`. Le cœur GB signale l'accéléromètre MBC7.
- API C : `ravenemu_sensors()`, `ravenemu_set_solar_light()`, `ravenemu_set_rotation()` et `ravenemu_set_tilt()`, qui couvre aussi l'accéléromètre MBC7. Ces ajouts ne changent pas `RAVENEMU_API_VERSION`.
- Libretro : gyroscope et inclinaison GBA au stick analogique gauche, et option `ravenemu_gba_solar_light` pour la lumière du capteur solaire, modifiable en cours de partie.

### Corrigé

- **GBA : jeux Classic NES Series (Famicom Mini).** Sur une cartouche à EEPROM, la zone SRAM (`0x0E000000`) est vide : elle se lit 0xFF et les écritures s'y perdent. Le cœur y plaçait jusqu'ici une SRAM de repli. Or ces jeux écrivent puis relisent cette zone pour reconnaître une copie équipée de SRAM, et affichaient « GAME PAK ERROR » (vérifié avec The Legend of Zelda). La SRAM de repli reste quand aucun type de sauvegarde n'est détecté.

### Compatibilité des états instantanés

- **GBA : format 13**, qui ajoute l'état des capteurs en fin d'état. Les formats 12 et 11 se rechargent, capteurs à leur état initial.
- **GB/GBC : format 11**, inchangé.
- **API C : version 0**, inchangée : les nouvelles fonctions s'ajoutent sans modifier les existantes.
- Le paquet CMake se demande désormais avec `find_package(RavenCore 0.3)`.

## 0.2.0 - 2026-10-15

Cette version simplifie les noms de l'API C ([#14](https://github.com/GhostPunishR/RavenCore/pull/14)). Le changement est incompatible pour les programmes qui utilisent cette API : c'est pourquoi la version mineure augmente. Les moteurs, l'API C++, les cœurs Libretro et les formats de sauvegarde ne changent pas.

### Modifié

- Suppression des marqueurs de génération dans les noms du SDK C : fonctions et types `ravenemu_*`, constantes `RAVENEMU_*`. Par exemple, `ravenemu_v3_create()` devient `ravenemu_create()` et `RAVENEMU_V3_GBA` devient `RAVENEMU_GBA`. Les consommateurs doivent adapter leurs noms d'appel et être recompilés.
- Le paquet CMake se demande désormais avec `find_package(RavenCore 0.2)`. Avant la 1.0, seules les versions de même mineure sont compatibles : `find_package(RavenCore 0.1)` ne trouve pas cette installation.

### Compatibilité des états instantanés

- **GB/GBC : format 11**, et **GBA : format 12**, tous deux inchangés. Les états créés avec la version 0.1.2 se rechargent.
- **API C : version 0**, inchangée. `RAVENEMU_API_VERSION` reste à 0 tant que l'API est en préversion ; seuls les noms changent.

## 0.1.2 - 2026-10-13

Cette version valide RavenGB et RavenGBC sur les suites de tests publiques et corrige les bugs qu'elles ont révélés ([#13](https://github.com/GhostPunishR/RavenCore/pull/13)). Le dépôt et le paquet CMake s'appellent désormais RavenCore ([#12](https://github.com/GhostPunishR/RavenCore/pull/12)).

### Ajouté

#### Validation GB/GBC

- `public-test-roms.json` fige 198 tests issus de l'archive publique [game-boy-test-roms v7.0](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0) : Mooneye Test Suite (profils DMG et CGB, tests MBC), les ROMs de Blargg (`cpu_instrs`, `instr_timing`, `mem_timing`, `mem_timing-2`, `halt_bug`, `interrupt_time`, `oam_bug`, `dmg_sound`, `cgb_sound`), dmg-acid2 et cgb-acid2. `fetch_test_roms.sh` télécharge l'archive et vérifie son SHA-256, puis l'orchestrateur vérifie celui de chaque ROM et de chaque capture. Avec `-DRAVENEMU_GB_TEST_ROMS=DOSSIER`, chaque test devient un test CTest `gb_rom_<id>` ; la CI Linux et la passe ASan/UBSan les exécutent. Aucune ROM n'est stockée dans le dépôt.
- `gb_conformance_runner` compare l'image à une capture RGB888 (`--expect-frame-rgb888`) dans la palette commune des captures de référence, avec les palettes de la boot ROM CGB en mode compatibilité (`--compatibility-palette boot-default`). `--expect-memory-before` exige un état intermédiaire avant d'accepter une réussite mémoire. Le runner signale l'échec Mooneye (`0x42` dans tous les registres) au lieu d'attendre le timeout.
- `conformance_manifest.py` lit les captures PNG sans dépendance externe (critère `screenshot`), accepte `memory_before` et sélectionne un test avec `--id`.

### Modifié

- Le dépôt s'appelle désormais **RavenCore** (`GhostPunishR/RavenCore`) ; les anciennes adresses `RavenEmu-Cores` redirigent.
- Le paquet CMake s'appelle désormais `RavenCore` : `find_package(RavenCore)`. Les cibles `RavenEmu::ravenemu_cores` et `RavenEmu::ravenemu_c_api` ne changent pas.
- Pour un projet qui inclut ce dépôt avec `add_subdirectory()`, les variables créées par `project()` s'appellent désormais `RavenCore_*` (par exemple `RavenCore_SOURCE_DIR`), et non plus `RavenEmuCores_*`.
- Le README est plus court et plus lisible ; la procédure de publication d'une version passe dans le [guide de contribution](https://github.com/GhostPunishR/RavenCore/blob/main/CONTRIBUTING.md).

### Obsolète

- L'ancien nom du paquet, `find_package(RavenEmuCores)`, fonctionne toujours mais affiche un avertissement de dépréciation. Il charge `RavenCore` depuis la même installation et sera retiré à la version 1.0.

### Corrigé

Ces corrections font passer les onze ROMs de Blargg qui échouaient. Le format des états instantanés GB/GBC ne change pas ; un état existant se charge comme avant.

- **Son (DMG et CGB).** Rallumer l'APU par NR52 remet le séquenceur de trames à l'étape 0, toujours cadencé par DIV (`dmg_sound` et `cgb_sound` 07).
- **Canal d'onde.** Le premier échantillon est lu 6 cycles après une période complète suivant le déclenchement (`09`, `12`). Sur DMG, la RAM d'onde n'est accessible pendant que le canal joue que juste après une lecture, et le redéclenchement ne corrompt cette RAM que si la lecture suivante tombe au cycle suivant, avec l'octet qui allait être lu (`10`).
- **Bug de corruption de l'OAM (DMG).** Une incrémentation ou décrémentation 16 bits (`INC rr`, `DEC rr`, `LD A,[HL±]`, `PUSH`, `POP`, appel d'interruption) dont la valeur tombe dans `$FE00`–`$FEFF` pendant le mode 2 corrompt l'OAM selon les formules de Pan Docs. `ADD HL,rr`, `ADD SP,e` et `LD HL,SP+e` ne la déclenchent pas. Le CGB n'est pas concerné. La ROM `oam_bug` de Blargg passe.

### Sécurité

- **HDMA GBC.** Une fois la destination arrivée en fin de VRAM, relancer un transfert sans réécrire `FF53`/`FF54` écrivait un octet après la banque, de plus en plus loin à chaque relance : la banque 1 débordait du tableau de la VRAM. L'état instantané produit était ensuite refusé au chargement. Le transfert s'arrête désormais en fin de banque, et un état pris pendant cette relance se recharge.

### Compatibilité des états instantanés

- **GB/GBC : format 11**, et **GBA : format 12**, tous deux inchangés. Les états créés avec la version 0.1.1 se rechargent.
- **API C : version 0**, inchangée.

## 0.1.1 - 2026-10-12

Cette version regroupe les travaux pour sortir RavenGBA de l'état expérimental ([#9](https://github.com/GhostPunishR/RavenCore/pull/9)) et la lecture de la mémoire pour RetroAchievements dans l'API C ([#10](https://github.com/GhostPunishR/RavenCore/pull/10)). RavenGBA reste marqué expérimental tant qu'il n'a pas été essayé sur un large panel de jeux du commerce.

### Ajouté

#### Validation GBA

- `gba_conformance_runner` exécute une ROM de test publique jusqu'à sa boucle finale, puis vérifie un registre de résultat et des mots mémoire. Il refuse une ROM dont le SHA-256 diffère de celui attendu.
- `external_test_roms.txt` fige 15 ROMs publiques sur un commit et un SHA-256 : les 10 de [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests) (MIT) et les 5 de [FuzzARM](https://github.com/DenSinH/FuzzARM) (GPL-3.0, 10 000 tests ARM et Thumb aléatoires chacune). `fetch_test_roms.sh` les télécharge ; la CI Linux et la passe ASan/UBSan les exécutent. Aucune ROM n'est stockée dans le dépôt.
- `gba_cpu_test` et `gba_hardware_test` vérifient chaque comportement corrigé ; chaque test échoue si l'on retire la fonctionnalité qu'il couvre.

#### Cartouches GBA

- Vibration de Drill Dozer et de WarioWare: Twisted! (broche 3 du GPIO), reconnus par leur code de jeu. Elle est transmise à la manette par l'adaptateur Libretro et exposée par `ravenemu_rumble_active()`.
- Port série sans câble : les transferts en horloge interne et en mode multijoueur se terminent et lèvent leur interruption. Les jeux qui attendaient la fin d'un transfert ne restent plus bloqués.

#### Libretro / RetroArch

- Options du cœur GBA, appliquées au chargement du jeu : type de sauvegarde (`ravenemu_gba_save_type`) et horloge de cartouche (`ravenemu_gba_rtc`), pour les jeux dont la détection automatique échoue.

#### API C

- `ravenemu_read_achievement_memory()` lit l'espace d'adresses utilisé par RetroAchievements : carte mémoire de la console, RAM de cartouche et banques de WRAM CGB pour GB/GBC ; IWRAM, EWRAM puis sauvegarde pour GBA. La lecture n'avance pas l'émulation et ne passe pas par le bus ; une zone non exposée donne une lecture plus courte que demandé ([#10](https://github.com/GhostPunishR/RavenCore/pull/10)).
- `ravenemu_set_gba_save_type()` et `ravenemu_set_gba_rtc()` imposent le type de sauvegarde et l'horloge d'un jeu GBA ; le réglage prend effet au prochain chargement ou reset. `ravenemu_rumble_active()` indique si le moteur de vibration de la cartouche (GB MBC5 Rumble ou GBA) tourne. Ces ajouts ne changent pas `RAVENEMU_API_VERSION`.

#### Publication

- Chaque tag `vX.Y.Z` publie une release GitHub : ses notes sont la section de ce journal, et les cœurs Libretro Linux x86-64 y sont joints avec leurs fichiers `.info`.

### Modifié

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

### Compatibilité des états instantanés

- **GBA : format 12.** Il ajoute le verrou de lecture du BIOS, le pipeline du CPU, la valeur du bus ouvert, le transfert série en cours et l'accès séquentiel en attente du bus. Les états au format 11 se rechargent toujours ; un état au format 12 ne se recharge pas dans la version 0.1.0.
- **Drill Dozer et WarioWare: Twisted!** : un état de la version 0.1.0, sans port GPIO, se recharge, moteur au repos.
- **GB/GBC : format 11**, inchangé.
- **API C : version 0**, inchangée : les nouvelles fonctions sont des ajouts.

### Limites connues

- GBA reste expérimental : il réussit les suites de tests publiques, mais n'a pas encore été essayé sur un large panel de jeux du commerce.
- Ne sont pas émulés : les capteurs solaire, gyroscopique et d'inclinaison de certaines cartouches GBA, le câble link GBA et le mode UART du port série, le tampon de préchargement de la cartouche et l'exception d'instruction indéfinie.
- La sauvegarde GBA est détectée par la signature de la bibliothèque du jeu, sans base de données par jeu ; les options Libretro et l'API C corrigent une détection erronée.
- RetroAchievements n'est toujours pas activé dans les adaptateurs Libretro.

## 0.1.0 - 2026-10-12

Première version de RavenCore, alors nommé RavenEmu Cores : les moteurs GB, GBC et GBA de [RavenEmu](https://github.com/GhostPunishR/RavenEmu), importés depuis le commit `312e4af` et rendus utilisables sans l'application Android. Ils sont livrés avec trois adaptateurs Libretro, une API C et un paquet CMake.

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
- Noms des boutons dans l'écran de configuration des touches de RetroArch ([#7](https://github.com/GhostPunishR/RavenCore/pull/7)).
- Vibration des cartouches MBC5 Rumble transmise à la manette ([#7](https://github.com/GhostPunishR/RavenCore/pull/7)).
- Inclinaison des cartouches MBC7 (Kirby Tilt 'n' Tumble, Command Master) pilotée par le stick analogique gauche ([#7](https://github.com/GhostPunishR/RavenCore/pull/7)).

#### API C et SDK

- API C `bridge.h` pour les frontends non C++ : chargement de ROM, image ARGB8888, audio PCM16, boutons, sauvegardes batterie et états instantanés ([#3](https://github.com/GhostPunishR/RavenCore/pull/3)).
- Vérification de version : `ravenemu_api_version()`, `RAVENEMU_API_VERSION` et `ravenemu_version()` ([#7](https://github.com/GhostPunishR/RavenCore/pull/7)).
- Suivi des écritures batterie : `ravenemu_battery_dirty()`, `ravenemu_copy_battery_generation()` et `ravenemu_acknowledge_battery()`. Un frontend n'écrit la sauvegarde que si elle a changé ([#7](https://github.com/GhostPunishR/RavenCore/pull/7)).
- En-tête C++ `sdk.hpp` avec une fabrique simplifiée.
- Paquet CMake installable : `find_package(RavenEmuCores)`, puis les cibles `RavenEmu::ravenemu_cores` ou `RavenEmu::ravenemu_c_api` ([#5](https://github.com/GhostPunishR/RavenCore/pull/5)).

#### Build, CI et documentation

- Le build se fait en Release par défaut quand ce dépôt est le projet principal ([#5](https://github.com/GhostPunishR/RavenCore/pull/5)).
- Une seule source de version, `project(VERSION)` de CMake : elle alimente les cœurs Libretro et leurs fichiers `.info` ([#7](https://github.com/GhostPunishR/RavenCore/pull/7)).
- CI sur Linux, Windows (MSVC), macOS (Apple Clang) et Android (arm64-v8a et armeabi-v7a), avec une passe ASan/UBSan et l'installation du paquet CMake ([#4](https://github.com/GhostPunishR/RavenCore/pull/4), [#6](https://github.com/GhostPunishR/RavenCore/pull/6)).
- Les actions GitHub tierces sont épinglées sur un commit précis ([#6](https://github.com/GhostPunishR/RavenCore/pull/6)).
- Test de mutation des états instantanés : des états GB, GBC et GBA altérés doivent être refusés proprement ou s'exécuter sans erreur ([#4](https://github.com/GhostPunishR/RavenCore/pull/4)).
- README, guide de contribution, code de conduite, politique de sécurité, modèles d'issue et de pull request, et `NOTICE.md` sur la provenance des sources.

### Corrigé

#### Sécurité : états instantanés et sauvegardes non fiables

Un état instantané ou une sauvegarde peut venir d'un tiers. Ces corrections empêchent un fichier forgé de planter ou de corrompre l'application hôte.

- Canaux audio GB/GBC : les valeurs restaurées sont bornées, et un filtre audio non fini est refusé ([#1](https://github.com/GhostPunishR/RavenCore/pull/1)).
- FIFO d'arrière-plan du PPU GB/GBC : une couleur ou une palette hors limites est refusée ([#2](https://github.com/GhostPunishR/RavenCore/pull/2)).
- Sprites retenus par le PPU GB/GBC : un index forgé lisait hors de l'OAM et plantait l'hôte à la frame suivante ([#4](https://github.com/GhostPunishR/RavenCore/pull/4)).
- Timers GBA : un rechargement de `0x10000` provoquait une division par zéro ([#4](https://github.com/GhostPunishR/RavenCore/pull/4)).
- Horloge GBA : la position de transfert, restaurée sans contrôle, indexait hors du tampon ([#4](https://github.com/GhostPunishR/RavenCore/pull/4)).
- MBC1, MBC2, MBC3 et MBC5 : un numéro de banque extrême débordait l'arithmétique signée. L'horloge MBC3 calcule désormais le temps écoulé sans débordement ([#4](https://github.com/GhostPunishR/RavenCore/pull/4)).
- Registres restants bornés aux valeurs que le matériel peut produire ([#5](https://github.com/GhostPunishR/RavenCore/pull/5)) :
  - GB : SP, PC, IF, IE, timer et compteurs du PPU.
  - GBA : touches, IE, IF et GPIO.
- Les états existants restent chargeables.

#### Sauvegardes, audio et horloge

- GBA : les états conservent maintenant les canaux PSG, les FIFO Direct Sound et les échantillons en attente. Le son reprend exactement après un chargement ([#1](https://github.com/GhostPunishR/RavenCore/pull/1)).
- GBA : une EEPROM détectée par sa signature réserve 8 Kio dès le chargement, car la signature ne distingue pas 512 o de 8 Kio. Les anciens fichiers de 512 o restent importables ([#1](https://github.com/GhostPunishR/RavenCore/pull/1)).
- GBA : la lecture audio ne coupe plus une trame stéréo en deux. Avant, une capacité impaire inversait la gauche et la droite pour toute la suite ([#4](https://github.com/GhostPunishR/RavenCore/pull/4)).
- MBC3 : sauvegarder la batterie ne remplace plus les registres d'horloge verrouillés par le jeu ([#2](https://github.com/GhostPunishR/RavenCore/pull/2)).

#### Build et portabilité

- L'en-tête public `ravenemu/gba/core.hpp` ne compilait pas ([#4](https://github.com/GhostPunishR/RavenCore/pull/4)).
- La lecture des entiers big-endian ne dépend plus de l'ordre d'évaluation du compilateur : avec MSVC, elle pouvait rendre tous les états illisibles ([#5](https://github.com/GhostPunishR/RavenCore/pull/5)).
- Les cœurs Libretro n'exportent plus que les fonctions `retro_*`. Avant, deux cœurs chargés dans le même frontend pouvaient lier les symboles C++ l'un de l'autre ([#5](https://github.com/GhostPunishR/RavenCore/pull/5)).
- Les avertissements stricts (`-Wconversion`, `/W4`…) ne s'imposent plus aux projets qui utilisent les cœurs ([#5](https://github.com/GhostPunishR/RavenCore/pull/5)).
- Windows : les tests des moteurs disposent d'une pile de 16 Mio, car la pile par défaut de 1 Mio débordait ([#6](https://github.com/GhostPunishR/RavenCore/pull/6)).
- Cibles 32 bits (armeabi-v7a, x86) : la lecture de la mémoire des succès ne produit plus d'avertissements de conversion ([#6](https://github.com/GhostPunishR/RavenCore/pull/6)).
- MSVC : les tests Libretro sont compilés en C++20 ([#6](https://github.com/GhostPunishR/RavenCore/pull/6)).
- Métadonnées du cœur Libretro Game Boy Color corrigées.

### Performances

Ces gains ont été mesurés sur Linux x86-64, en gardant la meilleure de 7 séries ([#8](https://github.com/GhostPunishR/RavenCore/pull/8)).

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

- **GB/GBC : format 11.** Les états au format 10 se rechargent toujours. Un état au format 11 ne se recharge pas dans une version antérieure ([#8](https://github.com/GhostPunishR/RavenCore/pull/8)).
- **GBA : format 11.** Il inclut désormais l'audio. Les états GBA au format 10 sont refusés, mais les sauvegardes batterie restent utilisables ([#1](https://github.com/GhostPunishR/RavenCore/pull/1)).
- **API C : version 0.** C'est une préversion : son ABI n'est pas encore stabilisée.

### Limites connues

- RetroAchievements n'est pas activé dans les adaptateurs Libretro.
- GBA reste expérimental : la compatibilité et les performances sont encore limitées.
- Le sens de l'inclinaison MBC7 découle de la physique de l'accéléromètre, mais il n'a pas été vérifié en jeu.
- La vibration est échantillonnée une fois par frame : les impulsions plus courtes ne sont pas reproduites.
- Les fichiers Libretro produits par la CI ne fonctionnent que sur Linux x86-64. Ils ne sont pas distribués par le téléchargeur officiel de RetroArch.
