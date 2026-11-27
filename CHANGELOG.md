# Journal des modifications

Ce fichier recense les évolutions notables de RavenCore. Son format suit [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/) et le projet applique le [versionnage sémantique](https://semver.org/lang/fr/). Avant la version 1.0, une version mineure peut changer l'API ou le format des états instantanés : chaque changement de ce type est signalé ici.

## Non publié

### Ajouté

- **CPU ARM commun** (`cores/common/arm/`), écrit pour RavenCore d'après GBATEK. Ce premier morceau du futur cœur NDS sert à ses deux processeurs :
  - l'ARMv4T (ARM7TDMI) et l'ARMv5TE (ARM946E-S), en ARM et en Thumb ;
  - les instructions de l'ARMv5TE : `BLX`, `CLZ`, l'arithmétique saturée, les multiplications 16 bits, `LDRD`, `STRD`, `PLD`, `BKPT` ;
  - les écarts entre les deux architectures : alignements de `LDRH` et `LDRSH`, réécriture de la base des `LDM` et `STM`, liste vide, changement d'état par `LDR`, `LDM` et `POP` vers PC ;
  - les exceptions, la ligne IRQ, les accès aux coprocesseurs et les appels au BIOS pris par l'hôte.

  Le bus compte le coût des accès, rechargement du pipeline compris après un saut (B : 2S + 1N). Les cycles internes suivent ceux de l'ARM7 ; ceux de l'ARM9 restent à mesurer. Le cœur GBA garde son propre CPU : son passage sur ce CPU commun sera un refactor à part, prouvé octet pour octet.
- Test : `ravenemu_arm_cpu`, des programmes ARM et Thumb assemblés par llvm-mc, exécutés sur les deux architectures.
- **Cœur NDS, fondations** (`cores/nds/`, cible `nds_raven_core`), écrit pour RavenCore d'après GBATEK. Il n'a encore ni API publique, ni cœur Libretro, ni image. Il comprend :
  - **la machine** : l'ARM9 et l'ARM7 avancent par tranches d'au plus 64 cycles système, jusqu'au prochain événement ;
  - **la mémoire** : RAM principale, WRAM partagée selon WRAMCNT, WRAM de l'ARM7, palettes, OAM et slot GBA vide ;
  - **les deux bus**, avec les temps d'accès de GBATEK pour le code et les données ;
  - **CP15** : identifiants, contrôle, ITCM et DTCM, mise en attente ;
  - **les périphériques** : interruptions des deux CPU, IPC (IPCSYNC et FIFO), diviseur et racine carrée, timers, balayage de l'écran (DISPSTAT, VCOUNT, IRQ), clavier, POSTFLG, HALTCNT ;
  - **le démarrage direct** : en-tête vérifié (CRC, zones de chargement), binaires chargés, RAM préparée comme par le BIOS et le firmware. Une ROM dont la zone sécurisée est chiffrée est refusée ;
  - **un BIOS HLE** : vecteurs d'exception écrits pour RavenCore, et les SWI `WaitByLoop`, `IntrWait`, `VBlankIntrWait`, `Halt`, `Sleep`, `CustomHalt`, `Div`, `Sqrt`, `CpuSet`, `CpuFastSet`, `GetCRC16`, `IsDebugger`. Aucun BIOS ni firmware de Nintendo n'est nécessaire ni inclus.

  Pas encore modélisés : la vidéo (VRAM, moteurs 2D et 3D), le son, la DMA, le protocole de cartouche et ses sauvegardes, le firmware, l'écran tactile, l'horloge, la Wi-Fi, les caches de l'ARM9 (comptés comme des succès quand CP15 les active).
- Tests : `nds_raven_hardware`, les composants un à un ; `nds_raven_machine`, une ROM écrite pour le test. Ses deux CPU s'échangent une valeur par IPC, divisent, l'un par le diviseur matériel, l'autre par `Div`, et l'ARM9 compte les images par interruption de V-Blank.
- **Cœur NDS, premier affichage** :
  - **VRAM** : les neuf banques (656 Kio), projetées par VRAMCNT_A à VRAMCNT_I selon la table de GBATEK, sur le LCDC, les fonds et objets des deux moteurs et l'ARM7. Deux banques superposées se lisent combinées et s'écrivent ensemble. Palettes, VRAM et OAM ignorent les écritures de 8 bits, sauf la VRAM donnée à l'ARM7 ;
  - **moteurs 2D A et B** : affichage éteint (blanc), normal et VRAM affichée telle quelle (mode 2). Les fonds en mode texte ont 16 ou 256 couleurs, retournements, défilement, priorités et couleur de fond ;
  - **luminosité maîtresse**, et image des deux écrans en ARGB8888 (`Machine::frame`). POWCNT1 bit 15 choisit l'écran du moteur A ;
  - **réglages utilisateur du firmware** générés par RavenCore et recopiés à 0x027FFC80 au démarrage : version, pseudo « RavenEmu », langue française, calibration du tactile.

  Pas encore : fonds affines, étendus et bitmap, objets, fenêtres, effets, palettes étendues, 3D, affichage depuis la RAM principale.
- Tests : VRAM et moteur 2D dans `nds_raven_hardware`. `nds_raven_machine` vérifie l'écran : l'ARM9 remplit la banque A de rouge et l'affiche, et l'image du bas sort rouge.
- **Cœur NDS, DMA et décompressions du BIOS** :
  - **DMA** : quatre canaux par CPU (GBATEK, « DS DMA Transfers »), unités de 16 ou 32 bits, source et destination croissantes, décroissantes ou fixes, destination rechargée, répétition, IRQ de fin. Départs immédiat, au V-Blank et, pour l'ARM9, au H-Blank des lignes visibles. Les registres DMA0FILL à DMA3FILL servent aux remplissages. La DMA ne voit pas les TCM, et son CPU attend la fin du transfert ;
  - **BIOS HLE** : `BitUnPack` (10h), LZ77 et RL vers des octets (11h, 14h), filtres différentiels de 8 et 16 bits de l'ARM9 (16h, 18h).

  Pas encore : les autres départs de DMA (début d'affichage, cartouche, FIFO de géométrie, Wi-Fi), les décompressions par fonctions de rappel (12h, 13h, 15h).
- Tests : décompressions sur une mémoire factice (`nds_raven_hardware`). `nds_raven_machine` couvre la DMA : copie immédiate, remplissage, départ au V-Blank avec IRQ, écriture sous la DTCM qui va en RAM principale, DMA de l'ARM7.
- **Cœur NDS, objets (sprites)** des deux moteurs 2D (GBATEK, « DS Video OBJs ») :
  - les 128 objets de l'OAM, normaux ou affines, taille doublée comprise ;
  - 16 ou 256 couleurs, tuiles en correspondance 1D (pas réglé par DISPCNT) ou 2D ;
  - retournements, objets masqués ;
  - priorités : un objet passe devant les fonds de priorité égale ou moindre, et l'objet de plus petit numéro l'emporte entre deux objets.

  Pas encore : objets bitmap, fenêtre d'objets, transparence, mosaïque, palettes étendues, limite de points par ligne.
- Test : objets dans `nds_raven_hardware`. Il couvre un objet de 8 × 8, un objet retourné, un affine, un masqué, la hauteur de l'objet et sa priorité face à un fond.
- **Cœur NDS, fonds affines et bitmap** (GBATEK, « BG Mode », « LCD I/O BG Rotation/Scaling ») :
  - **fonds affines** de BG2 et BG3 : carte de 8 bits, tuiles de 256 couleurs, de 128 à 1024 points, avec ou sans répétition ;
  - **fonds étendus** : carte de 16 bits avec retournements, bitmap de 256 couleurs, bitmap en couleurs directes (bit 15 = point visible) ;
  - **grand bitmap** du mode 6, 512 × 1024 ou 1024 × 512, moteur A seul ;
  - **points de référence internes** : rechargés à la ligne 0 et à chaque écriture de BGxX ou BGxY, puis avancés de PB et PD à chaque ligne.

  Pas encore : palettes étendues des fonds, mosaïque.
- Test : fonds affines et bitmap dans `nds_raven_hardware`. Il couvre l'identité, avec et sans répétition, la mise à l'échelle par PA, l'avancée par PD, les bitmaps de 256 couleurs et en couleurs directes, et le grand bitmap.
- **Cœur NDS, fenêtres et effets de couleur** (GBATEK, « LCD I/O Window Feature », « LCD I/O Color Special Effects ») :
  - **fenêtres** WIN0, WIN1 et fenêtre d'objets, faite des objets du mode 2 : chacune choisit les fonds, les objets et les effets permis, l'extérieur aussi. GBATEK note que seuls 8 bits de la ligne sont comparés, que X2 = 0 vaut 256, et que X1 = X2 = 0 n'affiche rien ;
  - **effets** : transparence entre les deux calques visibles du dessus (EVA, EVB), éclaircissement et assombrissement (EVY), selon les cibles de BLDCNT ;
  - **objets semi-transparents** (mode 1), toujours mélangés au calque dessous quand celui-ci est une seconde cible.

  La composition se fait désormais point par point, dans l'ordre des priorités.
- Test : fenêtres et effets dans `nds_raven_hardware`. Il couvre WIN0, l'assombrissement, la transparence d'un fond sur la couleur de fond, et un objet semi-transparent sur un fond.
- **Cœur NDS, palettes étendues, objets bitmap et mosaïque** (GBATEK, « DS Video Extended Palettes », « DS Video OBJs », « LCD I/O Mosaic Function ») :
  - **palettes étendues des fonds** (DISPCNT bit 30) : 4 emplacements de 8 Kio dans E, F, G (moteur A) ou H (moteur B). Elles servent aux tuiles de 256 couleurs à carte de 16 bits, en mode texte ou étendu. BG0 et BG1 peuvent prendre les emplacements 2 et 3 (BGxCNT bit 13) ;
  - **palettes étendues des objets** (DISPCNT bit 31) : 16 palettes de 256 couleurs dans F, G (moteur A) ou I (moteur B) ;
  - ces banques sortent de l'espace du CPU, comme le décrit GBATEK ;
  - **objets bitmap** (mode 3) en couleurs directes, dont le bit 15 rend le point visible. Ils se lisent en correspondance 2D (bitmap source de 128 ou 256 points de large) ou 1D (pas de 128 octets, ou de 256 octets sur le moteur A) ;
  - **mosaïque** des fonds et des objets : chaque bloc prend le point de son coin haut gauche, sur la grille de l'écran. Les lignes d'un bloc d'un fond affine gardent le point de référence de sa première ligne.

  La valeur alpha de l'OAM des objets bitmap n'est pas appliquée : GBATEK la nomme sans en décrire l'effet.
- Test : palettes étendues, objets bitmap et mosaïque dans `nds_raven_hardware`. Il couvre les emplacements 0 à 3, les banques E, F et G, la palette standard quand les palettes étendues sont coupées, les correspondances 2D et 1D des objets bitmap, et la mosaïque horizontale et verticale des fonds et des objets.
- **Cœur NDS, décompressions par fonctions de rappel** (GBATEK, « BIOS Decompression Functions », « NDS/DSi Decompression Callbacks ») :
  - LZ77 (SWI 12h) et RL (15h), écrits par demi-mots, et Huffman (13h), écrit par mots ;
  - c'est du code ARM écrit pour RavenCore et placé dans ses BIOS. Il ne demande que l'ARMv4T, donc sert aux deux CPU. L'hôte laisse ces trois SWI au vecteur, et le CPU les exécute en mode superviseur ;
  - les fonctions du jeu (Open, Close, Get_8bit, Get_32bit), en ARM ou en Thumb, reçoivent l'adresse source courante ;
  - la SWI rend la taille décompressée, ou le code d'erreur négatif de Open ou de Close. Huffman recopie son arbre dans le tampon fourni par le jeu ;
  - comme le décrit GBATEK, l'écriture par demi-mots empêche LZ77 de relire l'octet qui vient d'être produit (déplacement 0).
- Test : décompressions par fonctions de rappel dans `nds_raven_machine`. Il couvre les trois SWI sur l'ARM9, avec une fonction Thumb, un Close et un Open qui rendent une erreur, et LZ77 sur l'ARM7.
- **Cœur NDS, homebrew de démonstration** (`cores/nds/homebrew/`), qui clôt l'étape 2 :
  - écrit en C et en assembleur pour RavenCore, sans libnds, avec une police de 5 × 7 points dessinée pour lui ;
  - compilé pour l'ARM9 (ARM946E-S) et l'ARM7 (ARM7TDMI) par Clang et ld.lld, puis réuni en ROM `.nds` par `nds_make_rom`. Cet outil écrit l'en-tête de GBATEK, sans logo ni zone sécurisée, et le fait valider par le cœur ;
  - à l'écran, du texte sur les deux moteurs, avec les couleurs de RavenEmu ;
  - il compte les images par VBlankIntrWait et les secondes par l'IRQ d'un minuteur ;
  - il reçoit la réponse de l'ARM7 par la FIFO de l'IPC, et affiche le pseudo du firmware et les touches appuyées ;
  - option `RAVENEMU_NDS_HOMEBREW`, activée dans la CI Linux, qui installe Clang et ld.lld et publie la ROM comme artefact `raven-hello-nds`. La ROM n'est jamais versionnée.
- Test : `nds_raven_homebrew` démarre la ROM, relit le texte des deux écrans dans la VRAM, puis vérifie les compteurs, la réponse de l'ARM7, le pseudo, les touches A et START, et les couleurs de l'image. `RAVENEMU_NDS_SCREENSHOT=chemin.ppm` enregistre les deux écrans.
- **Cœur NDS, bus de la cartouche** (GBATEK, « DS Cartridge I/O Ports », « DS Cartridge Protocol »), début de l'étape 3 :
  - registres AUXSPICNT, ROMCTRL et commande de 8 octets, mots reçus à 0x04100010 ;
  - accès réservé au CPU qui a le slot (EXMEMCNT bit 11), l'autre lit 0 ;
  - mode de lecture principal laissé par le démarrage direct. B7 lit les données : le flux reste dans son bloc de 4 Kio, les adresses bouclent au-delà de la puce, et 0 à 0x7FFF lisent 0x8000 + (adresse & 0x1FF). B8 rend l'identifiant de la puce. Les autres commandes rendent des 0 ;
  - temps de transfert tirés de ROMCTRL : horloge de 5 ou 8 cycles, commande, gap1, 4 octets par mot, gap2 tous les 0x200 octets. Chaque mot prêt lève le DRQ et déclenche la DMA « cartouche » du CPU qui a le slot. En fin de transfert, l'IRQ 19 part si AUXSPICNT la demande ;
  - le chiffrement KEY2 n'est pas modélisé : les données arrivent en clair, comme le DS les rend une fois déchiffrées.
- Homebrew : il lit un message placé à 0x8000 dans sa ROM, par la commande B7, la DMA « cartouche » et IntrWait sur l'IRQ de fin. `nds_make_rom` accepte un fichier de données à 0x8000 et écrit les ROMCTRL par défaut des puces MROM.
- Tests : `game_card_test` dans `nds_raven_hardware` (temps, boucle de 4 Kio, redirection, miroir, B8, transfert sans données, gap2, DRQ, IRQ), `card_owner_test` dans `nds_raven_machine` (EXMEMCNT bit 11), et le message lu par le homebrew dans `nds_raven_homebrew`.
- **Cœur NDS, sauvegardes de la cartouche** (GBATEK, « DS Cartridge Backup », « DS Firmware Serial Flash Memory ») :
  - puce sur le SPI de la cartouche : en mode SPI, chaque écriture d'AUXSPIDATA échange un octet ; AUXSPICNT bit 6 garde la puce sélectionnée ; le bus reste occupé (bit 7) 64 << débit cycles ;
  - EEPROM de 0,5 Kio (adresse d'un octet, 0Ah/0Bh pour 100h-1FFh), de 8 et 64 Kio (2 octets), de 128 Kio (3 octets) ; FRAM de 8 et 32 Kio ; Flash de 256 Kio, 512 Kio, 1 Mio et 8 Mio, avec leur identifiant JEDEC ;
  - commandes WREN, WRDI, RDSR, WRSR (protection WP et SRWD), RDID, lecture, écriture bouclée dans la page, et pour la Flash FAST, PW, PP, PE, SE, DP et RDP. Écritures sans délai, WEL remis à 0 en fin de commande ;
  - type choisi par l'hôte (`SaveType`) ou déduit de la taille d'un fichier de sauvegarde ; `dirty()` signale à l'hôte une écriture à enregistrer. La détection par les commandes du jeu reste à écrire.
- Homebrew : il compte ses démarrages dans une EEPROM de 8 Kio.
- Tests : `save_memory_test` et `card_spi_test` dans `nds_raven_hardware` ; dans `nds_raven_homebrew`, un premier démarrage compté, puis un second démarrage depuis le fichier de sauvegarde du premier.
- **Cœur NDS, détection du type de la puce de sauvegarde** (GBATEK, « DS Cartridge Backup », « Detection (in emulators) ») :
  - une cartouche neuve a une puce de type `automatic`. Le SDK envoie la commande et l'adresse depuis une fonction, les données depuis une autre : les octets écrits par l'instruction de la commande donnent la largeur d'adresse, dès le premier octet de données ;
  - 1 octet : EEPROM de 0,5 Kio ; 2 octets : EEPROM de 64 Kio (qui garde aussi les données des puces de 8 Kio et des FRAM) ; 3 octets : EEPROM de 128 Kio sur WR (02h), Flash de 1 Mio sur PW, PE, SE ou une lecture. Une Flash choisie par une lecture devient l'EEPROM de 128 Kio si la première écriture est WR ;
  - la commande en cours est rejouée sur la puce choisie ; un effacement se décide en fin de commande ; un fichier de sauvegarde garde la priorité (type d'après sa taille) ;
  - les bus donnent à la cartouche l'adresse de l'instruction qui écrit AUXSPIDATA (`InstructionHook`, `Cpu::instruction_address`).
- Homebrew : commande et adresse de l'EEPROM partent d'une fonction, les données d'une autre ; son type est reconnu seul. Ces deux fonctions sont en assembleur (`card_spi.s`), chacune avec une seule écriture d'AUXSPIDATA : en C, le Clang de la CI répartissait autrement ces écritures, et le test échouait sur `main`.
- Tests : `save_detection_test` dans `nds_raven_hardware` (largeurs de 1, 2 et 3 octets, instruction unique, bascule vers l'EEPROM de 128 Kio, Flash confirmée, effacement, fichier) ; `nds_raven_homebrew` démarre sans type imposé.
- **Cœur NDS, SPI de l'ARM7** (GBATEK, « DS Serial Peripheral Interface Bus (SPI) », « DS Power Management Device », « DS Touch Screen Controller (TSC) », « DS Firmware Header », « DS Firmware User Settings ») :
  - SPICNT et SPIDATA : choix de l'appareil, maintien du chip select (bit 11), occupation de 64 << débit cycles, IRQ 23 en fin de transfert (bit 14) ;
  - gestion de l'alimentation : registres 0 à 3, répétés jusqu'à 7Fh ; son et rétroéclairages allumés au démarrage ;
  - Flash du firmware (puce de 256 Kio du modèle de sauvegarde) au contenu généré par RavenCore : en-tête, calibrage Wi-Fi minimal avec une adresse MAC locale, réglages utilisateur en deux exemplaires avec compteur et CRC16 ;
  - écran tactile (TSC2046) : voies X, Y, Z1, Z2, températures, batterie et micro, sur 12 ou 8 bits, avec commande pendant la réponse ; le point touché suit le calibrage des réglages ; `Machine::set_touch` le règle, et EXTKEYIN bit 6 suit le crayon.
- **Cœur NDS, tables du son du BIOS de l'ARM7** (SWI 1Ah, 1Bh, 1Ch) : calculées par RavenCore d'après la description de GBATEK (sinus, hauteur, volume en pas de 0,1 dB), aux bornes de GBATEK ; la table des volumes reste une estimation à vérifier avec le son.
- Homebrew : l'ARM7 lit le point touché par le SPI, l'ARM9 le convertit en pixels avec le calibrage des réglages et le diviseur matériel.
- Tests : `spi_devices_test` dans `nds_raven_hardware` (alimentation, tactile, image du firmware, bus SPI, tables du son) ; dans `nds_raven_homebrew`, crayon levé, point (100, 50) touché, puis relevé.

### Documentation

- **Carte de melonDS pour préparer le cœur NDS** ([docs/nds_melonds_map.md](docs/nds_melonds_map.md)). Elle décrit ses composants, son modèle temporel, sa carte mémoire et son démarrage. Elle propose aussi un découpage de `cores/nds/` et des étapes. C'est une lecture d'architecture, sans aucun code ni aucune donnée repris : GBATEK reste la référence matérielle. Les moteurs, l'API, Libretro et le format des états ne changent pas.

## 0.17.4 - 2026-11-21

Cette version corrige les palettes d'objets laissées par la boot ROM du DMG et documente ce qui bloque la suite de GBMicrotest : 355 de ses 513 tests passent, contre 353. L'API, Libretro, le GBA, la GBC et le format des états ne changent pas.

### Corrigé

- **OBP0 et OBP1 au démarrage sans boot ROM (DMG).** La boot ROM ne les écrit pas : ils lisent $FF, pas $00 (GBMicrotest, `poweron_obp0`, `poweron_obp1` ; Pan Docs les dit indéterminés). Un jeu qui affiche des objets sans régler ses palettes les dessine désormais comme sur la console mesurée.
- Test : palettes d'objets au démarrage ; il échoue sans le correctif.

### Reste à faire

- **GBMicrotest et Mooneye se contredisent sur LY.** Vus du CPU, GBMicrotest voit les bits de mode de STAT et son drapeau LY=LYC changer quatre points après LY sur toutes les lignes (`poweron_stat`, ligne 153 avec LYC=0 et LYC=153, `win*_b`, `ppu_sprite0_scx*_b`) ; Mooneye le voit sur les lignes d'allumage (`lcdon_timing-GS`), que RavenCore reproduit. Mais Mooneye fixe aussi, par ses interruptions, la chaîne mode 2 → mode 0 → LY (`intr_2_0_timing`, `hblank_ly_scx_timing-GS`, `intr_2_mode0_timing`, `intr_2_mode3_timing`, avec AGE `ly`) : STAT y montre le mode 0 252 points après LY, et GBMicrotest, qui lit LY et STAT au registre, 256. Les deux suites, mesurées sur DMG, diffèrent donc de quatre points sur l'instant où LY change par rapport au reste de la ligne, et l'instant où le CPU lit le bus s'annule dans cette mesure : aucune phase ni aucun retard ne satisfait les deux. Essais, tous écartés : retarder STAT de quatre points sur toutes les lignes et lever l'interruption mode 2 au début de la ligne (GBMicrotest 366, mais `intr_2_0_timing`, `intr_2_oam_ok_timing` et AGE `m3-bg-lcdc` échouent) ; retarder en plus l'interruption H-Blank de un à quatre points (jusqu'à 372, mais `hblank_ly_scx_timing-GS` échoue) ; lire le bus plus tôt dans le M-cycle (69 à 91 ROMs de la CI en échec). RavenCore suit Mooneye et AGE, vérifiés sur plusieurs consoles.

## 0.17.3 - 2026-11-21

Cette version corrige trois comportements de la Game Boy (DMG) que mesure GBMicrotest, une suite vérifiée sur console : 353 de ses 513 tests passent, contre 304. L'API, Libretro, le GBA et le format des états ne changent pas ; la GBC ne change que pour l'interruption H-Blank de la ligne d'allumage.

### Corrigé

- **Fausse interruption à l'écriture de STAT.** Sur DMG, écrire STAT lève une interruption en H-Blank, en V-Blank ou si LY=LYC, d'après l'état du PPU au début du M-cycle d'écriture. RavenCore la levait aussi en mode 2, et d'après la fin du M-cycle : un jeu qui configurait STAT pendant le scan OAM recevait une interruption de trop dès EI (GBMicrotest `stat_write_glitch`, `hblank_int_scx`).
- **Interruption H-Blank après l'allumage de l'écran.** La première ligne après l'allumage commence par un mode 0 partiel : STAT y lit bien le mode 0, mais l'interruption H-Blank n'arrive qu'à la fin du mode 3 de cette ligne. RavenCore la levait dès l'allumage (`int_hblank_nops`, `int_hblank_halt`). La GBC suit la même règle, faute de mesure contraire ; SameSuite et AGE n'en changent pas.
- **Position du PPU au démarrage sans boot ROM (DMG).** À l'entrée en $0100, la boot ROM laisse le PPU en V-Blank, à 60 points de la fin de la ligne 153 ; RavenCore le plaçait au début de la ligne 0. LY, STAT, l'OAM et la VRAM lus dans les premières lignes d'un jeu donnent désormais la valeur de la console au M-cycle près (`poweron_ly`, `poweron_stat`, `poweron_oam`, `poweron_vram`). La GBC garde sa position, faute de mesure.
- Tests : phase de la fausse interruption STAT, interruption H-Blank de la ligne d'allumage et position du PPU au démarrage ; chacun échoue sans son correctif. Les tests du PPU, 1 686 lignes, sont découpés par composant (`ppu_fetcher`, `ppu_object`, `ppu_line_timing`, `ppu_video_bus`, `ppu_state`, `post_boot`), selon la règle d'AGENTS.md.
- Performances : inchangées.

### Reste à faire

- **Interruption H-Blank, à deux points près.** Sur la console, elle arrive deux points plus tard que dans RavenCore : `hblank_int_scx`, `int_hblank_halt` et `int_hblank_incs` l'indiquent, comme les fins de mode 3 de `poweron_stat`. La retarder de deux points casse Mooneye (`intr_2_0_timing`, `hblank_ly_scx_timing-GS`) : l'écart vient d'ailleurs, sans doute de l'instant où le CPU échantillonne les interruptions dans un M-cycle. Avec la position mesurée du PPU au démarrage, quatre tests qui passaient échouent : `hblank_int_scx1_nops_a`, `scx2`, `scx5` et `scx6`. Ils encadrent l'interruption juste après le démarrage, avec leurs pendants `_nops_b`, qui passent désormais pour SCX 3, 4 et 7.
- **GBMicrotest.** Restent aussi la ligne 153 et LYC, les interruptions de V-Blank et de mode 2 lues dans IF, l'allumage de l'écran et la window (`win*_b`).

## 0.17.2 - 2026-11-21

Cette version corrige l'instant où le fetcher du fond de la Game Boy et de la Game Boy Color lit ses registres et la VRAM pendant le mode 3. Sur l'ensemble des tests Mealybug Tearoom et des tests AGE du mode 3, qui changent ces registres en cours de ligne, les pixels faux passent de 127 242 à 80 543 (−37 %) ; AGE `m3-bg-lcdc` passe sur Game Boy et rejoint la CI (261 ROMs). L'API, Libretro, le GBA et le format des états ne changent pas.

### Corrigé

- **Image : avance du fetcher.** Le fetcher ne pousse une tuile que dans une FIFO du fond vide (Pan Docs, *Pixel FIFO*). RavenCore la poussait dès qu'il n'y restait que huit pixels : le fetcher prenait une tuile d'avance de trop et lisait index, données et registres huit points trop tôt. La durée du mode 3 ne change pas. Mealybug Tearoom `m3_lcdc_bg_map_change` (de 1 984 à 700 pixels faux sur Game Boy, de 2 112 à 316 sur GBC), `m3_lcdc_tile_sel_change` (de 2 172 à 1 536, de 2 108 à 1 460), `m3_scx_high_5_bits` (de 342 à 84, de 277 à 19) et leurs variantes se rapprochent de l'attendu ; AGE `m3-bg-lcdc` passe sur Game Boy (sur GBC, de 5 120 à 256).
- **Image : SCY en cours de tuile.** Sur Game Boy, la ligne de la tuile suit SCY jusqu'à chacune des deux lectures de données ; sur GBC, elle reste celle de la lecture de l'index et des attributs. Une écriture de SCY est validée dès le premier point de son M-cycle, comme celles des palettes. `m3_scy_change` : de 9 698 à 1 431 pixels faux sur Game Boy, de 10 395 à 2 170 sur GBC ; `m3_scy_change2` (GBC) : de 535 à 204.
- **Image : carte de la window.** Le bit 6 de LCDC (carte de la window) est lui aussi validé dès le premier point du M-cycle, comme les bits 0 et 1 : `m3_lcdc_win_map_change` passe de 1 146 à 754 pixels faux sur Game Boy et de 1 084 à 700 sur GBC.
- Tests : avance d'une seule tuile du fetcher, ligne de tuile et SCY sur Game Boy et sur GBC, instant de validation de SCY et du bit 6 de LCDC ; chacun échoue sans son correctif.
- Performances : inchangées (−0,1 % d'instructions sur `cgb-acid2`).

### Modifié

- **Cœur GB/GBC : un fichier par composant.** Le PPU (1 553 lignes en un fichier), l'APU et le bus mémoire sont découpés par composant : fetcher du fond, fetcher des objets, rendu, RAM couleur, séquencement des lignes, bus vidéo, états instantanés ; canaux carrés, d'onde et de bruit ; DMA OAM et HDMA, devenus des classes. Le comportement ne change pas : les états instantanés internes, pris à des instants arbitraires, les images et le son de 54 exécutions, ainsi que les 688 mesures des suites publiques, sont identiques octet pour octet avant et après. Les performances aussi (±0,005 % d'instructions). La carte des fichiers est dans le [guide de contribution](CONTRIBUTING.md).

### Reste à faire

- **Mealybug Tearoom et AGE.** Pour les bits 3 et 4 de LCDC (carte et données du fond) et pour SCX, l'instant d'écriture qui rapproche Mealybug Tearoom de l'attendu éloigne AGE `m3-bg-lcdc` et `m3-bg-scx`, et inversement : RavenCore garde le sien en attendant un modèle qui satisfasse les deux. Les changements de WX en cours de ligne (`m3_wx_4_change`, `m3_wx_5_change`) comptent environ 200 pixels faux de plus, et AGE `m3-bg-scx` sur GBC 56 de plus : ils sont à reprendre avec le démarrage de la window. Restent aussi les objets au bord gauche de l'écran et, sur Game Boy, la ligne 0.

## 0.17.1 - 2026-11-21

Cette version poursuit le rapprochement du son de la Game Boy et de la Game Boy Color avec celui de la console : SameSuite passe de 47 à 51 tests sur 76. L'API, Libretro, le GBA et le format des états ne changent pas ; les 4 tests rejoignent la CI (260 ROMs).

### Corrigé

- **Son : départ du canal de bruit.** Après son déclenchement, le premier pas du LFSR arrive une période et un M-cycle plus tard ; deux M-cycles si le canal jouait encore. RavenCore le faisait arriver dès la fin de la première période : toute la suite pseudo-aléatoire avait un pas d'avance (SameSuite, `channel_4_lfsr_restart`, `channel_4_lfsr_restart_fast`, `channel_4_lfsr_7_15` ; `channel_4_lfsr`, `lfsr15` et `lfsr_15_7` passent toujours).
- **Son : canal 3 arrêté par son DAC.** Couper le DAC (NR30) vide le tampon d'échantillon : redémarré, le canal sort 0 jusqu'à son premier échantillon, au lieu de rejouer le dernier (`channel_3_restart_stop_delay`).
- Tests : délais de départ et de redéclenchement du bruit, et tampon du canal 3 vidé par son DAC ; chacun échoue sans son correctif. Le test qui compte quinze pas du LFSR tient compte du délai de départ.

### Reste à faire

- **SameSuite.** Le « zombie mode » (écritures NRx2 pendant qu'un canal joue) suit sur CGB-E une règle que ni Pan Docs ni les seize mesures de `channel_1_nrx2_glitch` ne permettent d'établir sûrement ; RavenCore garde son seul cas commun. Restent aussi l'alignement 1 MHz en double vitesse, le balayage du canal 1 et les délais du bruit selon sa fréquence (`channel_4_delay`, `channel_4_freq_change`, `channel_4_frequency_alignment`).

## 0.17.0 - 2026-11-21

Cette version rapproche le son et l'image de la Game Boy et de la Game Boy Color de ceux de la console, d'après deux suites de test publiques plus exigeantes que celles de la CI : SameSuite (47 tests sur 76 passent, contre 22) et Mealybug Tearoom (2 tests CGB sur 27, contre 0). L'API, Libretro et le GBA ne changent pas. **Le format des états GB passe à 12** : il garde le rapport cyclique de l'échantillon en cours des canaux carrés et un front du séquenceur audio à sauter ; les états aux formats 10 et 11 restent lisibles, un état au format 12 ne se recharge pas dans une version antérieure. C'est la raison de la version mineure : un projet qui consomme le paquet CMake demande `find_package(RavenCore 0.17)`. Les 56 tests de ces suites et des AGE test roms qui passent rejoignent la CI.

### Corrigé

- **Son : départ des canaux carrés.** Après son déclenchement, un canal carré attend une période et deux tops de 1 MHz (8 cycles) avant son premier échantillon ; un seul top s'il jouait encore, sans repartir du début de son cycle de service (SameSuite, `channel_1_delay`, `channel_1_restart`). RavenCore le faisait partir 8 cycles trop tôt : les registres PCM12 de la GBC, que lisent ces tests, l'exposaient, et chaque note des canaux 1 et 2 commençait 2 µs en avance. 15 tests SameSuite passent désormais.
- **Son : canal carré arrêté.** Arrêté (longueur écoulée, DAC coupé, balayage hors limites), un canal carré fige son minuteur et sa position dans le cycle ; seule l'extinction de l'APU (NR52) l'efface. Redéclenché, il repart de là avec le délai complet d'un départ. RavenCore laissait tourner le minuteur (`channel_1_stop_restart`, `channel_2_stop_restart`).
- **Son : rapport cyclique.** Un nouveau rapport cyclique (NRx1) n'agit qu'à l'échantillon suivant ; RavenCore le prenait aussitôt (`channel_1_duty_delay`, `channel_2_duty_delay`).
- **HDMA de la GBC.** Lancé pendant un H-Blank, ou écran éteint, un HBlank DMA copie aussitôt un bloc, puis attend le H-Blank suivant ; écran éteint, il n'en vient pas et le transfert en reste là. Arrêté par l'écriture de $00, FF55 relit $80. RavenCore suivait Pan Docs : écran éteint, le HBlank DMA devenait un transfert général, et l'arrêt relisait les blocs restants. Sur décision du propriétaire, il suit désormais SameSuite (`hdma_mode0`, `hdma_lcd_off`, `gdma_addr_mask`) ; un état gardant un HBlank DMA actif écran éteint, désormais possible, est accepté. Éteindre l'écran pendant un HBlank DMA le termine toujours comme un transfert général, faute de mesure contraire.
- **Son : mise sous tension pendant le front du séquenceur.** Allumé (NR52) pendant que le bit de DIV qui cadence le séquenceur vaut 1, l'APU saute le front qui suit, et ce front ne compte pas pour la longueur : un déclenchement fait alors le cadencement de longueur en plus (`div_write_trigger_10`, `div_trigger_volume_10`, `div_write_trigger_volume_10`).
- **Image : écritures de palette en mode 3.** Un pixel sorti de la FIFO n'est coloré que trois points plus tard : une écriture dans BGP, OBP0, OBP1, la RAM couleur (BCPD, OCPD) ou les bits 0 et 1 de LCDC atteint déjà les pixels sortis pendant les trois points qui la précèdent. RavenCore la faisait voir trois pixels trop tard. Le bus valide désormais ces écritures dès le premier point de leur M-cycle ; les autres bits de LCDC, lus par le fetcher ou qui allument l'écran, gardent leur instant, et la durée du mode 3 ne change pas. Mealybug Tearoom `m3_bgp_change` et `m3_window_timing` (CGB-D) et AGE `m3-bg-bgp` passent. En double vitesse, faute de mesure, l'écriture est validée au premier des deux points du M-cycle.
- **Image : ligne 0 sur GBC.** Sur GBC, l'interruption STAT du mode 2 de la ligne 0 arrive au point 0, comme le compensent les tests Mealybug Tearoom, et non au point 4 comme sur Game Boy (Mooneye `intr_1_2_timing-GS`, vérifié sur DMG et qui échoue sur CGB). La ligne 0 de chaque test Mealybug était décalée de quatre pixels.
- **VRAM au démarrage sans boot ROM.** La boot ROM laisse en VRAM le logo de l'en-tête de la cartouche (tuiles 1 à 24) et le glyphe « ® » (tuile 25), et sur Game Boy la carte BG qui les montre à l'écran ; un jeu qui n'efface pas la VRAM les retrouve. RavenCore la laissait vide. Il y recopie désormais le logo de la cartouche et ces 8 octets de la boot ROM, sur Game Boy et en mode de compatibilité de la GBC, comme le montrent les captures de Mealybug Tearoom ; rien ne change en GBC native, ni avec une vraie boot ROM. Neuf tests Mealybug, qui affichent le « ® » en sprite, se rapprochent de l'attendu (`m3_obp0_change` : de 432 à 112 pixels faux) sans encore passer : leurs autres écarts restent à corriger.
- Tests : redéclenchement d'un canal carré en marche et après un arrêt, rapport cyclique changé au milieu d'un échantillon, front du séquenceur sauté à la mise sous tension, instant de l'interruption mode 2 de la ligne 0 sur Game Boy et sur GBC, pixels qu'atteint une écriture de palette en mode 3, et VRAM laissée sans boot ROM ; chacun échoue sans son correctif. Le test des anciens formats d'état recharge désormais les formats 10 et 11, APU en disposition 1. La ROM synthétique du harnais de conformité éteint le fond, pour garder l'image uniforme que ses tests attendent malgré le logo. Trois tests existants attendaient l'ancien départ des canaux carrés. La CI exécute désormais 256 ROMs de test publiques au lieu de 200 : 47 SameSuite, 3 Mealybug Tearoom et 6 AGE s'ajoutent. Deux tests unitaires du HBlank DMA, qui figeaient le comportement de Pan Docs, suivent désormais SameSuite.
- Performances : le coût est nul en Game Boy et d'environ 1 % d'instructions sur `cgb-acid-hell`, qui écrit la RAM couleur à chaque ligne.

### Reste à faire

- **SameSuite.** Restent l'alignement des canaux sur l'horloge à 1 MHz en double vitesse, les écritures NRx2 pendant qu'un canal joue (« zombie mode »), le balayage du canal 1 et le bruit (canal 4).
- **Mealybug Tearoom.** Restent les écritures de LCDC, SCX, SCY et WX lues par le fetcher, les objets au bord gauche de l'écran (qui décalent la suite de la ligne), et, sur Game Boy, la ligne 0, que contredit Mooneye.

## 0.16.0 - 2026-11-20

La Game Boy Color et la Game Boy ne forment plus qu'**un seul cœur**, **RavenGBC** : comme la console, il lit aussi les jeux Game Boy. Le moteur était déjà commun ; le cœur Libretro RavenGB et l'ancienne façade disparaissent, et le moteur prend le nom de la Game Boy Color. Ce qu'un jeu voit ne change pas, ni le format des états ; l'API C ne change pas. Le cœur Libretro RavenGB, ses clés d'options et l'en-tête C++ `ravenemu/gb/` sont retirés : c'est la raison de la version mineure.

### Modifié

- **Libretro : RavenGBC pour la Game Boy Color et la Game Boy.** `ravenemu_gbc_libretro` ouvre les `.gbc`, `.gb` et `.sgb` ; `ravenemu_gb_libretro` n'existe plus. L'option **Console émulée** (`ravenemu_gbc_hardware`) garde ses valeurs `gbc` (par défaut, comme avant : un jeu Game Boy prend des couleurs) et `gba`, et en gagne trois : `auto` (un jeu Game Boy sur la Game Boy, un jeu Game Boy Color, 0x80 ou 0xC0 en 0x143, sur la GBC), `auto_gba` (de même, la GBC sur une GBA) et `gb` (la Game Boy pour tous, qui refuse un jeu réservé à la GBC, comme RavenGB). La boot ROM lue est celle de la console émulée.
  - Les réglages de RavenGBC enregistrés par RetroArch restent valables. Ceux de RavenGB passent sur RavenGBC : `ravenemu_gbc_palette` (teintes des jeux sur la Game Boy), et les options communes sous `ravenemu_gbc_*` ; les réglages enregistrés sous `ravenemu_gb_*` ne sont pas repris. Pour retrouver RavenGB, choisir **Game Boy** ou **Selon le jeu**.
  - Les sauvegardes de cartouche et le format des états ne changent pas. Comme avant, le moteur refuse un état pris sur une autre console (Game Boy ou Game Boy Color) que celle émulée.
- **Moteur.** `cores/gb` devient `cores/gbc`, et l'ancienne façade `cores/gbc` y est repliée : KEY1 et double vitesse (`src/speed`), port infrarouge (`src/infrared`), tests du matériel CGB (`gbc_color_hardware`). La cible `gb_raven_core` devient `gbc_raven_core`, les en-têtes `ravenemu/gb/` deviennent `ravenemu/gbc/` et le namespace `ravenemu::gb` devient `ravenemu::gbc` ; `gbc_hardware` est retirée. `ravenemu::gbc::make_core(ravenemu::gbc::HardwareModel::cgb)` crée la Game Boy Color, `dmg` la Game Boy, `agb` la GBC d'une GBA, et `automatic` (par défaut) choisit d'après l'en-tête : l'ancien `gbc::make_core()` sans argument, toujours en GBC, demande désormais `HardwareModel::cgb`. Les tests du moteur prennent le préfixe `gbc_`. `RavenCore::ravencore` regroupe `gbc_raven_core` et `gba_raven_core`.
- **API C et façade C++.** Inchangées : `RAVENEMU_GB`, `RAVENEMU_GBC`, `RAVENEMU_GBC_ON_GBA` et `sdk::System` choisissent la console du même moteur. Version mineure : un projet qui consomme le paquet CMake demande `find_package(RavenCore 0.16)`.
- Tests : le test de fumée Libretro joue trois scénarios sur deux cœurs (RavenGBC réglé en Game Boy, RavenGBC tel quel en Game Boy Color, RavenGBA). Nouveau cas : la console choisie pour chaque jeu et chaque valeur de **Console émulée**, jeu réservé à la GBC refusé par la Game Boy ; il échoue si RavenGBC ne suit plus l'option.

## 0.15.4 - 2026-11-14

Cette version apporte des corrections issues d'une relecture du Game Boy, du GBA, de Libretro et de l'API C. Aucune ne change ce qu'un jeu voit : images, son et format des états restent les mêmes. L'API C ne change pas, seul son en-tête est précisé. Elles ferment des écritures hors limites ou des débordements que seuls un état modifié, une ROM hostile ou un partenaire réseau anormal pouvaient atteindre.

### Corrigé

- **Game Boy, état modifié.** Un état gardant le PPU en mode 3 au bout de la ligne (`transfer_x` à 160) était accepté ; le pixel suivant s'écrivait hors de la ligne, et hors de l'image à la ligne 143. Le mode 3 passe en H-Blank dès son 160e pixel : un tel état est désormais refusé.
- **GBA, Huffman sans fin (SWI 0x13).** Un arbre en chaîne lu dans une ROM lente pouvait compter plus de deux milliards de cycles dans un seul appel : débordement du compteur d'attente du bus, plusieurs secondes de blocage et un état pris ensuite impossible à recharger. L'appel s'arrête désormais à 2^27 cycles (huit secondes de console), signalé aux diagnostics et sans rien écrire ; aucun jeu n'en approche.
- **GBA, ObjAffineSet (SWI 0x0F).** Les multiples de l'écart r3 étaient calculés en entier signé : débordement pour un écart de 2^30 ou plus. Ils le sont désormais modulo 2^32, comme les adresses.
- **GBA, câble distant.** La durée d'un transfert reçue du partenaire n'était pas bornée : une valeur hors de toute durée réelle débordait au départ du transfert et rendait l'état pris en attendant impossible à recharger. Elle est bornée comme dans un état.
- **GBA, état DMA.** Un état donnant à une phase de DMA jusqu'à 2^31 cycles figeait le CPU deux minutes ; une phase dure au plus un accès (36 cycles), et l'état est refusé au-delà de 64.
- **GBA, ROM refusée.** Le nouveau jeu n'est installé qu'une fois entier, empreinte comprise : un refus laisse l'ancien en place.
- **Libretro.** `retro_serialize` ne peut plus sortir du tampon si un joueur du câble link dépasse la marge de l'état (soustraction non signée). `retro_unserialize` et `retro_set_environment` ne laissent plus passer une erreur d'allocation par l'ABI C, et les messages d'erreur des fonctions `retro_*` sont écrits au journal sans demander de mémoire : un manque de mémoire ne peut plus relancer une exception en sortant. Un redémarrage interrompu (`retro_reset`) est écrit au journal.
- **API C.** Un `ravenemu_load` refusé laisse la partie en cours accessible, batterie non sauvegardée comprise, au lieu de la rendre injoignable. L'en-tête précise la limite de 0x58000 octets de `ravenemu_read_achievement_memory`, le moment où prendre `ravenemu_state_size` avant `ravenemu_save_state`, et l'arrêt des appels réseau avant `ravenemu_remote_link_destroy`.
- Nouveaux tests : état PPU au bout de la ligne, Huffman sans fin, ObjAffineSet avec un écart de 2^30 et 2^31, durée distante hors bornes, phase DMA de 2^31 cycles, et états Libretro abîmés (tronqués, mauvais en-tête, longueur au-delà de la fin, tampon trop petit) refusés sans toucher au jeu en cours. Chaque test des cœurs échoue sans son correctif (ObjAffineSet sous UBSan) ; les états Libretro abîmés étaient déjà refusés, ce test le garde.

## 0.15.3 - 2026-11-07

Cette version supprime les parasites du son des jeux Game Boy Color. L'API, Libretro, le Game Boy, le GBA et le format des états ne changent pas.

### Corrigé

- **Son GBC sans parasites.** En mode CGB, et en GBC sur GBA, le condensateur de sortie de l'APU prenait le facteur MGB/CGB de Pan Docs, 0,998943 par point : une constante de temps de 0,23 ms, une coupure vers 700 Hz. Il ne retirait plus seulement la composante continue, mais les graves et la forme des notes : chaque front de créneau sortait en impulsion retombée en moins d'une milliseconde, et une basse devenait un train de claquements. La CGB prend désormais le facteur du DMG, 0,999958 par point (5,7 ms, coupure vers 28 Hz) : le condensateur qui retire lentement le continu, tel que Pan Docs le décrit. Sur des homebrews GBC joués une minute, la part de l'énergie sous 300 Hz remonte de 15 à 59 % (Aevilia), de 25 à 60 % (A Walk in Snow) et de 22 à 42 % (K32 Super Rocket), et les pics qui dominaient le signal s'atténuent. Seuls les échantillons sonores en mode CGB changent ; le mode DMG et le format des états restent les mêmes.
  - Nouveau test `cgb_keeps_a_square_waves_shape_test` : un carré à 64 Hz garde, 2 ms après un front, plus de 60 % de son niveau, sur DMG comme sur CGB. Il échoue avec l'ancien facteur.
  - Écartés par mesure : l'APU différé de la 0.15.1 (le son de cinq homebrews GBC est identique au bit près entre la 0.15.0 et la 0.15.2) et le coût du cœur (une image GBC coûte moins qu'une image GBA).

## 0.15.2 - 2026-11-07

Cette version rend le cœur Game Boy Advance plus rapide, sans rien changer à ce qu'il émule : images, son et états restent identiques au bit près. Avec le compilateur de l'application Android, un jeu demande de 1,2 à 1,5 fois moins de calcul par image. L'API, Libretro, le Game Boy et le format des états ne changent pas.

### Modifié

- **Cœur GBA plus rapide**, sans rien changer à ce qu'il émule : images, son et état final restent identiques au bit près à ceux de la 0.15.1 sur les suites de test publiques (jsmolka, FuzzARM, suite mGBA) et sur quatre jeux du commerce testés en local (Pokémon Émeraude et Rubis, The Legend of Zelda, Grand Theft Auto), en jouant de 1 200 à 3 000 images avec des appuis de touches, en traçant une image sur deux et en rechargeant un état en route. Le format des états ne change pas. Instructions exécutées par image : de 1,21 à 1,54 fois moins avec Clang en -O2, le compilateur de l'application Android (Pokémon Émeraude 33,2 → 23,7 millions, GTA 10,2 → 6,6) ; de 1,05 à 1,45 fois moins en -O3, l'option des cœurs Libretro publiés.
  - **Affichage.** La palette convertie en couleurs est relue au début de chaque ligne tracée, au lieu d'une conversion par pixel ; une comparaison à la copie de la dernière conversion voit tout changement, CPU, DMA, codes de triche ou état chargé. Un plan texte sans mosaïque se trace tuile par tuile : chaque ligne de tuile se décode une fois, et une ligne transparente est sautée. Les bornes des fenêtres et les coefficients de BLDALPHA et BLDY se lisent une fois par ligne. Sans effet de couleur, seule la couche du dessus est tenue à jour.
  - **Processeur.** La lecture d'instruction est spécialisée en Thumb et en ARM, et le tampon de préchargement de la cartouche garde l'adresse de l'instruction qu'il attend au lieu de la recalculer. Les formats Thumb rares (PUSH/POP, LDMIA/STMIA, SWI, drapeau C de MUL) sont sortis de la fonction principale : ils imposaient à chaque instruction Thumb une grande pile et le contrôle de pile du compilateur. La ligne d'IRQ suit les changements de IE, IF et IME au moment où ils se font, au lieu d'être recalculée à chaque instruction ; elle l'est encore une fois après un chargement d'état.
  - Nouveaux tests : couleur changée en cours d'image, plan texte 4 bpp (miroir, banque de palette, ligne vide, défilement), tuile 8 bpp hors de la VRAM, fenêtre passant par le bord de l'écran, BLDY, et tampon de préchargement restauré par un état.

### Corrigé

- **Test `gba_raven_core`** : la vérification de l'horloge réelle du RTC échouait environ une fois sur 200. Le cœur lit l'heure avec `std::chrono::system_clock`, le test la lisait avec `std::time()`, qui suit sous Linux une horloge grossière en retard de quelques millisecondes ; à un changement de seconde, le RTC paraissait en avance. Le test lit désormais la même horloge que le cœur.

## 0.15.1 - 2026-11-07

Cette version rend le cœur Game Boy et Game Boy Color environ 1,7 fois plus rapide, sans rien changer à ce qu'il émule : images, son et états restent identiques au bit près. L'API, Libretro, le GBA et le format des états ne changent pas.

### Modifié

- **Cœur GB/GBC environ 1,7 fois plus rapide**, sans rien changer à ce qu'il émule : sur 1 339 ROM de test publiques, 120 images chacune, en DMG comme en CGB, les images, le son et l'état final restent identiques au bit près. Seule exception, attendue : l'état des trois ROM MBC3 à horloge, qui contient l'heure réelle et change déjà d'un lancement à l'autre. Le format des états ne change pas. Mesuré en instructions exécutées par image, une mesure stable : de 23,7-25,4 millions à 14,5-14,7 millions, sur une boucle vide, `cpu_instrs`, `dmg_sound` et `cgb-acid-hell`.
  - **APU différé.** Le bus l'avançait d'un point à chaque point d'image, soit 70 224 fois par image, et y faisait avancer les quatre canaux : le quart du temps du cœur. Entre deux accès, l'APU ne dépend que du temps écoulé, et avancer de N points d'un coup donne le même état que N pas d'un point. `tick()` entasse donc le temps et `catch_up()` le rattrape avant tout ce qui l'observe ou le modifie : registres, wave RAM, PCM, échantillons, séquenceur, états et réinitialisations. Au-delà de 65 536 points en attente, l'APU rattrape de lui-même, pour qu'un jeu qui bloque le séquenceur n'entasse pas le temps sans fin.
  - **Raccourcis exacts du bus**, quand le composant n'a rien à faire : timer éteint sans recharge en attente, port série sans transfert, DMA OAM et HDMA inactives, cartouche sans horloge propre (toutes sauf HuC3 et MBC7).
  - **PPU** : des points 81 à 447 d'une ligne, aucune règle de point particulier ne s'applique. En HBlank et en VBlank, le point se réduit alors à avancer le compteur, et en mode 3 le reste de la fonction est sauté.
  - Nouveau test `deferred_ticks_match_single_dots_test` : un APU avancé par paquets irréguliers rend exactement les registres, la wave RAM, le PCM, les échantillons et l'état d'un APU avancé point par point, sur DMG comme sur CGB.

## 0.15.0 - 2026-11-06

Cette version lit les codes de triche comme l'appareil et comme RetroArch : les codes GameShark et Action Replay déjà déchiffrés, les codes Codebreaker tels que la console les exécute et le Game Genie par groupes passent, dans RetroArch comme dans l'API C. Elle corrige aussi le câble distant, plusieurs fonctions du BIOS et l'EEPROM des grandes ROM GBA, ainsi que le joypad Game Boy. Le format des états ne change pas.

### Ajouté

- **Codes de triche, comme dans RetroArch.** Sur la base de triches de RetroArch (`libretro-database`), 229 codes GBA et 115 codes Game Boy et GBC refusés jusqu’ici passent : 6 530 codes GBA sur 7 009 et 24 675 codes Game Boy et GBC sur 26 124. Les codes encore refusés sont des modèles à compléter (`01??E4CB`), des codes Xploder, des codes Codebreaker chiffrés, des patchs de ROM ou des codes faux.
  - GBA : les codes GameShark v1/v2 et Action Replay v3 **déjà déchiffrés** (« RAW ») sont reconnus. Ils sont essayés après les deux chiffrements, avec les mêmes règles de validité, et aucun code de la base n'y change de sens.
  - GBA, Codebreaker : un code s'exécute comme sur l'appareil. Un miroir de l'EWRAM, de l'IWRAM, de la palette ou de l'OAM vise la mémoire qu'il répète. Une écriture de 16 bits ignore le bit 0 de l'adresse. Une écriture de 8 bits (type 3) ne garde que l'octet bas. Les lignes d'identification (type 0) et d'accroche (type 1) ne sont pas vérifiées, et le remplissage d'une chaîne d'octets (type 5) est ignoré.
  - Game Boy : un Game Genie écrit par groupes de trois chiffres séparés comme des lignes (`ABC+DEF+GHI`) est lu comme `ABC-DEF-GHI`.

### Corrigé

- **Câble distant** : la file des messages reçus est bornée à 4 096. Un partenaire qui envoie plus que la console ne lit, par exemple pendant une pause de l'émulation, ne remplit plus la mémoire : il n'est plus servi, `ravenemu_remote_link_receive()` renvoie 0 et l'état passe à `RAVENEMU_REMOTE_INCOMPATIBLE` jusqu'au prochain branchement. Un transfert en attente finit comme sans câble.
- **GBA, MidiKey2Freq** : une touche hors de la plage MIDI ne convertit plus une valeur infinie ou NaN en entier (comportement indéfini, relevé par UBSan). Le résultat est borné à 0-0xFFFFFFFF.
- **GBA, CpuFastSet** : la copie et le remplissage se font par blocs de huit mots, le compte arrondi au multiple de 8 supérieur, comme le BIOS (GBATEK). Un compte de 1 copiait un seul mot.
- **GBA, EEPROM et ROM de 32 Mio** : au-delà de 16 Mio de ROM, seuls les 256 derniers octets de 0x0D000000-0x0DFFFFFF vont à l'EEPROM (GBATEK) ; le reste lit la ROM, au lieu de bits d'EEPROM.
- **Game Boy, interruption joypad** : sélectionner dans P1 une rangée où une touche est déjà tenue abaisse sa ligne et demande l'interruption, comme un appui (Pan Docs). Un appui dont la ligne était déjà basse, par l'autre rangée, n'en demande plus.
- **Game Boy, STOP** : seul un appui pendant l'arrêt le termine. Un appui antérieur, déjà relâché, réveillait la console aussitôt.
- **Publication** : lancée à la main avec un tag qui existe déjà, elle compile et publie le commit de ce tag, et non la tête de la branche choisie. Un seul commit est résolu pour toute la publication.

## 0.14.0 - 2026-10-24

Cette version ouvre à l'API C les codes de triche et la Game Boy Color d'une Game Boy Advance : RavenEmu Android et RavenEmu PC peuvent désormais les proposer. Les jeux Game Boy sur une GBC sans boot ROM prennent les couleurs que sa boot ROM leur choisit, jeu par jeu, au lieu de gris. Le BIOS d'une GBA peut remplacer celui de RavenCore. Dans RetroArch, les cartouches à capteur suivent les capteurs du téléphone, et deux joueurs peuvent se relier par câble link. Le format des états GBA passe à 17.

### Ajouté

- **Codes de triche dans l'API C.**
  - `ravenemu_check_cheat()` vérifie un code sans handle, au moment où le joueur le saisit, et laisse la raison d'un refus, en français, dans `ravenemu_last_error()`.
  - `ravenemu_set_cheats()` active une liste de codes à partir de l'image suivante. Si une entrée est refusée, la liste précédente reste active et l'erreur nomme l'entrée, par exemple « code de triche 2 : … ».
  - Formats, survie aux chargements, réinitialisations et états : comme dans RetroArch depuis la 0.12.0.
- **`RAVENEMU_GBC_ON_GBA`** pour `ravenemu_create()`, et `sdk::System::game_boy_color_on_gba` dans le SDK C++ : la GBC d'une Game Boy Advance, `gb::HardwareModel::agb` depuis la 0.13.0.
- **Couleurs des jeux Game Boy sur une GBC.**
  - `Core::set_gbc_compatibility_colors()` et `ravenemu_set_gbc_colors()` choisissent entre les couleurs de la boot ROM, quatre gris et les douze combinaisons de touches tenues pendant son logo : Haut (brun), A + Haut (rouge), B + Haut (sépia), Gauche (bleu), A + Gauche (bleu-violet), B + Gauche (gris), Bas (pastel), A + Bas (jaune et rouge), B + Bas (jaune et brun), Droite (vert et rouge), A + Droite (vert et bleu), B + Droite (négatif).
  - Dans RavenGBC, l'option **Couleurs des jeux Game Boy** fait de même.
  - Le réglage s'applique tout de suite, et survit au chargement, au reset et aux états.
  - Les gris sont ceux de B + Gauche.
  - Une boot ROM fournie choisit elle-même ses couleurs.
- **Gris neutre** pour la palette de RavenGB.
- **Vrai BIOS GBA, facultatif.** RavenCore répond toujours lui-même aux appels du BIOS par défaut ; le BIOS d'une console (16 Kio) peut désormais le remplacer.
  - Les SWI et les IRQ du jeu passent alors par lui. Il joue son animation de démarrage, sauf si on la saute : le jeu démarre alors comme avec le BIOS de RavenCore.
  - `Core::set_gba_bios()` et `ravenemu_set_gba_bios()` le donnent, ou reviennent au BIOS de RavenCore, à partir du prochain chargement ou reset.
  - Dans RavenGBA, l'option **BIOS** (catégorie Système) lit `gba_bios.bin` dans le dossier système de RetroArch : « BIOS de RavenCore » par défaut, « Vrai BIOS, avec son animation » ou « Vrai BIOS, sans animation ». S'il manque, un message le dit et le jeu démarre avec le BIOS de RavenCore. Le fichier d'informations le déclare comme micrologiciel facultatif.
  - Un état ne se recharge qu'avec le BIOS qui l'a créé. Sinon, le refus le dit : « État pris avec un vrai BIOS GBA, absent ici », « … avec le BIOS de RavenCore, pas avec un vrai BIOS » ou « … avec un autre BIOS GBA ». Recharger un état garde le BIOS en place.
  - Aucun BIOS n'est fourni.
- **Capteurs du téléphone ou de la manette dans RetroArch.** Les cœurs Libretro lisent ceux de l'appareil par l'interface de capteurs de Libretro, avec les conversions de RavenEmu Android :
  - l'accéléromètre incline Kirby Tilt 'n' Tumble et Command Master (MBC7), Yoshi Topsy-Turvy et Koro Koro Puzzle. À plat, l'appareil n'est pas incliné ;
  - le gyroscope fait tourner WarioWare: Twisted! : un demi-tour par seconde donne la pleine échelle ;
  - le capteur de lumière éclaire Boktai si l'option **Lumière du capteur solaire** vaut « Capteur de lumière de l'appareil ». Rien sous 1000 lux, le plein soleil vers 100 000 lux. Sans capteur, un message le dit et le soleil reste à 50 %.
  - Le stick gauche s'ajoute aux capteurs. Un capteur ne s'allume que pour un jeu qui en a besoin, et s'éteint avec lui. L'option **Capteurs de l'appareil** (catégorie Commandes) les ignore.
  - L'accélération en m/s² de certains frontends anciens est reconnue à sa norme, et ramenée en g.
- **Deux joueurs reliés par câble link dans RetroArch.** Les trois cœurs déclarent le sous-système « Deux joueurs (câble link) » (`link`). Il charge deux jeux de la même console et les relie par le câble local de RavenCore :
  - les deux écrans côte à côte, le joueur 1 à gauche ;
  - une manette par joueur, ports 1 et 2 ;
  - une sauvegarde par joueur, que RetroArch nomme d'après la ROM de chacun : celle d'une partie en solo sert telle quelle ;
  - le son du joueur 1 ;
  - les états, le reset et le mélange d'images pour les deux consoles. L'état d'une paire ne se recharge pas dans un jeu seul, ni l'inverse. Si la console du joueur 2 refuse le sien, le joueur 1 reprend le sien.
  - RetroAchievements, les codes de triche et les capteurs de l'appareil restent au joueur 1. Sur GBA, il est la console parente.
  - Un jeu refusé nomme son joueur : « joueur 2 : … ».

### Modifié

- **Jeu Game Boy sur une GBC sans boot ROM : en couleurs.** Il prend les palettes que la boot ROM de la GBC lui aurait choisies, au lieu de quatre gris (Pan Docs, « Compatibility palettes ») :
  - Pour un jeu Nintendo, d'après la somme des octets de son titre, départagée par sa 4e lettre quand il le faut : Pokémon Rouge en rouge, Bleu en bleu, Pikachu en jaune et rouge, Link's Awakening, Metroid II, Kirby… La table vient de [The Cutting Room Floor](https://tcrf.net/Notes:Game_Boy_Color_Bootstrap_ROM) (CC BY 3.0, voir `NOTICE.md`) : 94 sommes de titre, 51 triplets de palettes.
  - Pour un jeu inconnu ou d'un autre éditeur, ses couleurs par défaut. Ce sont celles des captures de référence des suites publiques.
  - Le B qu'elle laisse au jeu (la somme du titre) et le choix des palettes partagent désormais le même calcul.
  - L'option **Gris** rend l'ancien rendu.
  - `gb_conformance_runner --compatibility-palette boot-default` reste accepté, mais n'est plus nécessaire.

### Compatibilité des états instantanés

- **GBA : format 17.** L'état se termine par le BIOS utilisé : celui de RavenCore, ou l'empreinte SHA-256 du vrai. Les états aux formats 11 à 16 se rechargent, avec le BIOS de RavenCore. Un état au format 17 ne se recharge pas dans une version 0.13.x.
- **GB/GBC : format 11**, inchangé.
- **API C : version 0**, avec des ajouts seulement : une valeur de `ravenemu_system`, l'énumération `ravenemu_gbc_colors` (automatique, gris et douze combinaisons de touches) et quatre fonctions. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.14)`.
- **API C++** : `Core` gagne `set_gbc_compatibility_colors()` et `set_gba_bios()`, avec une implémentation par défaut, et `GbcCompatibilityColors`.

### Tests

- `ravenemu_c_api` :
  - codes vérifiés sans handle : acceptés, ou refusés avec leur raison, pour une autre console, un système inconnu ou un code absent ;
  - deux codes GameShark appliqués dès l'image suivante ;
  - une liste dont la deuxième entrée est refusée : l'erreur la nomme, et l'ancienne liste reste active ;
  - avec `RAVENEMU_GBC_ON_GBA`, B vaut 1 au démarrage d'un jeu GBC ; avec `RAVENEMU_GBC`, il vaut 0 ;
  - les couleurs d'un jeu Game Boy, puis des gris, puis le négatif de B + Droite ; le réglage est refusé sur un handle GB ou GBA, ou pour une valeur hors de l'énumération.
  - un BIOS fait maison qui marque l'IWRAM à la mise sous tension : pris au reset suivant seulement, sauté sur demande, refusé avec une autre taille ou sur un handle GB ; son état est refusé par un handle sans lui.
- `gba_raven_core`, avec un BIOS fait maison qui marque l'IWRAM au démarrage et à chaque SWI :
  - le démarrage et le SWI passent par lui, sauf le démarrage quand on saute l'animation ;
  - un état repris continue au même cycle ; il est refusé sans ce BIOS, avec un autre, et un état du BIOS de RavenCore l'est avec lui ;
  - un nouveau réglage attend le prochain chargement, un état rechargé d'ici là garde le BIOS en place ;
  - une image de 16383 octets est refusée, et un cœur Game Boy refuse tout BIOS GBA.
- `libretro_smoke_gba` : l'option BIOS déclarée, avec sa catégorie et sa valeur par défaut ; `gba_bios.bin` lu dans le dossier système, avec ou sans animation, ignoré par défaut, et son absence signalée.
- `libretro_smoke_*`, avec un frontend qui simule les capteurs d'un téléphone :
  - un jeu MBC7 lit 81D0h - 38h et 81D0h + 1Ch pour un appareil penché de 0,5 g à droite et de 0,25 g vers le joueur, en g comme en m/s². Il lit le repos quand l'option écarte les capteurs, et l'accéléromètre se rallume quand on la rétablit en jeu ;
  - sur GBA, Yoshi Topsy-Turvy lit 392h + 70h et 3A0h - 38h dans son capteur. WarioWare: Twisted! lit 6C0h + 191h pour un quart de tour par seconde dans le sens horaire ;
  - le capteur de lumière ne s'allume pour Boktai que si l'option le demande, et son absence est signalée ;
  - chaque capteur s'éteint avec le jeu.
- `libretro_smoke_*`, deux joueurs reliés :
  - le sous-système est déclaré, avec une sauvegarde par joueur ;
  - l'image fait deux largeurs et les deux manettes sont lues ;
  - le joueur 1 reçoit par le port série le mot du joueur 2 : 99h sur Game Boy (horloge interne contre externe), 5678h sur GBA en mode multijoueur. Sans câble, il lirait FFh ;
  - la sauvegarde du joueur 2 est exposée, et un état de la paire se recharge à l'identique ;
  - un état de jeu seul est refusé, comme un sous-système inconnu ou un seul jeu.
- En local, Pokémon Émeraude et Rubis chargés ensemble par le sous-système, chacun avec sa sauvegarde, s'affichent côte à côte. Rubis reprend sa partie jusqu'au Club Link.
- En local, avec un BIOS libre (Cult-of-GBA, licence MIT, ni fourni ni téléchargé par le projet) : les ROMs de test jsmolka (ARM, Thumb, mémoire, sauvegardes, NES, unsafe) et FuzzARM passent, avec et sans animation. Seule jsmolka bios échoue : elle vérifie des mots du BIOS officiel, absents de ce BIOS libre. Pokémon Émeraude et Zelda tournent comme avec le BIOS de RavenCore, à la même vitesse.
- `gb_raven_core` : un jeu Game Boy sur GBC dessine la couleur `$1BEF` de la boot ROM. En gris, ses trois composantes sont égales. Après un état ou un reset, il suit le réglage du moment.
- `gb_raven_core`, couleurs par jeu : un titre de somme 14h (Pokémon Rouge) dessine le rouge `$421F`, avec l'éditeur 01h comme avec 33h et « 01 ». La somme 61h dessine le bleu `$7E8C` avec la 4e lettre E (Pokémon Bleu), et les couleurs par défaut avec une autre lettre. Un autre éditeur garde aussi les couleurs par défaut. B + Droite donne le négatif, et « Gris » les gris de B + Gauche. Le test échoue quand on ignore la 4e lettre ou l'éditeur (vérifié).
- Les tables se vérifient à la compilation : chaque titre et chaque combinaison renvoie à un triplet, aucun titre n'est ambigu, et le triplet par défaut est celui des captures de référence.
- Les 200 tests publics GB/GBC passent toujours, captures de référence en mode de compatibilité comprises.

## 0.13.0 - 2026-10-23

Cette version comble une partie des écarts relevés face aux émulateurs de référence (mGBA, SameBoy, Gambatte, VBA-M, gpSP). Elle prend en charge le MBC30 et le mode Game Boy Advance des jeux GBC. Les cœurs Libretro gagnent le mélange d'images, le blocage des directions opposées et les boot ROMs du dossier système. L'API C et le format des états ne changent pas ; l'API C++ gagne un modèle de console.

### Ajouté

- **MBC30** (Pan Docs), la variante du MBC3 de *Pocket Monsters Crystal Version* au Japon : 8 bits de banque de ROM (4 Mio) et 8 banques de RAM (64 Kio). Elle est reconnue à 64 Kio de RAM ou à plus de 2 Mio de ROM.
- **Mode Game Boy Advance des jeux GBC.**
  - `gb::HardwareModel::agb` émule la GBC d'une Game Boy Advance. Elle démarre avec B = 1, ou B + 1 pour un jeu Game Boy, avec les drapeaux de ce `INC B` (Pan Docs, « Power Up Sequence »).
  - Le diviseur prend un cycle machine d'avance : celui de cette instruction en plus.
  - Les jeux qui testent ce registre débloquent leurs bonus GBA, comme Zelda Oracle ou Shantae.
  - Dans RavenGBC, l'option **Console émulée** le choisit, au chargement du jeu.
- **Mélange d'images** dans les trois cœurs Libretro. Il atténue le scintillement des jeux qui affichent un objet une image sur deux.
  - **Mélange** : la moyenne des deux dernières images.
  - **Rémanence** : 3/8 de l'image affichée restent, comme sur un écran LCD lent.
  - Le mélange s'applique après la palette et la correction des couleurs, et change pendant le jeu.
- **Directions opposées.** Haut et bas, ou gauche et droite, tenus ensemble sont bloqués par défaut, comme sur la croix de la console : certains jeux plantent sinon. L'option **Directions opposées** les autorise.
- **Boot ROMs du dossier système** pour RavenGB et RavenGBC.
  - Fichiers reconnus : `dmg_boot.bin` ou `gb_bios.bin`, `cgb_boot.bin` ou `gbc_bios.bin`, `agb_boot.bin` pour le mode Game Boy Advance.
  - Un fichier d'une autre taille est ignoré, et le journal le dit.
  - L'option **Boot ROM** les écarte. Les fichiers `.info` les déclarent comme firmwares facultatifs.
- Les options Libretro se rangent en quatre catégories : **Image**, **Commandes**, **Système** et **Cartouche**.

### Modifié

- **États et boot ROM.** Un état pris après la boot ROM se recharge désormais avec ou sans elle : seul un état pris pendant qu'elle s'exécute exige la même. Avant, ajouter ou retirer une boot ROM rendait inutilisables tous les états d'un jeu Game Boy. C'est utile maintenant que RavenGB et RavenGBC prennent celle du dossier système, où beaucoup de joueurs en ont déjà une pour d'autres cœurs.

### Compatibilité des états instantanés

- **GBA : format 16** et **GB/GBC : format 11**, inchangés. Un état MBC30 accepte désormais une banque de ROM au-delà de 0x7F.
- **API C : version 0**, inchangée. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.13)`.
- **API C++** : `gb::HardwareModel` gagne `agb`. Un `switch` exhaustif sur ce type doit traiter la nouvelle valeur.

### Tests

- Tests publics GB/GBC : 200 au lieu de 198. Les tests Mooneye `misc/boot_regs-A` et `misc/boot_div-A` passent en mode `agb`. Ils échouent en mode `cgb`, comme `boot_regs-cgb` et `boot_div-cgbABCDE` échouent en mode `agb`. `gb_conformance_runner` et le manifeste acceptent `--hardware agb`.
- `gb_cartridge_mbc` : banques de ROM et de RAM du MBC30, face à un MBC3 ordinaire, et banque de ROM au-delà de 0x7F gardée dans un état.
- `gb_raven_core` :
  - registres de démarrage d'une GBC et d'une GBA, pour un jeu GBC et pour un jeu Game Boy ;
  - avec une boot ROM maison : un état pris après elle se recharge sans elle ou avec une autre ; un état pris pendant qu'elle s'exécute est refusé dans ces deux cas.
- `libretro_smoke_gb`, `_gbc` et `_gba`, avec un jeu maison qui change de fond à chaque image et range la croix en mémoire :
  - gauche et droite bloquées, puis autorisées ;
  - le mélange et la rémanence, comparés au calcul attendu ;
  - sur RavenGBC, B vaut 1 en mode Game Boy Advance ;
  - une boot ROM du dossier système exécutée, puis écartée par l'option ;
  - les nouvelles options v2 et leurs catégories.

## 0.12.0 - 2026-10-23

Cette version complète les cœurs Libretro. Ils sont désormais publiés pour Windows, macOS et Android, prennent les codes de triche, gagnent des options d'image et signalent un jeu refusé. Le cœur ne dessine plus les images que RetroArch n'affiche pas. L'API C et le format des états ne changent pas ; l'API C++ gagne les codes de triche.

### Ajouté

- **Cœurs Libretro pour toutes les plateformes.** Chaque version publie, en plus de Linux x86-64 :
  - Windows x86-64, avec le runtime C++ intégré : pas de redistribuable Visual C++ à installer ;
  - macOS 11 ou plus récent, en une bibliothèque universelle pour Apple Silicon et Intel ;
  - Android arm64-v8a et armeabi-v7a, nommés `*_libretro_android.so` comme l'attend RetroArch pour Android.
  - La CI fournit les mêmes archives à chaque PR. `libretro/package.sh` les assemble : `cores/` et `info/`.
- **Options d'image.**
  - Palette des jeux Game Boy : vert (Game Boy) ou gris (Game Boy Pocket).
  - Correction des couleurs LCD pour la Game Boy Color et la Game Boy Advance, la même que RavenEmu Android : environ 28 % de désaturation, puis un gamma de 1,15.
  - Elles changent pendant le jeu.
- **Options v2.** Libellés et descriptions en français, catégories **Image** et **Cartouche**. Un frontend plus ancien garde la liste simple.
- **Codes de triche**, dans le menu de RetroArch et par l'API C++.
  - **Game Boy et Game Boy Color.**
    - Game Genie : l'octet est remplacé à la lecture de la ROM, comme le fait l'adaptateur ; sa comparaison écarte les autres banques.
    - GameShark : l'octet est récrit à chaque image, dans la banque de RAM de cartouche choisie par le code, ou de WRAM de la GBC pour 91 à 97.
  - **GBA.** Codebreaker non chiffré, GameShark v1/v2 et Pro Action Replay v3 chiffrés ; ces deux derniers sont reconnus à leur déchiffrement.
    - Pris en charge : écritures, additions, opérations logiques, séries, écritures par pointeur, conditions (dont les blocs IF/ELSE/ENDIF de l'Action Replay) et test des touches du Codebreaker.
    - Les codes s'exécutent une fois par image, hors du temps de la console : ils ne décalent pas son minutage d'un cycle.
  - **Non pris en charge**, avec un message qui le dit : patchs de ROM GBA, changement de chiffrement `DEADFACE`, Codebreaker chiffré (type 9). Les codes des boutons des appareils n'ont pas d'effet, faute de bouton.
  - Un code refusé affiche sa raison à l'écran et dans le journal, par exemple « RavenGB : code de triche 1 refusé (le code GameShark « 01021234 » vise la ROM : c'est le rôle d'un code Game Genie) ».
  - **API C++.** `parse_cheat` décode une entrée et lève `CheatError` avec la raison d'un refus ; `Core::set_cheats` active la liste. Les codes survivent aux changements de ROM, aux réinitialisations et aux états, sans y être enregistrés.
- **Messages.** Un jeu refusé affiche sa raison à l'écran et dans le journal de RetroArch, par exemple « RavenGBA : jeu refusé (ROM GBA trop courte) ». Une erreur pendant le jeu, une fois par partie, et un état refusé vont au journal.

### Modifié

- **Images et son que RetroArch n'utilise pas** (anticipation « run-ahead », avance rapide), signalés par `RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE` :
  - le cœur GBA ne dessine pas l'image ;
  - aucun cœur ne la convertit, et RetroArch reprend l'image précédente ;
  - le son de ces images est jeté.
  - Sans reprise de l'image possible (`RETRO_ENVIRONMENT_GET_CAN_DUPE`), l'image est dessinée comme avant.
- Les options GBA existantes gardent leurs clés et leurs valeurs ; seuls leurs libellés passent en français.

### Compatibilité des états instantanés

- **GBA : format 16** et **GB/GBC : format 11**, inchangés.
- **API C : version 0**, inchangée. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.12)`.
- **API C++** : `Core::set_cheats` est une méthode virtuelle nouvelle, avec une implémentation par défaut.

### Tests

- `libretro_smoke_gb`, `_gbc` et `_gba` vérifient désormais :
  - une image et un son non voulus : l'image précédente est reprise, aucun son ne part ;
  - un jeu refusé : sa raison est au journal et à l'écran ;
  - la palette Pocket appliquée en cours de jeu ;
  - la correction LCD sur un fond rouge pur : `0xBF0909`, la valeur que donne la formule de RavenEmu Android ;
  - les options v2 : libellés, catégories et valeurs par défaut ;
  - un code de triche refusé, signalé avec sa raison, et un code accepté, appliqué dès l'image suivante.
- `ravenemu_cheats`, nouveau :
  - le chiffrement contre le vecteur de référence de l'algorithme (clé et bloc nuls), et l'aller-retour avec les graines GameShark et Action Replay ;
  - le décodage de chaque format, dont l'exemple GameShark de Pan Docs ;
  - les conditions, les blocs IF/ELSE/ENDIF, les séries et les pointeurs, exécutés sur une mémoire factice ;
  - les refus, chacun avec sa raison.
- `gb_raven_core` : un code Game Genie appliqué ou écarté par sa comparaison, un code GameShark en WRAM, en banque de WRAM GBC et en banque de RAM de cartouche, et des codes conservés à la réinitialisation.
- `gba_raven_core` :
  - des codes qui ne font que lire laissent l'état identique, octet pour octet, à celui d'une console sans codes ;
  - des écritures appliquées, conservées après un état, retirées avec la liste.

## 0.11.0 - 2026-10-23

Cette version corrige le câble link distant GBA quand le réseau ajoute de la latence : un échange Pokémon entre un téléphone et un PC finissait en « Erreur de communication » dès la salle d'échange. Le protocole reste en version 2 et se relie à la 0.10.0. L'API C ne change pas ; l'API C++ gagne une méthode.

### Corrigé

- **Câble link distant GBA : l'enfant prenait des images d'avance.** Le parent attend un aller-retour réseau par transfert. Pokémon en enchaîne neuf par image : à 20 ms d'aller-retour, le parent tombait vers 5 images par seconde, pendant que l'enfant restait à 60. L'enfant prenait ainsi des centaines d'images d'avance, et le jeu finissait par couper la liaison.
  - L'enfant suit désormais l'horloge du parent grâce aux écarts annoncés à chaque transfert. Tant que les transferts se suivent à moins de deux images d'écart, il ne la devance pas de plus de trois images : il attend le transfert suivant, une seconde au plus. Si le parent se tait au-delà, ou quitte le mode multijoueur, l'enfant reprend sa route ; l'avance prise hors liaison ne compte pas.
  - Mesuré sur un échange Pokémon Rubis ↔ Émeraude complet, avec la latence dès l'arrivée au Centre Pokémon et deux consoles tenues à 60 images par seconde :

    | Aller-retour | 0.10.0 | 0.11.0 |
    |---|---|---|
    | 0 ms | échange complet, 60 images par seconde | échange complet, 60 images par seconde |
    | 6 ms | non mesuré ainsi | échange complet, les deux consoles vers 16 images par seconde pendant la liaison |
    | 20 ms | « Erreur de communication » dans la salle d'échange | échange complet, les deux consoles vers 5 images par seconde pendant la liaison |

### Modifié

- **Écart des transferts GBA.** Une GBA envoie l'écart réel depuis son transfert précédent ; la 0.10.0 le plafonnait à 125 829 cycles. Une console en 0.10.0 le plafonne toujours à la réception : rien ne change pour elle. Face à un partenaire en 0.10.0, reconnu à un écart d'exactement 125 829 cycles, une console en 0.11.0 ne règle pas son pas sur lui.
- `RemoteLink::wait_until` attend un message sans le prendre, jusqu'à une échéance.

### Compatibilité des états instantanés

- **GBA : format 16** et **GB/GBC : format 11**, inchangés.
- **API C : version 0**, inchangée. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.11)`.

### Tests

- `ravenemu_remote_link` : pas du parent en GBA, comme dans les salles du Club Câble. Le parent fait 12 images de 9 transferts chacune, à 0 et 5 ms de latence, et l'enfant tourne comme dans une application. Entre le premier transfert et le dernier, l'enfant compte au plus 15 images. Avec la version 0.10.0, il en compte 88 à 5 ms.

## 0.10.0 - 2026-10-23

Cette version corrige le câble link distant : les échanges Pokémon (Émeraude, Rubis, Saphir, et Rouge, Bleu, Or, Argent en Game Boy) finissaient en « Erreur de communication ». Le protocole du câble passe en version 2, incompatible avec la 0.9 : les deux appareils doivent se mettre à jour. L'API C ne change pas ; le format des états non plus.

### Corrigé

- **Câble link distant : une réponse donnée trop tôt.** La console qui ne cadence pas répondait dès la lecture du message. Deux transferts lus d'affilée, entre deux images notamment, recevaient la même donnée : son jeu n'avait pas encore traité le premier. Pokémon, qui enchaîne neuf transferts par image, voyait des données en double et coupait la liaison.
  - Chaque transfert porte désormais l'écart, en cycles, depuis le précédent chez la console qui cadence. L'autre n'y entre pas avant que sa propre horloge ait avancé d'autant depuis le transfert précédent : son jeu a eu le temps de préparer la donnée suivante, comme sur deux consoles. Une attente ne dépasse jamais la durée du plus long transfert GBA, ou une demi-image en Game Boy.
  - Entre deux images, ou console en pause, `ravenemu_remote_link_service` fait avancer la console jusqu'à ce cycle au lieu de laisser l'autre attendre l'image suivante. L'image suivante reprend là où elle en est.
  - Mesuré sur un échange Pokémon Rubis ↔ Émeraude entre deux consoles tenues à 60 images par seconde : l'échange va jusqu'au bout sans erreur. La console qui cadence garde 60 images par seconde sans latence, environ 46 avec 2 ms d'aller-retour et 17 avec 6 ms pendant les transferts.

### Modifié

- **Protocole du câble distant, version 2.** Le transfert passe à 18 octets : il gagne l'écart depuis le transfert précédent (u32). Un appareil en version 1 voit le câble incompatible.
- `LinkPort::link_clock()` donne l'horloge d'un port série Game Boy ; `RemoteLink::Transfer` gagne `interval`.

### Compatibilité des états instantanés

- **GBA : format 16** et **GB/GBC : format 11**, inchangés.
- **API C : version 0**, inchangée ; **API C++** : `LinkPort` gagne une méthode virtuelle, recompiler le code qui en dérive. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.10)`.

### Tests

- `ravenemu_remote_link` : rafales comme celles de Pokémon, l'enfant ou l'esclave tournant comme dans une application (une image, puis il dort et son câble est servi), à 0, 5 et 20 ms de latence.
  - GBA : 16 transferts multijoueur, chaque numéro de l'enfant reçu une fois et dans l'ordre.
  - Game Boy : 16 octets d'un maître GB vers un esclave GBC, de même.
  - Les deux échouent avec la version 0.9.0.

## 0.9.0 - 2026-10-22

Cette version ajoute le câble link distant : une console se relie à celle d'un autre appareil, deux téléphones par exemple, l'application transportant les messages. L'API C et l'API C++ s'enrichissent sans rien casser ; le format des états ne change pas.

### Ajouté

- **Câble link distant** ([docs/remote_link.md](docs/remote_link.md)).
  - API C : `ravenemu_remote_link_create`, `_receive`, `_set_connected`, `_state`, `_service`, `_destroy`. L'application livre les messages dans l'ordre et sans perte (TCP, par exemple) et branche ou débranche le câble.
  - La console qui cadence un transfert envoie sa donnée et attend la réponse de l'autre en temps réel, au cycle exact du transfert : son temps émulé ne bouge pas, le jeu ne voit aucun retard. L'autre répond quand elle lit le message, comme une console qui attendait l'horloge d'un partenaire. Les deux appliquent la même décision.
  - Game Boy et Game Boy Color : un octet par transfert, horloge normale ou rapide, collisions de deux consoles qui cadencent.
  - GBA : deux joueurs, multijoueur (le joueur 0 est le parent) et mode normal 8 ou 32 bits ; les lignes d'état SD et SI suivent celles de l'autre console.
  - Un petit protocole versionné : annonce, transfert, réponse, lignes d'état. Une autre version ou une autre famille de consoles laisse le câble sans effet.
- **`ravenemu_link_wanted()`** : le jeu se sert de son port link (transfert demandé ou attendu dans les trois dernières secondes, ou GBA en mode multijoueur). Une application peut ne chercher un partenaire qu'à ce moment.
- API C++ : `RemoteLink`, `Core::connect_remote_link()`, `Core::service_remote_link()`, `Core::link_wanted()`.

### Modifié

- GBA : le port série passe par une interface `SerialLink`, que partagent le câble local et le câble distant. Le câble local se comporte comme avant.
- `LinkPort` donne l'octet et les bits restants du transfert en cours ; `LinkEndpoint` peut lire un transport extérieur (`poll()`).
- La bibliothèque commune se lie aux fils du système (`Threads::Threads`), que le paquet CMake demande.

### Compatibilité des états instantanés

- **GBA : format 16** et **GB/GBC : format 11**, inchangés. Le câble distant n'entre pas dans l'état d'une console.
- **API C : version 0**, ajouts seulement ; **API C++** : ajouts seulement. Le paquet CMake se demande avec `find_package(RavenCore 0.9)`.

### Tests

- `gbc_raven_core` :
  - deux ports Game Boy reliés par deux câbles distants : octets échangés, interruptions, transfert sans câble ;
  - collision de deux consoles qui cadencent, partenaire qui n'attend pas, débranchement pendant une attente ;
  - protocole : annonce, refus d'une autre famille ou version, messages mal formés.
- `ravenemu_remote_link`, nouveau, par l'API C : deux consoles sur deux fils, reliées par un faux réseau à 0, 5 et 20 ms de latence.
  - Game Boy : un maître et un esclave échangent leurs octets.
  - GBA : transfert multijoueur, même résultat que le câble local.
  - Exclusion du câble local, paramètres refusés, destruction dans les deux ordres.

## 0.8.0 - 2026-10-21

Cette version complète la carte mémoire RetroAchievements de la Game Boy et de la Game Boy Color ([#29](https://github.com/GhostPunishR/RavenCore/pull/29)) et active l'horloge des ROM Pokémon modifiées ([#28](https://github.com/GhostPunishR/RavenCore/pull/28)). L'API et le format des états ne changent pas.

### Ajouté

- **Game Boy et Game Boy Color : carte mémoire RetroAchievements complète.** La banque de ROM paginée (0x4000-0x7FFF) et les registres d'E/S (0xFF00-0xFF7F) rejoignent la carte, comme dans la table de rcheevos. Un succès qui les lit n'est plus signalé non pris en charge. Cela vaut pour les cœurs Libretro et pour `read_achievement_memory()`, que RavenEmu utilise.
  - Les registres d'E/S se lisent par le même code que les lectures du CPU, sans effet de bord sur l'émulation.
  - Le détail de la carte, ses limites et ses tests sont dans [docs/retroachievements.md](docs/retroachievements.md).

### Corrigé

- **Libretro, Game Boy : première fenêtre de ROM figée.** Elle n'était copiée qu'au démarrage, au reset et au chargement d'un état. Le MBC1 en mode 1 et le MMM01 la commutent pourtant en cours de partie : un succès y lisait alors l'ancienne banque. Les deux fenêtres sont maintenant recopiées à chaque image.
- **GBA : horloge des ROM Pokémon modifiées** ([#28](https://github.com/GhostPunishR/RavenCore/pull/28)). Les pilotes compatibles CFRU (Pokémon Unbound, par exemple) peuvent omettre le marqueur de la bibliothèque Seiko : l'horloge n'était alors pas détectée. Une ROM qui porte l'identité de Rouge Feu et un groupe de références aux trois ports du pilote série active l'horloge. Le Rouge Feu d'origine, sans horloge, reste sans. Le forçage explicite garde la priorité.

### Modifié

- **Lecture de la ROM pour les succès environ 250 fois plus rapide.** Chaque contrôleur de cartouche indique la banque visible dans chaque fenêtre, et la fenêtre se copie d'un bloc. La lecture du CPU passe par ce même calcul. Lire les deux fenêtres prend 0,3 µs au lieu de 76 sur un PC. Le MBC6 et le menu du MMM01 se lisent encore octet par octet.

### Compatibilité des états instantanés

- **GBA : format 16** et **GB/GBC : format 11**, inchangés.
- **API C : version 0**, inchangée ; **API C++** inchangée. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.8)`.

### Tests

- `gb_raven_core` :
  - registres d'E/S égaux à ce qu'une ROM de test en recopie par le CPU ;
  - banque paginée choisie par un MBC5 et première fenêtre commutée par un MBC1 en mode 1.
- `gb_cartridge_mbc` : pour chaque contrôleur et chaque banque choisie, la copie d'une fenêtre est, octet pour octet, la lecture du CPU. Le test échoue si la copie oublie le décalage dans la banque.
- `libretro_smoke_gb` et `libretro_smoke_gbc` : la carte contient les deux fenêtres de ROM et les E/S, et suit un changement de banque à l'image suivante. Le test échoue si la première fenêtre n'est plus recopiée à chaque image.

## 0.7.1 - 2026-10-18

Cette version active RetroAchievements dans les cœurs Libretro ([#27](https://github.com/GhostPunishR/RavenCore/pull/27)). L'API et le format des états ne changent pas.

### Ajouté

- **Libretro : RetroAchievements.** Les trois cœurs annoncent la prise en charge des succès et déclarent à RetroArch une carte mémoire (`RETRO_ENVIRONMENT_SET_MEMORY_MAPS`), aux adresses de la console et dans la disposition de rcheevos :
  - **GBA :** IWRAM en 0x03000000, EWRAM en 0x02000000, et puce de sauvegarde en 0x0E000000 (64 Kio au plus).
  - **Game Boy et Game Boy Color :** banque 0 de la ROM, VRAM, RAM de cartouche dans toutes ses banques, WRAM (son miroir y renvoie), OAM, HRAM et IE, et banques 2 à 7 de la WRAM de la Game Boy Color.

  RetroArch garde des pointeurs vers cette mémoire pendant toute la partie, alors que RavenCore recrée sa machine à chaque reset et à chaque chargement d'état. RetroArch lit donc une copie de l'espace de RetroAchievements, rafraîchie après chaque image (quand les succès sont évalués), après un reset et après un chargement d'état. Une écriture du frontend dans cette copie n'atteint pas le jeu : les codes de triche restent non pris en charge.
  - Sur Game Boy, la banque de ROM paginée et les registres d'E/S ne sont pas exposés : un succès qui les lit est signalé comme non pris en charge. La banque 0 est copiée telle qu'elle est au démarrage, au reset et au chargement d'un état. La relire à chaque image, à travers le contrôleur de la cartouche, coûterait environ 2 % du temps d'une image.
  - Chaque région est découpée en descripteurs dont la longueur est une puissance de deux et le départ aligné sur elle : c'est ce qu'exige `libretro.h` sans masque `select`, et RetroArch rejette sinon toute la carte (OAM, banques de la WRAM GBC, RAM de cartouche). Relevé par la revue Codex.
  - Les fichiers `.info` indiquent `memory_descriptors = "true"`.

  Vérifié en local avec le code de rcheevos qui relie un cœur Libretro aux succès dans RetroArch (`rc_libretro`), compilé hors du dépôt. Il retrouve les régions GBA et Game Boy Color, y compris après le pré-traitement que RetroArch applique à la carte, et chaque octet lu par ce chemin est celui que rend le cœur. Pas encore essayé en jeu dans RetroArch.

### Modifié

- **Lecture de l'espace de RetroAchievements environ 16 fois plus rapide** (`read_achievement_memory()`, `ravenemu_read_achievement_memory()`) : elle copie la mémoire par blocs au lieu d'octet par octet. Lire tout l'espace d'un jeu GBA prend 24 µs au lieu de 390 sur un PC. Les octets rendus ne changent pas.

### Compatibilité des états instantanés

- **GBA : format 16** et **GB/GBC : format 11**, inchangés.
- **API C : version 0**, inchangée. Le paquet CMake se demande toujours avec `find_package(RavenCore 0.7)`.

### Tests

- `libretro_smoke_gb`, `libretro_smoke_gbc` et `libretro_smoke_gba` vérifient plusieurs points :
  - les succès sont annoncés et la carte mémoire est déclarée aux adresses de la console, en descripteurs que RetroArch accepte ;
  - la sauvegarde et la RAM de cartouche y apparaissent ;
  - un compteur en mémoire de travail y suit chaque image, garde son adresse, et repart d'un état rechargé comme d'un reset.
- `gb_raven_core` et `gba_raven_core` vérifient qu'une lecture qui franchit une frontière de région rend les mêmes octets que des lectures d'un octet.

## 0.7.0 - 2026-10-18

Cette version donne aux registres d'E/S et au port série GBA les valeurs que relit la console, au drapeau de H-Blank son instant, et au Halt du BIOS sa sortie ([#26](https://github.com/GhostPunishR/RavenCore/pull/26)). Le format des états GBA change ; l'API ne change pas.

### Corrigé

- **GBA : relecture des registres d'E/S.** Le CPU relisait chaque registre tel qu'il l'avait écrit. Il relit désormais ce que décrit GBATEK ; les bits inutilisés, que GBATEK ne détaille pas, ont été relevés sur console par la suite de tests de mGBA.
  - Les bits inutilisés se lisent 0 : BG0CNT et BG1CNT (bit 13), WININ, WINOUT, BLDCNT, BLDALPHA, registres du son, DMAxCNT_H, DISPSTAT (bits 6 et 7), WAITCNT (bit 15 : cartouche GBA) et JOYCNT.
  - Les parties en écriture seule aussi : durée, fréquence et départ des canaux du son, remises à zéro des FIFO, HALTCNT.
  - SOUNDCNT_X indique les canaux du son en cours, que le jeu ne peut pas écrire.
  - Un registre en écriture seule ou un emplacement inutilisé rend le bus ouvert quand tout son mot de 32 bits est illisible (défilements, matrices, fenêtres, MOSAIC, BLDY, FIFO, adresses des DMA), 0 sinon (DMAxCNT_L, 0x066…), comme le décrit GBATEK (« Reading from Unused or Write-Only I/O Ports »).
- **GBA : son éteint.** Quand le bit 7 de SOUNDCNT_X passe à 0, les registres du PSG (0x60 à 0x81) repartent de zéro, et ils ignorent ensuite les écritures jusqu'à ce que le son soit rallumé, comme le décrit GBATEK. Le réglage interne des canaux (volume, fréquence, DAC de l'onde, bruit) repart de zéro avec eux. Un canal déclenché pendant l'arrêt, ou relancé après sans nouveau réglage, ne part donc plus au rallumage. Relevé par la revue Codex.
- **GBA : relecture du port série selon son mode** (GBATEK, valeurs sans câble relevées sur console par la suite de mGBA).
  - SIODATA32 ne se lit qu'en mode normal 32 bits.
  - En multijoueur, SIOMULTI0-3 rendent les données reçues : une écriture du CPU ne les change pas, la console relit 0. L'écriture reste dans SIODATA32, qu'un passage en mode normal 32 bits retrouve.
  - SIOCNT : le bit 15 se lit 0, comme les bits 4 à 6 hors multijoueur. En UART, non émulé, il indique une FIFO de réception vide.
  - RCNT : les bits 9 à 13 se lisent 0. Hors mode GPIO, les bits 0 à 3 donnent l'état des lignes SC, SD, SI et SO.
- **GBA : drapeau de H-Blank.** Le bit 1 de DISPSTAT ne se lève qu'à 1006 cycles de la ligne, 46 après la fin du tracé (GBATEK, « DISPSTAT »). L'IRQ de H-Blank reste à 960 cycles, mais elle vient désormais à chaque ligne, VBlank compris. GBATEK se contredit sur ce point ; la console la lève pendant le VBlank, comme le relève la suite de mGBA. Le DMA de H-Blank ne part toujours qu'aux lignes visibles.
- **GBA : sortie du Halt du BIOS.** Sur console, le Halt du BIOS (SWI 2) ne rend pas la main au jeu dès le réveil : le gestionnaire d'IRQ passe, puis le BIOS sort de sa fonction. Le BIOS de RavenCore rendait la main aussitôt. Le jeu reprend désormais 27 cycles plus tard, comme sur la console : la suite de mGBA le mesure en attendant par Halt la fin d'un transfert série. Ce qui ressemblait à 27 cycles d'avance du port série, à tous les débits, venait de là.

Sur la suite de mGBA lancée en local, hors du dépôt et de la CI :
- I/O read passe de 11 à 130 cas sur 130 ;
- SIO R/W passe de 7 à 90 sur 90 ;
- SIO timing passe de 0 à 8 sur 8 ;
- Misc. edge passe de 0 à 2 sur 12.

Les autres mesures du test de H-Blank dépendent de la vitesse de ses boucles et restent à quelques cycles de la console. Les autres suites ne changent pas, et le cœur n'exécute que 0,08 % d'instructions de plus sur Pokémon Émeraude. Le câble link entre Pokémon Émeraude et Rubis fonctionne toujours (vérifié en local, ROM et sauvegardes jamais publiées).

### Compatibilité des états instantanés

- **GBA : format 16.** L'état garde l'instruction qui suit un Halt du BIOS en cours, et SIOMULTI0-3, désormais distincts de SIODATA32. Les états aux formats 11 à 15 se rechargent : un Halt en cours y rend la main dès son réveil, et SIOMULTI0-3 y reprennent la valeur rangée avec SIODATA32. Un état au format 16 ne se recharge pas dans une version 0.6.x.
- **GB/GBC : format 11**, inchangé.
- **API C : version 0**, inchangée ; **API C++** inchangée. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.7)`.

### Tests

- `gba_raven_hardware` écrit puis relit les registres d'E/S : bits inutilisés, parties en écriture seule, bus ouvert ou 0, SOUNDCNT_X. Il relit aussi le port série dans chacun de ses modes, aux valeurs relevées sur console.
- `gba_raven_timing` vérifie que le drapeau de H-Blank se lève à 1006 cycles, que l'IRQ part à 960, y compris pendant le VBlank, et qu'après un Halt du BIOS le jeu reprend 27 cycles plus tard qu'après un arrêt par HALTCNT.
- `gba_raven_core` reprend au même cycle un état pris pendant un Halt du BIOS, et garde SIOMULTI0-3 dans un état.
- `gba_raven_sensors` vérifie le format 16 et recharge un état au format 15.

## 0.6.0 - 2026-10-18

Cette version donne aux IRQ et aux timers GBA les instants de la console, et fait fonctionner le câble link entre Pokémon Émeraude et Rubis ([#25](https://github.com/GhostPunishR/RavenCore/pull/25)). Le format des états GBA change ; l'API ne change pas.

### Corrigé

- **GBA : câble link entre Pokémon Émeraude et Rubis ou Saphir.** La connexion échouait au Club Link (« Erreur de connexion »), alors que deux Émeraude se reliaient. Deux défauts du BIOS de RavenCore faisaient perdre à Rubis des IRQ série, donc le compte des transferts, si bien que ses sommes de contrôle tombaient une case trop tôt :
  - IntrWait et VBlankIntrWait gardaient le CPU arrêté jusqu'à l'IRQ attendue, sans servir les autres. Sur console, toute IRQ réveille le BIOS : le gestionnaire du jeu s'exécute, puis l'attente reprend (GBATEK, « IntrWait »). C'est désormais le cas : le gestionnaire revient sur le SWI, qui reprend l'attente sans effacer les drapeaux déjà notés.
  - Une longue fonction du BIOS (CpuSet, CpuFastSet, décompression) prenait tout son temps d'un bloc, et deux IRQ survenues pendant ce temps n'en faisaient qu'une. Le BIOS d'origine exécute ces fonctions avec les IRQ de l'appelant, qui les interrompent. RavenCore fait toujours la copie d'un coup, mais son temps s'écoule ensuite, avant l'instruction qui suit le SWI, et une IRQ peut l'interrompre. La durée totale ne change pas.

  Vérifié en local avec les ROM et sauvegardes de l'utilisateur, jamais publiées : Émeraude et Rubis entrent ensemble au Centre Troc, dans les deux ordres, et y échangent un Pokémon. Deux Émeraude se relient toujours.
- **GBA : instant où le CPU prend une IRQ.** Le CPU ne prend plus une IRQ dès la fin de l'instruction pendant laquelle elle se lève. Il attend que la ligne d'IRQ (IME, et une source à la fois dans IE et IF) soit levée depuis 4 cycles, puis entre dans l'exception, ce qui coûte 2 cycles internes de plus qu'un saut ; l'unité de préchargement de la cartouche lit pendant ces cycles. Les deux durées sont calées sur des valeurs mesurées sur console : la suite de tests de mGBA les publie, et RavenCore la lance en local, hors du dépôt et de la CI. Une IRQ de timer laisse ainsi encore passer trois NOP, comme sur la console, et son gestionnaire arrête le timer au même cycle.
- GBA : un jeu arrêté par Halt, IntrWait ou VBlankIntrWait ne reprend qu'après son gestionnaire d'IRQ. Le CPU arrêté attend que la ligne soit synchronisée avant de repartir, au lieu d'exécuter d'abord une instruction du jeu.
- GBA : le vecteur d'IRQ du BIOS est celui de la console. Le mot en 0x18 saute en 0x128, où commence le gestionnaire (GBATEK, « BIOS Interrupt Handling »). Une IRQ coûte donc aussi ce saut.
- GBA : une recharge de timer écrite dans TMxCNT_L ne sert qu'à partir du cycle suivant. Un débordement dans le cycle même de l'écriture recharge encore l'ancienne valeur. Une écriture 32 bits qui donne la recharge et démarre le timer le fait partir de la nouvelle recharge.
- GBA : quand une IRQ se lève, le BIOS de RavenCore la note en 0x03007FF8 directement en IWRAM. Il passait par le bus, ce qui comptait 2 cycles d'accès de trop dans l'instruction qui levait l'IRQ, par exemple une écriture dans IME.

Sur la suite de mGBA lancée en local, timer-irq passe de 54 échecs à 0, et timers de 561 à 349. Les échecs restants viennent d'IRQ prises coup sur coup : chaque IRQ supplémentaire y arrive environ un cycle trop tard. Les autres suites ne changent pas. Le suivi de la ligne d'IRQ et le temps interruptible du BIOS ne coûtent que 0,2 % d'instructions de plus sur Pokémon Émeraude.

### Compatibilité des états instantanés

- **GBA : format 15.** L'état garde les recharges de timer écrites mais pas encore en service, l'âge de la ligne d'IRQ, et le temps restant d'une fonction du BIOS. Un état pris pendant qu'une IRQ se synchronise, ou pendant une copie du BIOS, reprend ainsi au même cycle. Les états aux formats 11 à 14 se rechargent ; une IRQ en attente y est prise sans délai, comme avant. Un état au format 15 ne se recharge pas dans une version 0.5.x.
- **GB/GBC : format 11**, inchangé.
- **API C : version 0**, inchangée ; **API C++** inchangée. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.6)`.

### Tests

- `gba_raven_timing` vérifie qu'une IRQ levée pendant des NOP en IWRAM en laisse passer quatre, puis que l'entrée et le saut vers 0x128 prennent 5 cycles. Pendant ces cycles, l'unité de préchargement de la cartouche lit la ROM du jeu interrompu. Il vérifie aussi qu'un Halt réveillé par un timer laisse passer le gestionnaire avant le jeu, et qu'une recharge de timer ne sert qu'un cycle après son écriture, même reprise d'un état. Il contrôle que le BIOS note une IRQ sans prendre de cycles. Enfin, il vérifie qu'un timer est servi pendant VBlankIntrWait, et pendant un CpuFastSet de 16 Kio dont la durée reste celle de la copie, sauf quand le jeu masque les IRQ.
- `gba_raven_core` lève une IRQ de VCount à chaque fin de trame. Un état pris avant qu'elle ne soit synchronisée doit reprendre au même cycle. Un état pris pendant une boucle de CpuFastSet interrompue par un timer aussi.
- `gba_raven_sensor` vérifie le format 15 et recharge un état au format 14.

## 0.5.2 - 2026-10-17

Cette version donne aux fonctions ArcTan, ArcTan2 et Div du BIOS GBA les valeurs de la console ([#24](https://github.com/GhostPunishR/RavenCore/pull/24)). L'API et le format des états ne changent pas.

### Corrigé

- GBA : ArcTan et ArcTan2 du BIOS (SWI 0x09 et 0x0A) rendent les angles de la console. Le BIOS n'évalue pas l'arc tangente exacte, mais un polynôme en virgule fixe 1.14 par la méthode de Horner, sur 32 bits signés ; RavenCore calculait l'angle exact en virgule flottante et rendait donc souvent une ou deux unités de trop ou de moins. ArcTan lit désormais r0 en entier, comme le BIOS, et laisse en r1 et r3 les valeurs intermédiaires de la console ; ArcTan2 laisse le r1 d'ArcTan et 0x170 en r3.
- GBA : Div et DivArm (SWI 0x06 et 0x07) par zéro se comportent comme le BIOS d'origine, qui ne teste pas ce cas. 0/0, 1/0 et -1/0 rendent le signe du numérateur (1 pour 0), le numérateur en reste et 1 en r3. Au-delà, sa boucle d'alignement ne finit jamais : la console reste bloquée dans le BIOS, IRQ masquées, et RavenCore aussi désormais (d'après la [GBA BIOS Reference](https://retrointernals.github.io/gba-bios-reference/swi_06_div/), documentation comportementale vérifiée sur console).

### Tests

- `gba_raven_hardware` vérifie ArcTan, ArcTan2 et Div appelés par `swi`. Il contrôle des valeurs calculées à la main : ArcTan(1) vaut exactement π/4, avec le polynôme à 0x8000. Il vérifie aussi que l'approximation reste à moins de 1,4 unité de l'arc tangente exacte sur [−1, 1], et ArcTan2 à moins de 3 unités de l'angle exact sur 20 000 points. Enfin, il contrôle les quadrants, la division par zéro (résultats de 0/0, 1/0 et -1/0, blocage au-delà) et le débordement de -0x80000000 / -1.

## 0.5.1 - 2026-10-17

Cette version donne au drapeau C des multiplications GBA la valeur du matériel et ajoute une batterie de cycles CPU tirée de GBATEK ([#23](https://github.com/GhostPunishR/RavenCore/pull/23)). L'API et le format des états ne changent pas.

### Corrigé

- GBA : après MULS, MLAS, UMULLS, UMLALS, SMULLS, SMLALS et le MUL Thumb, le drapeau C n'est plus laissé inchangé. Il prend la valeur que produit l'ARM7TDMI : un bit de la rangée des retenues de son multiplieur de Booth, simulé rangée par rangée (`cpu/booth_multiplier.hpp`). Le modèle matériel vient de l'analyse publiée par zayd en 2024 et des brevets ARM qu'elle cite ; le code est écrit pour RavenCore. Il ne tourne que pour les multiplications qui modifient les drapeaux, et ne change ni les résultats ni les cycles.

### Tests

- `gba_raven_cycles` mesure les cycles CPU de 41 instructions ou courts blocs ARM et Thumb (ALU, multiplications, accès à l'IWRAM, à l'EWRAM et à la ROM, sauts, bug du préchargement désactivé, frontière des 128 Kio de la cartouche), plus l'écart lu sur le timer 0 entre deux lectures. Chaque valeur attendue est une somme tirée des tables de GBATEK. Le test s'exécute dans la CI, sans ROM externe.
- `gba_raven_cpu` vérifie que les rangées du multiplieur de Booth somment au produit exact, accumulateur compris, sur 60 000 opérandes des six formes. Il vérifie aussi que la boucle compte le m de GBATEK, et que MUL sur quatre cycles lève C exactement quand les deux bits hauts de Rs valent 10. Enfin, il contrôle le drapeau C laissé par chaque forme exécutée sur le CPU.

## 0.5.0 - 2026-10-17

Cette version émule le câble link GBA et le tampon de préchargement de la cartouche ([#22](https://github.com/GhostPunishR/RavenCore/pull/22)), et compte le temps du processeur GBA au cycle près. Le format des états GBA change ; l'API C gagne le câble link sans rien retirer.

### Ajouté

- **Câble link entre consoles d'une même application.** `connect_link_cable()` en C++, `ravenemu_link_create()` et `ravenemu_link_run_frame()` dans l'API C relient deux Game Boy ou Game Boy Color (mélangées si besoin), ou deux à quatre GBA. Les consoles reliées avancent ensemble, une image chacune par appel, à tour de rôle par tranches : une ligne (456 cycles, moins qu'un bit à 8 kHz) sur Game Boy, de sorte que l'autre console est prête avant l'arrivée des bits et répond à temps. Usage visé : deux jeux côte à côte sur un même appareil, pour échanger ou se battre.
- **GBA : câble link, modes multijoueur et normal** (GBATEK, « SIO Multi-Player Mode » et « SIO Normal Mode »).
  - **Multijoueur** : la console parente (la première) lance le transfert ; chaque console reçoit les quatre valeurs dans SIOMULTI0 à 3, 0xFFFF pour une place vide, puis son interruption. SIOCNT indique la console parente (SI), les consoles prêtes (SD) et le numéro de chaque console. La durée d'un transfert suit les mesures sur console publiées par mGBA, selon le débit et le nombre de consoles : à 115200 bauds, 3140 cycles seule, 5755 à deux.
  - **Normal 8 et 32 bits** : la console à horloge interne cadence celle qui attend avec une horloge externe, et toutes deux échangent leurs données. SI suit la ligne SO de l'autre console, basse quand elle attend.
  - **Cadence** : les consoles avancent par tranches de 1024 cycles, à tour de rôle, celle qui a lancé le dernier transfert en premier. Un transfert multijoueur dure plus qu'une tranche : chaque console lit les autres et reçoit le résultat au cycle exact de sa propre horloge. Un transfert en mode normal plus court qu'une tranche lit l'autre console là où elle en est. Chaque transfert porte un numéro : une console ne reçoit que le résultat de celui qu'elle attend, même quand plusieurs se suivent de près. Débranché, le câble rend à chaque console les bits d'état de SIOCNT d'une console seule.
  - **Sans câble**, une console en multijoueur se lit désormais enfant, SI tiré à 1 comme sur la console ; elle termine toujours seule un transfert qu'elle lance.
- **GBA : tampon de préchargement de la cartouche** (WAITCNT bit 14, GBATEK « GamePak Prefetch »). Pendant que le CPU n'utilise pas le bus de la cartouche (cycles internes, accès à la RAM ou aux registres), une unité lit d'avance jusqu'à huit demi-mots de code à la suite de la dernière instruction lue en ROM. Une instruction déjà lue coûte alors 1 cycle, celle en cours de lecture le reste de sa lecture. Un saut vide le tampon ; un accès de données à la cartouche arrête l'unité, et attend d'un cycle s'il tombe sur le dernier cycle d'un demi-mot en cours. Le tampon remplace l'approximation précédente, qui rendait gratuite toute lecture séquentielle en ROM dès que le préchargement était activé, comme le font presque tous les jeux du commerce : leur code tourne désormais à la cadence de la console.

### Corrigé

- **GBA : durée des instructions.** Chaque accès au bus coûte désormais 1 cycle plus ses attentes, et une instruction n'y ajoute que ses cycles internes. Les chargements simples (LDR, LDRH, LDRB, LDRSB, LDRSH, sauf LDR relatif à PC en Thumb), POP et LDMIA en Thumb retrouvent le cycle interne qui leur manquait ; STM en ARM perd le cycle de trop qu'il comptait. Le premier accès de chaque bloc de 128 Kio de la cartouche est non séquentiel.
- **GBA : préchargement désactivé.** Sans tampon, l'instruction qui suit un cycle interne en ROM est relue hors séquence (GBATEK, « Prefetch Disable Bug »).
- **GBA : timers et registres d'E/S au cycle près.** Un timer compte à partir de 2 cycles après l'écriture qui l'active. Les périphériques sont avancés jusqu'au cycle exact d'un accès aux registres, au milieu de l'instruction, et non plus à sa fin.
- **GBA : registres d'E/S répétés dans toute leur région.** Seuls 0x04000000 à 0x040003FF répondent ; 0x04xx0800 rend la valeur du contrôle mémoire (0x0D000020). Ailleurs, une lecture rend le bus ouvert et une écriture se perd. Une trace de débogage écrite à 0x04FFF600, comme celle de mGBA, écrasait sinon le registre IE et bloquait le programme en attente d'interruption.

Mesures avec la suite de test de mGBA (licence MIT, exécutée en local) : les échecs de la suite « Timing » passent de 785 à 340, ceux de « Timers » de 591 à 561 et ceux de « Timer IRQ » de 90 à 54. Tous les échecs restants de la suite Timing concernent la durée des appels du BIOS et des DMA, qui ne sont pas encore comptées au cycle près.

### Modifié

- **GBA : environ 5 % plus lent** sur Pokémon Émeraude (3,30 ms → 3,50 ms par image sur un PC), le prix du tampon de préchargement. Grand Theft Auto et Zelda gardent la même vitesse. La fenêtre de lecture d'instructions reste désormais ouverte après un saut ou un accès de données dans la même zone, ce qui compense une bonne partie du surcoût.

### Compatibilité des états instantanés

- **GBA : format 14.** L'état garde le tampon de préchargement, le démarrage des timers et un transfert en cours sur le câble link. Les états au format 13 (versions 0.4.x) se rechargent, tampon vide. Un état au format 14 ne se recharge pas dans une version 0.4.x. L'état d'une console reliée ne contient qu'elle : un transfert en cours avec une autre console, rechargé alors que le câble est passé à un autre transfert, se termine comme sans câble, sans se mêler au transfert suivant.
- **GB/GBC : format 11**, inchangé.
- **API C : version 0**, inchangée : les fonctions du câble link s'ajoutent sans rien changer aux autres. **API C++** : `Core` gagne la méthode virtuelle `create_link_cable()`, ce qui change sa table virtuelle ; recompiler le code qui dérive de `Core`. Le paquet CMake se demande désormais avec `find_package(RavenCore 0.5)`.

## 0.4.1 - 2026-10-16

Cette version corrige le redémarrage des DMA GBA ([#21](https://github.com/GhostPunishR/RavenCore/pull/21)), qui laissait GTA Advance sur un écran noir au début d'une nouvelle partie, garde l'heure réglée par un jeu GBA dans sa sauvegarde et accélère le cœur GBA. L'API et le format des états ne changent pas ; la sauvegarde d'un jeu GBA à horloge gagne 16 octets.

### Corrigé

- **GBA : réécriture du contrôle d'un DMA déjà actif.** Un canal ne recharge ses adresses et ne lance un transfert immédiat que quand le bit 15 de son contrôle passe de 0 à 1, comme le décrit GBATEK. Le cœur relançait le canal à chaque écriture où ce bit valait 1. GTA Advance arrête son DMA de son en réécrivant `0xB600` en `0x8400`, bit 15 gardé, avant d'effacer ce bit : le cœur lançait alors 0x4000 mots depuis FIFO A à travers tous les registres d'E/S, et le jeu restait sur un écran noir après le choix du nom. Une réécriture garde désormais le canal en l'état et ne change que ses réglages.
- **GBA : réglages de l'horloge de la cartouche perdus.** L'heure réglée par un jeu et le registre d'état de l'horloge n'étaient gardés que dans les états instantanés : au chargement suivant de la ROM, comme après un reset, l'horloge revenait à l'heure de l'appareil. Ils suivent désormais la sauvegarde, dans le bloc de 16 octets que mGBA place après la mémoire de la puce : date et heure du jeu, registre d'état, puis heure de l'appareil au même instant. Un réglage changé par le jeu marque la sauvegarde à réécrire, et le reset reprend les réglages en cours.
- **GB/GBC et GBA : sauvegarde perdue après un reset.** Le reset gardait la mémoire de sauvegarde mais la marquait comme enregistrée, même quand le frontend ne l'avait pas encore écrite. Sauvegarder dans le jeu, faire un reset avant l'écriture automatique puis quitter perdait donc la sauvegarde. Une écriture en attente reste désormais à enregistrer. Le compteur de générations continue au lieu de repartir de zéro : une génération rendue avant le reset ne peut plus acquitter une écriture qui le suit.

### Modifié

- **GBA : environ 12 % plus rapide** sur Pokémon Émeraude (3,76 ms → 3,30 ms par image sur un PC). Le bus cherchait le type de la puce de sauvegarde par `dynamic_cast` à chaque accès 16 bits ; il la classe désormais une fois, à la création de la cartouche. Le résultat de l'émulation ne change pas.
- Code : décompressions du BIOS GBA (BitUnPack, LZ77, Huffman, RLE, filtres différentiels) dans `bios/decompression.hpp`, avec un test pour chacune ; fichiers les plus denses reformatés à une instruction par ligne (`.clang-format`). Ni le comportement ni le code compilé du formatage ne changent.

### Compatibilité des états instantanés

- **GBA : format 13** et **GB/GBC : format 11**, inchangés : les états créés avec la version 0.4.0 se rechargent. L'état d'activation de chaque canal DMA se déduit des registres d'E/S restaurés.
- **API C : version 0**, inchangée. Le paquet CMake se demande toujours avec `find_package(RavenCore 0.4)`.

### Compatibilité des sauvegardes

- **GBA, jeux à horloge** (Pokémon Rubis, Saphir et Émeraude, Boktai, etc.) : la sauvegarde rendue par `snapshot_battery_ram()`, `ravenemu_copy_battery_generation()` et le `SAVE_RAM` de Libretro est la mémoire de la puce suivie du bloc de 16 octets de mGBA. Une sauvegarde sans ce bloc, d'une version précédente ou d'un autre émulateur, se charge comme avant, horloge à l'heure de l'appareil. Les versions précédentes de RavenCore et mGBA relisent les nouvelles sauvegardes.
- Les autres jeux GBA et tous les jeux GB/GBC gardent exactement la même sauvegarde.

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
