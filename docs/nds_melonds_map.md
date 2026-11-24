# Cœur NDS : carte de melonDS

Ce document prépare le futur cœur Nintendo DS de RavenCore. Il décrit comment [melonDS](https://github.com/melonDS-emu/melonDS) est organisé : ses composants, son modèle temporel, sa mémoire, ce qu'il charge au démarrage et ce qu'il laisse de côté. Il propose ensuite un découpage et des étapes pour RavenCore.

Révision étudiée : `ff972523f8a70e877dd054439876c3c8c6b79197` (22 novembre 2026, melonDS 1.1), sous GPL 3.0 ou ultérieure. Les nombres de lignes citées sont ceux de cette révision.

## Règle de travail : une carte, pas une source

RavenCore n'intègre aucun code copié, traduit ou adapté d'un autre émulateur ([CONTRIBUTING.md](../CONTRIBUTING.md), règle 4). melonDS sert à **savoir où regarder** et à repérer les pièges. Il n'est ni une source de code ni une référence matérielle :

- **La référence matérielle reste [GBATEK](https://problemkaputt.de/gbatek.htm)** (sections « DS ») et les mesures sur console. Un comportement vu dans melonDS se vérifie dans GBATEK ou par un test avant d'être écrit.
- **Aucune table ni donnée de melonDS n'entre dans RavenCore.** Elle n'y entre ni recopiée, ni régénérée par un script. En particulier :

  | Donnée | Fichier de melonDS | Ce que fera RavenCore |
  | --- | --- | --- |
  | Base des jeux (code du jeu → taille de ROM, type de sauvegarde) | `ROMList.cpp`, 6 807 lignes | Détecter le type de sauvegarde par les commandes du jeu, avec un choix forcé possible |
  | BIOS de remplacement de DraStic | `FreeBIOS_Data.h`, `freebios/` | Écrire son propre BIOS HLE, comme pour le GBA |
  | Table Blowfish KEY1 | Lue dans le BIOS ARM7 de Nintendo | Aucune : démarrage direct, ROMs à zone sécurisée déchiffrée |
  | Firmware | Généré, ou lu dans un dump | Générer ses propres réglages utilisateur |

- **Le nom et les approximations de melonDS ne passent pas dans le code.** Une formule de timing de melonDS est une estimation de son modèle, pas une mesure. Elle ne remplace pas une mesure.

## Vue d'ensemble

Sans son interface Qt ni les bibliothèques qu'il embarque, melonDS compte environ 94 000 lignes de code propre dans `src/` :

- **le DSi, le JIT et les rendus OpenGL** en prennent environ 35 000 ;
- **la base de jeux** près de 7 000 ;
- **le cœur DS seul**, en rendu logiciel, environ 43 000 ;
- **le reste** va au réseau, aux images de disque FAT et aux utilitaires.

| Composant | Fichiers de melonDS | Lignes | Rôle |
| --- | --- | ---: | --- |
| Machine et ordonnanceur | `NDS.cpp`, `NDS.h`, `NDS_Header.h`, `MemRegion.h` | 5 131 | Boucle d'image, événements, bus des deux CPU, registres d'E/S, IRQ, IPC, diviseur et racine carrée, timers, démarrage direct |
| CPU ARM9 et ARM7 | `ARM.cpp`, `ARM.h`, `ARMInterpreter*.cpp`, `ARM_InstrTable.h`, `ARM_InstrInfo.*` | 7 718 | Interpréteurs ARMv5TE (ARM946E-S) et ARMv4T (ARM7TDMI) |
| Coprocesseur CP15 | `CP15.cpp` | 1 048 | ITCM, DTCM, cache d'instructions, unité de protection |
| DMA | `DMA.cpp`, `DMA_Timings.cpp` | 1 120 | Quatre canaux par CPU, modes de départ, rafales en RAM principale |
| Vidéo, partie commune | `GPU.cpp`, `GPU.h`, `GPU_Soft.cpp`, `GPU_ColorOp.h` | 3 233 | Lignes et images, banques de VRAM, capture d'écran, FIFO d'affichage, luminosité |
| Moteurs 2D A et B | `GPU2D.cpp`, `GPU2D_Soft.cpp` | 2 551 | Fonds, objets, fenêtres, effets, palettes étendues |
| Géométrie 3D | `GPU3D.cpp`, `GPU3D.h` | 3 271 | FIFO de commandes, matrices, éclairage, découpage, tests, échange des tampons |
| Rendu 3D logiciel | `GPU3D_Soft.*`, `GPU3D_Texcache.*` | 2 987 | Rasterisation, textures, profondeur, transparence, brouillard, contours, anticrénelage |
| Rendus OpenGL et compute | `GPU*_OpenGL*`, `GPU3D_Compute*`, `OpenGLSupport.*` | 8 624 | Facultatifs : rendu matériel et suréchantillonnage |
| Son (SPU) | `SPU.cpp`, `SPU.h` | 1 708 | 16 canaux, ADPCM, PSG, bruit, deux unités de capture, mixage |
| Cartouche DS | `NDSCart.cpp`, `NDSCart/*` | 4 118 | Protocole, chiffrement KEY1/KEY2, puces de sauvegarde, variantes (IR, NAND, Bluetooth, homebrew DLDI) |
| Slot GBA | `GBACart.cpp`, `GBACartMotionPak.cpp` | 1 627 | Cartouche GBA et accessoires (Rumble Pak, extension RAM, capteurs) |
| SPI | `SPI.cpp`, `SPI_Firmware.*` | 1 727 | Firmware, écran tactile, gestion de l'alimentation |
| Horloge | `RTC.cpp` | 1 063 | Horloge temps réel, alarmes |
| Wi-Fi | `Wifi.cpp`, `WifiAP.cpp`, `net/` | 3 267 + réseau | Puce Wi-Fi, point d'accès simulé, multijoueur local |
| Micro | `Mic.cpp` | 297 | Échantillons du micro, via le contrôleur tactile |
| États instantanés | `Savestate.cpp` | 531 | Sections par composant, version majeure et mineure |
| Codes de triche | `AREngine.*`, `ARCodeFile.*`, `ARDatabaseDAT.*` | 1 403 | Codes Action Replay DS |
| JIT | `ARMJIT*`, `dolphin/` | 10 855 | Recompilation x64 et ARM64, hors périmètre |
| DSi | `DSi*`, `DSP_HLE/`, `teakra/` | 15 593 | Console DSi et son DSP, hors périmètre |

melonDS embarque aussi des bibliothèques tierces : `blip-buf` (rééchantillonnage), `fatfs`, `sha1`, `tiny-AES-c`, `xxhash`, `teakra` et `libslirp`. RavenCore n'en prend aucune.

## Le modèle temporel

C'est la partie la plus structurante. Une erreur ici se corrige mal plus tard.

### Horloges et image

- **L'horloge système et l'ARM7** tournent à 33,513982 MHz. **L'ARM9** tourne au double, environ 67 MHz. melonDS compte le temps de l'ARM9 en demi-cycles système, avec un décalage d'un bit.
- **Une ligne dure 355 points de 6 cycles**, soit 2 130 cycles. L'H-Blank commence à 48 + 256 × 6 = 1 584 cycles du début de ligne.
- **Une image compte 263 lignes**, dont 192 visibles, soit 560 190 cycles et environ 59,83 images par seconde.

### Ordonnanceur

melonDS range ses événements dans une table fixe, avec un emplacement par source : ligne LCD, mixage du son, Wi-Fi, horloge, FIFO d'affichage, transferts de cartouche (ARM9 et ARM7), SPI, diviseur, racine carrée. Chaque événement porte une date en cycles système. Un masque de bits note ceux qui sont actifs, et la prochaine cible est la date la plus proche.

La boucle d'une image tourne ainsi :

1. **L'ARM9 avance le premier** jusqu'à la cible. S'il est arrêté, une DMA ARM9 tourne à sa place, ou la FIFO de géométrie pleine le bloque.
2. **Les timers de l'ARM9 et la géométrie 3D** rattrapent l'ARM9.
3. **L'ARM7 rattrape l'ARM9** jusqu'à la même date, ou une DMA ARM7 à sa place, puis ses timers suivent.
4. **Les événements échus s'exécutent.** Un composant qui programme un événement plus proche abaisse la cible en cours (`Reschedule`).
5. **L'image se termine** quand le compteur de lignes en signale une. En veille, seuls l'horloge et le son avancent.

Ce rattrapage par lots a une limite. Entre deux événements, un CPU peut voir un état que l'autre n'a pas encore produit : un mot d'IPC, de la WRAM partagée, un drapeau d'interruption. C'est une approximation connue du modèle. Le cœur GBA de RavenCore synchronise ses périphériques à l'intérieur même d'une instruction (`sync_within_instruction`). Un même degré de précision pour deux CPU coûte cher. Il faudra trancher par une mesure, sans recopier le compromis de melonDS (voir les [points ouverts](#points-ouverts-pour-le-propriétaire)).

### Arrêts des CPU

melonDS tient un mot de bits `CPUStop` :

- un bit par DMA en cours : une DMA arrête son CPU pendant son transfert ;
- un bit pour la géométrie 3D : la FIFO pleine bloque l'ARM9 jusqu'à ce qu'elle se vide ;
- des bits pour la veille et le réveil.

`Halt` est géré à part, par CPU, jusqu'à une interruption autorisée.

### Coût des accès mémoire

Les attentes dépendent de la région, de la largeur du bus et du type d'accès, séquentiel ou non. melonDS les précalcule dans des tables, par pages de 16 Kio pour l'ARM9 et de 32 Kio pour l'ARM7. Il les recalcule quand un registre change la carte (`EXMEMCNT`, `WRAMCNT`, contrôle des banques de VRAM).

L'ARM9 lit son code et ses données par deux ports. melonDS en estime le recouvrement par une formule, une approximation à mesurer, pas à reprendre. Le cache d'instructions de l'ARM9 (8 Kio) est émulé pour le timing ; le cache de données (4 Kio) ne l'est pas à cette révision.

## Carte mémoire

Ces adresses viennent de GBATEK ; melonDS les traite par un `switch` sur l'octet haut de l'adresse, une fonction par CPU et par largeur.

| ARM9 | Contenu | ARM7 | Contenu |
| --- | --- | --- | --- |
| `0x00000000` | ITCM, 32 Kio, taille virtuelle réglée par CP15 | `0x00000000` | BIOS ARM7, 16 Kio |
| Réglable | DTCM, 16 Kio, base réglée par CP15 | | |
| `0x02000000` | RAM principale, 4 Mio | `0x02000000` | RAM principale, partagée |
| `0x03000000` | WRAM partagée, 0, 16 ou 32 Kio selon `WRAMCNT` | `0x03000000` | La part de WRAM partagée laissée à l'ARM7 |
| | | `0x03800000` | WRAM de l'ARM7, 64 Kio |
| `0x04000000` | E/S de l'ARM9 | `0x04000000` | E/S de l'ARM7 ; Wi-Fi à `0x04800000` |
| `0x05000000` | Palettes des moteurs A et B, 2 × 1 Kio | | |
| `0x06000000` | VRAM vue par les moteurs 2D, la 3D et le mode LCDC | `0x06000000` | Banques C et D, quand elles sont données à l'ARM7 |
| `0x07000000` | OAM des moteurs A et B, 2 × 1 Kio | | |
| `0x08000000` | Slot GBA : ROM, puis RAM à `0x0A000000` | `0x08000000` | Slot GBA, s'il est donné à l'ARM7 (`EXMEMCNT`) |
| `0xFFFF0000` | BIOS ARM9, 4 Kio | | |

**Les neuf banques de VRAM** totalisent 656 Kio :

- A, B, C et D font 128 Kio ;
- E fait 64 Kio ;
- F et G font 16 Kio ;
- H fait 32 Kio ;
- I fait 16 Kio.

Chacune se branche, selon son registre de contrôle, sur :

- le mode LCDC ;
- les fonds ou les objets d'un moteur ;
- les palettes étendues ;
- les textures ou les palettes de textures de la 3D ;
- l'ARM7.

melonDS garde, pour chaque page de 16 Kio de chaque usage, un masque des banques branchées. Il le recalcule à chaque écriture d'un registre de contrôle. Quand deux banques se superposent, il combine leurs octets. Ce point est à vérifier sur GBATEK avant d'en faire une règle.

## Composants : rôle, pièges, ce que RavenCore a déjà

### CPU

- **ARM7 (ARMv4T, ARM7TDMI).** C'est le CPU du GBA, que RavenCore émule déjà (`cores/gba/src/cpu/`). Sur DS, il a un autre bus, d'autres attentes, et il n'a pas le préchargement de la cartouche GBA. Le jeu d'instructions, lui, ne change pas.
- **ARM9 (ARMv5TE, ARM946E-S).** Il ajoute des instructions :
  - `BLX`, et le changement d'état par `LDR`, `LDM` et `POP` vers PC ;
  - `CLZ` ;
  - l'arithmétique saturée : `QADD`, `QSUB`, `QDADD`, `QDSUB` ;
  - les multiplications 16 bits : `SMLAxy`, `SMULxy`, `SMLAWy`, `SMULWy`, `SMLALxy` ;
  - `LDRD`, `STRD`, `PLD` et `BKPT` ;
  - les accès à CP15 (`MCR`, `MRC`).

  Plusieurs règles diffèrent aussi de l'ARMv4 : la réécriture de la base d'un `LDM` ou d'un `STM` quand la base est dans la liste, et les drapeaux des multiplications. Il faut les vérifier dans le manuel d'architecture ARM et dans GBATEK.
- **CP15**, l'ARM9 seulement, porte quatre fonctions :
  - les régions ITCM et DTCM, base et taille ;
  - l'unité de protection, 8 régions de droits et de « cacheabilité » ;
  - la commande des caches ;
  - l'adresse des vecteurs d'exception (`0x00000000` ou `0xFFFF0000`).

  Le jeu règle tout cela au démarrage ; un démarrage direct doit le préparer.

### Interruptions, IPC, calculs

- **Interruptions :** `IME`, `IE` et `IF` existent pour chaque CPU. Les sources :
  - pour les deux CPU : V-Blank, H-Blank, V-Count, quatre timers, quatre DMA, clavier, slot GBA, IPC (synchro, FIFO d'envoi vide, FIFO de réception non vide), fin de transfert cartouche, IREQ de la cartouche ;
  - pour l'ARM9 seul : FIFO de géométrie ;
  - pour l'ARM7 seul : horloge, ouverture du couvercle, SPI, Wi-Fi.
- **IPC :** `IPCSYNC` porte 4 bits dans chaque sens, avec une interruption. Deux FIFO de 16 mots, une par sens, ont leurs drapeaux d'erreur.
- **Diviseur et racine carrée** de l'ARM9 : le résultat arrive après un délai, que melonDS programme comme un événement. Les jeux lisent parfois le drapeau « occupé » : ce délai fait partie du comportement.
- **Timers :** quatre par CPU, cadencés par l'horloge système, comme ceux du GBA. Le cœur GBA de RavenCore en a déjà la logique (`timers/timers.hpp`).

### DMA

Il y a quatre canaux par CPU. Ceux de l'ARM9 ont plus de modes de départ que le GBA :

- début de l'affichage ;
- affichage depuis la RAM principale ;
- cartouche DS ;
- slot GBA ;
- FIFO de géométrie.

L'ARM9 a aussi quatre registres de remplissage (`DMAxFILL`). melonDS modélise les rafales vers la RAM principale avec une table de coûts. Le contrôleur DMA du GBA de RavenCore (`dma/dma_controller.hpp`) est un point de départ, pas un composant réutilisable tel quel.

### Vidéo 2D

Les deux moteurs, A (écran principal) et B, reprennent le PPU du GBA et l'étendent :

- fonds en rotation étendue et fonds en bitmap ;
- palettes étendues de 256 couleurs ;
- objets en bitmap ;
- calque 3D en BG0, sur le moteur A ;
- capture d'écran vers la VRAM et affichage depuis la VRAM ou la FIFO de la RAM principale, sur le moteur A ;
- luminosité maîtresse, sur chaque moteur.

melonDS dessine ligne par ligne, et fait passer le rendu par une interface (`Renderer2D`) qui accepte aussi un rendu OpenGL. Le PPU GBA de RavenCore (`ppu/ppu.hpp`, 788 lignes) montre la manière de faire. Les deux moteurs DS en forment une extension nette ; leur code partagé se décidera au moment du découpage.

### Vidéo 3D

**Géométrie.** L'ARM9 écrit des commandes dans une FIFO de 256 entrées, suivie d'un tube de 4. Une FIFO pleine bloque l'ARM9.

- **Matrices et éclairage :** projection, position, vecteurs et textures, avec leurs piles. Quatre lumières, et les matériaux.
- **Préparation des primitives :** découpage par le volume de vue, puis rangement en RAM de sommets (6 144) et de polygones (2 048), en double tampon.
- **Tests :** `BOX_TEST`, `POS_TEST` et `VEC_TEST`.
- **Fin d'image :** `SWAP_BUFFERS` échange les tampons à la V-Blank suivante.

melonDS fait avancer la géométrie juste après l'ARM9, à chaque tour de boucle. Il compte le coût de chaque commande et les blocages du tube.

**Rendu.** Les polygones échangés à la V-Blank sont dessinés pendant l'image suivante. Le matériel rasterise ligne par ligne, avec :

- l'interpolation en perspective, sur un tampon Z ou W ;
- la transparence et l'alpha-test ;
- le brouillard, le marquage des contours, le toon et le surlignage ;
- l'anticrénelage, l'image ou la couleur de fond.

Ses limites, comme le nombre de polygones et le tampon de lignes, produisent des artefacts que les jeux montrent. Elles sont décrites dans GBATEK. Le rendu logiciel de melonDS (`GPU3D_Soft.cpp`) peut tourner sur un autre fil. Chez RavenCore, le rendu doit rester déterministe pour les états instantanés et RetroAchievements.

### Son

Le SPU a 16 canaux, en PCM 8 bits, PCM 16 bits ou ADPCM IMA. Les canaux 8 à 13 savent aussi faire des carrés (PSG), et 14 et 15 du bruit. Deux unités de capture recopient le mélange en mémoire, et les jeux s'en servent pour leurs effets.

melonDS mélange un échantillon toutes les 1 024 cycles, soit environ 32 728 Hz. Il rééchantillonne ensuite avec `blip-buf`, une bibliothèque tierce. RavenCore garde ses propres outils de rééchantillonnage.

### Cartouche DS

- **En-tête :** titre, code du jeu, adresses et tailles des binaires ARM9 et ARM7, sommes de contrôle. Les champs sont décrits dans GBATEK (« DS Cartridge Header »).
- **Protocole :** des commandes de 8 octets, d'abord en clair, puis chiffrées en KEY1, puis en KEY2. Le chiffrement KEY1 se fonde sur une table du BIOS de Nintendo. Sans le vrai BIOS, melonDS met cette table à zéro et démarre la cartouche directement. RavenCore fera de même : la zone sécurisée d'une ROM doit déjà être déchiffrée. Une ROM chiffrée sera refusée, avec un message clair.
- **Puce de sauvegarde** sur le SPI auxiliaire : commandes d'écriture, de lecture et d'état. Le classement de melonDS :
  - EEPROM de 512 octets, à 1 octet d'adresse ;
  - EEPROM de 8 à 128 Kio ;
  - Flash de 256 Kio à 1 Mio ;
  - NAND de 8 à 64 Mio, pour quelques jeux.

  melonDS choisit le type dans sa base de jeux. RavenCore ne la reprend pas (voir le [premier tableau](#règle-de-travail--une-carte-pas-une-source)). Il déduit le type des premières commandes du jeu et d'un fichier de sauvegarde existant, et l'hôte peut forcer le type, comme pour le GBA (`ravenemu_set_gba_save_type`).
- **Variantes :** cartouche à infrarouge (Pokémon HeartGold et SoulSilver), cartouche à NAND, cartouche Bluetooth, et homebrew avec un pilote DLDI (`melonDLDI`). Elles restent hors de la première version.

### SPI, horloge, Wi-Fi, micro

Tous ces composants sont côté ARM7.

- **SPI :** trois périphériques.
  - **La gestion de l'alimentation**, avec le rétroéclairage, le haut-parleur et l'extinction.
  - **Le firmware**, une Flash qui garde les réglages utilisateur et les points d'accès Wi-Fi. melonDS en génère un par défaut s'il n'a pas de dump. RavenCore générera le sien : pseudo, langue, date de naissance, couleur, calibration de l'écran tactile. Les sommes CRC16 suivent GBATEK.
  - **Le contrôleur tactile**, un convertisseur 12 bits. Il passe par la calibration du firmware, et donne aussi le micro.
- **Horloge :** une horloge Seiko, pilotée bit à bit par `0x04000138`, avec ses alarmes. Le cœur GBA a déjà une horloge de cartouche (`cartridge/rtc.hpp`), d'un autre modèle.
- **Wi-Fi :** c'est un gros composant, 2 482 lignes de puce, plus le réseau. Dans une première version, la puce répond à ses registres sans aucune connexion, assez pour que les jeux ne se bloquent pas.
- **Couvercle et touches :** X, Y, le crayon posé et le couvercle passent par `EXTKEYIN` (`0x04000136`), côté ARM7.

### Slot GBA

Sur DS, des accessoires s'y branchent : Rumble Pak, extension RAM, capteur solaire, Guitar Grip. Une cartouche GBA y sert aussi aux jeux DS qui la lisent, comme le transfert Pokémon.

Dans une première version, ce slot sera vide, et ses lectures donneront ce qu'un slot vide rend, d'après GBATEK.

### États instantanés

melonDS découpe ses états en sections, une par composant, chacune marquée de quatre lettres. Il les versionne en majeure et mineure (14.0 à cette révision). RavenCore garde ses propres règles : l'ordre des champs suit les fonctions `save` et `load`, et tout changement de format se note dans le journal.

## Démarrage

melonDS sait démarrer de deux façons :

- **Par le BIOS et le firmware**, avec de vrais dumps. RavenCore ne les fournira jamais.
- **Directement**, en reproduisant l'état que laissent le BIOS et le firmware :
  - l'en-tête de la cartouche et les identifiants de la puce recopiés en fin de RAM principale ;
  - les binaires ARM9 et ARM7 chargés à leurs adresses ;
  - CP15, les registres et les piles réglés ;
  - les réglages utilisateur du firmware recopiés en RAM.

  GBATEK décrit cet état. melonDS impose ce démarrage quand il n'a pas les dumps (`NeedsDirectBoot`).

RavenCore démarre directement, avec un **BIOS HLE** écrit pour lui : ses fonctions SWI s'exécutent en C++, comme le BIOS GBA de `cores/gba/src/bios/`. Voici les fonctions du BIOS DS et leurs numéros, d'après la table de GBATEK (« BIOS Function Summary ») :

- **Communes aux deux CPU :**
  - attente : `SoftReset` (00h), `WaitByLoop` (03h), `IntrWait` (04h), `VBlankIntrWait` (05h), `Halt` (06h) ;
  - calcul : `Div` (09h), `Sqrt` (0Dh), `GetCRC16` (0Eh), `IsDebugger` (0Fh) ;
  - copie : `CpuSet` (0Bh), `CpuFastSet` (0Ch) ;
  - décompression : `BitUnPack` (10h), `LZ77UnComp` (11h, et 12h par rappels), `HuffUnComp` par rappels (13h), `RLUnComp` (14h, et 15h par rappels).

  La décompression existe dans le BIOS GBA de RavenCore et pourra être partagée ; sur DS, plusieurs variantes lisent la source par des fonctions de rappel du jeu.
- **ARM9 seul :** `Diff8bitUnFilter` (16h), `Diff16bitUnFilter` (18h), `CustomPost` (1Fh).
- **ARM7 seul :** `Sleep` (07h), `SoundBias` (08h), `GetSineTable` (1Ah), `GetPitchTable` (1Bh), `GetVolumeTable` (1Ch), `GetBootProcs` (1Dh), `CustomHalt` (1Fh). Les trois tables lisent des données du BIOS de Nintendo : RavenCore devra les recalculer à partir de leur définition et vérifier qu'elles tombent juste, sans jamais les copier.
- **Gestion des interruptions :** le BIOS DS saute à l'adresse que le jeu a rangée à DTCM + 0x3FFC (ARM9, ARM ou Thumb) ou à 0x0380FFFC (ARM7, ARM seul), et `IntrWait` lit les drapeaux rangés 4 octets plus bas. Sur l'ARM9, `IntrWait` attend toujours au moins une IRQ, même quand le drapeau est déjà levé : un bogue du BIOS d'origine.

## Découpage proposé pour RavenCore

Le dossier `cores/nds/` suivrait les règles du dépôt : un dossier par composant matériel, un fichier par composant, aucun fichier de plus de 800 lignes ni fonction de plus de 80 lignes.

| Dossier | Contenu |
| --- | --- |
| `cpu/` | `arm9.hpp` (ARMv5TE), `cp15.hpp`, `tcm.hpp` ; l'ARM7 vient du CPU partagé (ci-dessous) |
| `machine/` | `scheduler.hpp`, `machine.hpp` (boucle d'image, rattrapage entre CPU), `direct_boot.hpp` |
| `memory/` | `bus9.hpp`, `bus7.hpp`, `memory_timing.hpp`, `shared_wram.hpp`, `vram_banks.hpp` |
| `interrupts/`, `ipc/`, `math/`, `timers/` | IRQ par CPU, `ipc_sync.hpp` et `ipc_fifo.hpp`, `divider.hpp` et `square_root.hpp`, timers |
| `dma/` | `dma_channel.hpp`, `dma_controller.hpp` |
| `gpu/` | `lcd_timing.hpp`, `display_capture.hpp`, `engine2d/` (fonds, objets, fenêtres, effets), `geometry/` (FIFO, matrices, éclairage, découpage), `raster/` (rasterisation, textures, effets d'image) |
| `spu/` | `sound_channel.hpp`, `adpcm.hpp`, `capture_unit.hpp`, `mixer.hpp` |
| `cartridge/` | `header.hpp`, `protocol.hpp`, `save/` (`eeprom.hpp`, `flash.hpp`, `save_detector.hpp`) |
| `spi/`, `rtc/`, `wifi/` | `firmware.hpp` (généré), `touchscreen.hpp`, `power_management.hpp` ; horloge ; puce Wi-Fi muette |
| `bios/` | `bios9.hpp`, `bios7.hpp` (HLE), en réutilisant la décompression partagée |

**Ce qui se partage avec le cœur GBA**, chacun par un refactor à part, sans changement de comportement ([AGENTS.md](../AGENTS.md)) :

1. **L'interpréteur ARM7TDMI**, déplacé dans `cores/common/`, avec le bus en paramètre. Les sorties du GBA (états instantanés, images, son, suites de test) restent identiques octet pour octet, et le nombre d'instructions exécutées est mesuré avant et après. L'ARM9 étend ensuite ce socle, sans modifier ce qu'exécute l'ARM7.
2. **La décompression et les copies du BIOS HLE**, communes au GBA et au DS.
3. **Les timers**, si leur forme le permet sans coût.

## Étapes

Chaque étape se termine par des tests automatisés et un résultat montrable.

**Avancement.** L'étape 1 est faite, avec une partie de l'étape 2 :
- le CPU ARM commun (`cores/common/arm/`), en ARMv4T et ARMv5TE ;
- dans `cores/nds/` : l'ordonnanceur à deux CPU, CP15 et les TCM, les deux bus et leurs temps d'accès d'après GBATEK ;
- les interruptions, l'IPC, le diviseur et la racine carrée, les timers, le balayage de l'écran et le clavier ;
- l'en-tête de cartouche et le démarrage direct ;
- les vecteurs et une partie des SWI du BIOS HLE ;
- la VRAM et sa projection, les deux moteurs 2D en mode texte et l'image des deux écrans ;
- les réglages utilisateur du firmware, générés.

La DMA et les décompressions du BIOS sans fonction de rappel sont faites aussi. Les objets (sprites) normaux et affines aussi. Les fonds affines, étendus et bitmap aussi. Les fenêtres et les effets de couleur aussi. Restent pour l'étape 2 les objets bitmap, les palettes étendues et les décompressions par fonctions de rappel.

1. **Fondations :**
   - le CPU ARM partagé ;
   - l'ARM9 et CP15 ;
   - l'ordonnanceur à deux CPU ;
   - des tests unitaires d'instructions, ARMv4T comme ARMv5TE.
2. **Premier homebrew :** démarrage direct d'un homebrew en mode texte 2D, écrit pour RavenCore avec une chaîne libre (BlocksDS, ou devkitARM et libnds). Il demande :
   - le bus, l'IPC, les IRQ, les timers et le clavier ;
   - un moteur 2D en mode texte ;
   - le firmware généré et le BIOS HLE minimal.
3. **Jeux 2D :**
   - le protocole de cartouche et les sauvegardes EEPROM et Flash, avec leur détection ;
   - la 2D complète, les DMA, le son, l'écran tactile et l'horloge.
4. **3D :** la géométrie, puis le rendu logiciel, puis la capture d'écran.
5. **Libretro (RavenNDS) :**
   - deux écrans dans une même image ;
   - le tactile par `RETRO_DEVICE_POINTER` ;
   - les états instantanés et la carte mémoire pour RetroAchievements.

   L'en-tête `libretro.h` de RavenCore n'a pas encore l'interface du micro. Il faudra le mettre à jour, puisque c'est un en-tête tiers.
6. **RavenEmu :** ses deux écrans, le tactile, les skins et les menus. C'est une décision du propriétaire, prise dans le dépôt RavenEmu, et RavenEmu termine d'abord sa parité.

**Hors de la première version :**
- le DSi et son DSP ;
- le JIT ;
- le Wi-Fi réel et le téléchargement DS ;
- les accessoires du slot GBA ;
- les variantes de cartouches ;
- les rendus OpenGL et le suréchantillonnage ;
- les codes Action Replay.

## Tests et références

- **ROMs de test écrites pour RavenCore**, une par comportement : instructions ARMv5TE, CP15, IPC, diviseur, DMA, banques de VRAM, 2D, géométrie, rendu, SPU. Chacune est vérifiée sur une vraie console quand c'est possible, comme les suites GB/GBC citées dans [cores/gbc/tests/README.md](../cores/gbc/tests/README.md).
- **Suites publiques :** la page « NDS Tests » de l'[Emulation General Wiki](https://emulation.gametechwiki.com/index.php/NDS_Tests) recense des ROMs de test DS, dont Rockwrestler, et les résultats de plusieurs émulateurs. Leurs licences et leur provenance se vérifient une par une avant usage. Les cartouches de test officielles de Nintendo, comme NTR Aging, n'entrent pas dans le dépôt.
- **Performance :** l'ARM9 à 67 MHz et l'ARM7 à 33 MHz font un ordre de grandeur de plus que le GBA à 16,8 MHz. RavenEmu vise des téléphones ARM64, et le JIT est hors périmètre. Il faut donc mesurer le coût de l'interpréteur dès la première étape, avec `valgrind --tool=lackey` comme pour les autres cœurs.

## Points ouverts pour le propriétaire

1. **Précision du rattrapage entre l'ARM9 et l'ARM7.** Le rattrapage par lots est rapide, mais un CPU peut voir l'IPC et la WRAM partagée en retard. La synchronisation à chaque accès partagé est plus juste et plus chère. À trancher sur mesure, une fois l'étape 2 faite.
2. **BIOS et firmware de l'utilisateur, en option.** Le cœur GBA accepte déjà un BIOS fourni par l'utilisateur (`GbaBios`). Le même choix sur DS permettrait le démarrage par le firmware et les ROMs à zone sécurisée chiffrée, sans rien embarquer.
3. **Détection du type de sauvegarde sans base de jeux.** Il faut accepter qu'un jeu rare demande un choix manuel.
4. **Branche `feat/nds-foundation`.** Elle n'a été ni lue ni modifiée pour ce document, sur instruction. Il reste à dire si ce travail doit s'y raccorder.
