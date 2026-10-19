# RavenNDS — cœur expérimental

La cible C++20 `nds_raven_core` prépare le futur moteur Nintendo DS.
Elle expose `ravenemu::nds::Cartridge`, `Memory`, `Cpu`, `CpuState` et `Machine`, dans
`<ravenemu/nds/{cartridge,memory,cpu,machine}.hpp>` ; elle est aussi liée par `RavenCore::ravencore`.
L'état et les modes CPU sont déclarés dans `<ravenemu/nds/cpu_state.hpp>`.
Les premiers instantanés vidéo RGB555 sont déclarés dans `<ravenemu/nds/video.hpp>`.

## Organisation

```text
nds/
├── include/ravenemu/nds/  # contrats publics
├── src/
│   ├── bios/             # images et fenêtres BIOS
│   ├── cartridge/        # cartouches .nds
│   ├── cpu/              # ARM, Thumb, état, PSR et exceptions
│   ├── dma/              # canaux, transferts immédiats et VBlank/HBlank
│   ├── interrupts/       # contrôleurs IME/IE/IF indépendants
│   ├── ipc/              # synchronisation et FIFO ARM9/ARM7
│   ├── machine/          # assemblage des deux CPU et chargement
│   ├── memory/           # bus RAM, miroirs et WRAM
│   ├── timers/           # compteurs, préscalers et cascade
│   └── video/            # horloge LCD, VRAM, fonds, sprites, fenêtres et effets A/B
└── tests/                # programmes et BIOS synthétiques
```

Les en-têtes installés restent dans `include/ravenemu/nds/` ; les helpers des
décodeurs CPU restent privés. Cette page centralise la documentation NDS.

## Cartouches et tests

Le chargeur lit le titre, le code du jeu et les descripteurs ARM9/ARM7 en
little-endian, possède les octets de la ROM et fournit les deux exécutables.
Il refuse les en-têtes et programmes tronqués, les tailles nulles, les plages
débordantes et les entrées hors du programme. Le bit Thumb de l'entrée est
conservé. Les images DSi sont explicitement refusées.

Référence du format : [GBATEK, DS Cartridge Header](https://problemkaputt.de/gbatek.htm#dscartridgeheader).
Cette validation est structurelle : CRC, logo, signatures et chiffrement KEY1
ne sont pas encore vérifiés. Une image acceptée n'est pas nécessairement
exécutable. Les tests construisent leurs données synthétiques en mémoire,
sans ROM ni BIOS tiers.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build -R '^nds_' --output-on-failure
```

## Mémoire et chargement direct

`Memory` distingue les accès `Processor::arm9` et `Processor::arm7` :

| Région | Taille physique | Fenêtre et miroirs |
| --- | --- | --- |
| RAM principale commune | 4 Mio | `0x02000000–0x02ffffff` |
| WRAM partagée | 32 Kio | ARM9 : `0x03000000–0x03ffffff` ; ARM7 : `0x03000000–0x037fffff` |
| WRAM privée ARM7 | 64 Kio | `0x03800000–0x03ffffff` ; aussi dans la fenêtre basse ARM7 en mode 0 |

`set_wram_control()` garde les deux bits WRAMCNT : mode 0, tout à ARM9 ;
mode 1, moitié haute à ARM9 et basse à ARM7 ; mode 2, inverse ; mode 3,
tout à ARM7. La réattribution préserve les octets. Les transferts 8/16/32 bits
sont little-endian ; ceux de 16/32 bits doivent être alignés.
Référence : GBATEK, DS Memory Maps et WRAMCNT.

```cpp
ravenemu::nds::Cartridge cartridge{rom_bytes};
ravenemu::nds::Machine machine;
machine.load_cartridge(cartridge);
auto arm9_entry = machine.entries()->arm9; // adresse et bit Thumb séparés
auto instruction = arm9_entry.thumb
    ? machine.memory().read16(ravenemu::nds::Processor::arm9, arm9_entry.address)
    : machine.memory().read32(ravenemu::nds::Processor::arm9, arm9_entry.address);
```

`Machine::load_cartridge()` prépare une RAM neuve, donne toute la WRAM partagée
à ARM7, copie les deux programmes et initialise les deux CPU à leurs points d'entrée. Il accepte
ARM9 dans les 4 Mio canoniques de RAM principale, ARM7 dans cette même RAM,
les 32 Kio canoniques de WRAM partagée ou les 64 Kio canoniques privés. Il refuse
les miroirs comme destinations, les franchissements de région, les programmes
qui se chevauchent et les entrées mal alignées ou dont la première instruction
est tronquée. Un échec préserve mémoire, répartition WRAM, entrées et états CPU précédents.
Un nouveau chargement réussi efface le contenu antérieur de toutes les RAM et
réinitialise registres, banques, drapeaux, compteurs et lignes IRQ/FIQ. Les deux
CPU démarrent en System avec I/F masqués. Les images BIOS déjà fournies sont conservées.

Ce chargement est une préparation expérimentale, pas une séquence de démarrage
BIOS : pas de firmware, de déchiffrement, de paramètres de démarrage, de copie
de l'en-tête dans la RAM ni d'initialisation complète des registres CPU. Seuls les
MMIO LCD, interruption, DMA, timers, IPC, VRAMCNT et WRAMCNT/WRAMSTAT sont présents.
Les neuf banques VRAM ont des fenêtres CPU réaffectables ; palettes et OAM
standard des deux moteurs sont présentes. TCM, autres MMIO et
comportements open-bus restent absents ; les accès non mappés lèvent `ravenemu::Error`. Les lectures ARM
non alignées et leurs rotations ne sont pas encore prises en charge par le CPU.

## Interpréteur ARM/Thumb

Chaque CPU possède ses 16 registres et drapeaux NZCV. `CpuState::registers[15]`
désigne la prochaine instruction à lire ; les instructions qui lisent le PC
voient PC + 8 en ARM et PC + 4 en Thumb. `BX` sélectionne le mode par le bit 0
de l'adresse. `Cpu::reset()` réinitialise registres, drapeaux et compteur, puis
fixe l'adresse et le mode. Ce n'est pas le reset matériel d'un ARM946E-S.

```cpp
machine.run_steps(ravenemu::nds::Processor::arm9, 100);
machine.run_steps(ravenemu::nds::Processor::arm7, 100);
const auto& arm7 = machine.cpu_state(ravenemu::nds::Processor::arm7);
// arm7.registers, arm7.thumb, arm7.instructions, arm7.zero, etc.
```

Ces appels exécutent le nombre demandé de pas sur le CPU sélectionné,
sans faire avancer l'autre. Le budget est borné même sur une boucle infinie.
Un pas transfère une unité DMA, entre en IRQ/FIQ ou exécute une instruction.
Un DMA déclenché en attente est prioritaire sur les interruptions et suspend
uniquement le CPU associé ; l'autre CPU n'avance que par ses propres appels.
Le compteur `instructions`
compte uniquement les instructions terminées, dont SWI, UDF et les conditions
ARM fausses. Une unité DMA ou entrée IRQ/FIQ ne l'incrémente pas. Il n'y a pas
encore d'ordonnanceur, de calcul des cycles CPU ou de rapport de fréquence ARM9/ARM7.
Les timers et le balayage LCD disposent d'une horloge explicite commune, décrite ci-dessous.

| Mode | Instructions actuellement prises en charge |
| --- | --- |
| ARM | Les 16 opérations de traitement de données (AND à MVN), opérande immédiate tournée ou registre décalé par constante ou registre ; conditions EQ à AL ; B, BL, BX ; LDR/STR et LDRB/STRB à décalage immédiat ou registre décalé par constante, pré/post-indexés ; LDRH/STRH/LDRSB/LDRSH à décalage immédiat ou registre ; LDM/STM IA/IB/DA/DB ; MUL/MLA et UMULL/UMLAL/SMULL/SMLAL |
| Thumb | Décalages immédiats ; ADD/SUB registre ou immédiat ; MOV/CMP/ADD/SUB immédiats ; les 16 opérations ALU sur registres bas, dont décalages par registre et MUL ; ADD/CMP/MOV sur registres hauts, BX ; LDR littéral ; LDR/STR octet, demi-mot et mot à décalage immédiat ou registre, LDRSB/LDRSH à offset registre, transferts relatifs à SP ; branches conditionnelles, B et BL ; PUSH/POP, LDMIA/STMIA, ADD relatif à PC/SP et ajustement de SP |
| ARM9 uniquement | BLX par registre en ARM/Thumb et par immédiat en ARM/Thumb ; chargement de PC avec échange ARM/Thumb |

Les stockages de PC, les writebacks avec base dans la liste transférée (sauf
LDMIA Thumb, qui préserve la base chargée), les listes vides, les variantes
LDM/STM avec transfert des banques User et les autres variantes absentes sont refusés explicitement.
LDM avec PC et bit S est pris en charge pour le retour d'exception.
Les décalages ALU ARM par registre ne prennent pas encore PC comme opérande,
quantité de décalage ou destination : leurs règles de pipeline demandent une
prise en charge distincte. LDRD/STRD et SWP restent absents. Les transferts non
alignés sont refusés, y compris LDRSH sur une adresse impaire.

Les produits ARM 32 bits sont calculés modulo 2^32, les accumulations longues
modulo 2^64. SMULL/SMLAL traitent les deux sources comme des entiers signés sur
32 bits. Avec S, N/Z décrivent le résultat complet (64 bits pour les produits
longs) ; sans S, NZCV est conservé. C/V sont conservés par toutes les
multiplications dans ce modèle, comme par MUL Thumb : cela ne reproduit pas les
retenues indéterminées du multiplieur ARM7. Ses restrictions de recouvrement
avec Rm sont refusées sur ARM7 et permises sur ARM9, en lisant toutes les
sources avant de modifier les destinations. PC et deux destinations longues
identiques sont refusés. Il n'y a toujours pas de calcul des cycles CPU.
CP15, TCM, caches et une partie des instructions propres à ARMv5 restent à développer.
Ce sous-ensemble commun ne constitue pas encore un
ARM946E-S ou un ARM7TDMI complet. L'interpréteur lit directement la RAM à chaque
pas ; il ne reproduit pas le pipeline ni le comportement du code automodifiant.

`BL` Thumb se déroule en deux pas : le préfixe place la partie haute signée de
la cible dans LR, puis le suffixe branche et fixe l'adresse de retour Thumb.
Un arrêt entre les deux conserve ce LR intermédiaire. Les formes `BLX` ARMv5
sont disponibles uniquement sur ARM9 ; elles lèvent une erreur sur ARM7.
`LDR pc`, `LDM ...,{pc}` et `POP {pc}` échangent ARM/Thumb selon le bit 0 de
l'adresse sur ARM9, tandis qu'ARM7 conserve son mode courant. Les adresses
sont alignées selon le mode obtenu. Hors retours d'exception, les calculs de
PC, LR, SP et les transferts ne modifient pas NZCV. Référence : ARM Architecture Reference Manual, ARMv4T /
ARMv5T, descriptions de BL, BLX, LoadWritePC, LDM/STM et PUSH/POP.

## Modes privilégiés et exceptions

User et System partagent leurs registres. SVC, IRQ, FIQ, Abort et Undefined
ont leurs propres SP/LR et SPSR ; FIQ possède aussi r8-r12. `MRS/MSR` permettent
de lire et d'écrire les champs implémentés de CPSR/SPSR. En User, MSR ne peut
modifier que les drapeaux. Un mode réservé, un accès à SPSR en User/System ou
un changement de T par MSR est refusé. Q est conservé sur ARM9 ; les instructions
DSP qui le mettent à jour restent absentes.

SWI et les instructions indéfinies explicites (`0xe7f000f0` en ARM et `0xdeXX`
en Thumb) sauvegardent le CPSR, passent en ARM et branchent sur les vecteurs.
Les autres instructions absentes restent des erreurs de diagnostic : une
instruction légale encore manquante n'est pas assimilée à UDF. Les retours
`MOVS pc,lr`, `SUBS pc,lr,#4` et `LDM ...,{pc}^` restaurent CPSR et banques.
L'alignement de PC vient alors de T dans SPSR, pas du bit 0 de l'adresse chargée.

```cpp
machine.set_interrupt_lines(ravenemu::nds::Processor::arm9, true); // ligne IRQ
machine.step(ravenemu::nds::Processor::arm9);
```

IRQ/FIQ sont des entrées externes de niveau, échantillonnées à chaque pas hors DMA :
FIQ démasquée est prioritaire sur IRQ. I/F masquent leur prise en compte, et une
ligne restée active peut provoquer une nouvelle entrée après le retour. L'IRQ
du contrôleur MMIO est combinée à la ligne IRQ externe ; FIQ reste une entrée
externe. Le masque I du CPSR est indépendant d'IME et IE.

| MMIO | Accès | Comportement |
| --- | --- | --- |
| `0x04000208` IME | 32 bits | Seul bit 0 actif : autorisation globale des IRQ |
| `0x04000210` IE | 32 bits | Masque des sources autorisées, indépendant pour chaque CPU |
| `0x04000214` IF | 32 bits | Demandes mémorisées ; écrire 1 acquitte le bit, écrire 0 le conserve |
| `0x04000247` WRAMCNT | 8 bits ARM9 | Attribution des deux banques WRAM, bits 0–1 |
| `0x04000241` WRAMSTAT | 8 bits ARM7 | Lecture de l'attribution WRAM ; écritures ignorées |

Les accès alignés par octet ou demi-mot aux registres IRQ respectent les voies
little-endian ; les octets réservés d'IME se lisent zéro. Les demandes restent
dans IF lorsque IME ou IE sont désactivés. La ligne IRQ est active si
`IME && (IE & IF) != 0`. Les masques de sources NDS du modèle sont `0x003f3f7f`
sur ARM9 et `0x019f3fff` sur ARM7 : géométrie sur ARM9, RTC/capot/SPI/Wi-Fi sur
ARM7 ; les sources DSi ne sont pas implémentées.

Les périphériques émettent leurs événements par
`Memory::request_interrupt(cpu, sources)`, où `sources` est un masque IF.
Cette méthode sert aussi aux tests ; les timers alimentent IF lors de leur
avancement explicite, l'IPC lors des échanges et les DMA à leur fin de transfert.
Le LCD fournit les IRQ de balayage ; les moteurs A/B composent leurs fonds
et sprites sous forme d'instantanés RGB555.
Le CPU consulte la ligne du bus à chaque pas hors DMA :
un gestionnaire qui acquitte IF supprime la demande avant le retour. Référence :
[GBATEK, DS Interrupts](https://mgba-emu.github.io/gbatek/ds.html#dsinterrupts).

## DMA

Chaque CPU possède quatre canaux, décrits par `DmaController` dans
`<ravenemu/nds/dma.hpp>`. Pour le canal `n` (0 à 3), les registres 32 bits
SAD, DAD et CNT sont à `0x040000b0 + 12*n`, puis `+4` et `+8`.
Ils acceptent les accès 8/16/32 bits par voies little-endian.

| Bits de CNT | Fonction |
| --- | --- |
| 0–20 ARM9 | Nombre d'unités, zéro signifie `0x200000` |
| 0–13 ARM7 canaux 0–2 ; 0–15 canal 3 | Nombre d'unités, zéro signifie `0x4000` ou `0x10000` |
| 21–22 | Destination : incrément, décrément, fixe, incrément/rechargement |
| 23–24 | Source : incrément, décrément, fixe ; valeur 3 réservée |
| 25 | Répétition, sans répétition automatique en immédiat |
| 26 | Taille : 0 = 16 bits, 1 = 32 bits |
| 27–29 ARM9 ; 28–29 ARM7 | Déclenchement, 0 = immédiat ; bit 27 ignoré sur ARM7 |
| 30 | IRQ de fin, IF bit `8+n` |
| 31 | Activation ; le passage 0 vers 1 charge les pointeurs et le compteur internes |

ARM9 utilise 28 bits d'adresse sur tous les canaux. ARM7 utilise 27 bits
pour SAD du canal 0 et DAD des canaux 0–2, 28 bits ailleurs. Le bit d'adresse
0 est ignoré ; les pointeurs internes 32 bits ignorent aussi le bit 1.
Les compteurs et adresses internes évoluent séparément : SAD, DAD et la
longueur programmée restent lisibles sans modification pendant le transfert.
La dernière unité demande l'IRQ si elle est autorisée et efface enable,
sauf pour les blocs répétés VBlank/HBlank qui restent armés.
Un arrêt logiciel annule le reste sans IRQ de fin.

```cpp
using ravenemu::nds::Processor;
auto& bus = machine.memory();
bus.write32(Processor::arm9, 0x040000b0, 0x02001000); // source RAM
bus.write32(Processor::arm9, 0x040000b4, 0x02002000); // destination RAM
bus.write32(Processor::arm9, 0x040000b8, 0xc4000003); // trois mots, immédiat, IRQ
machine.run_steps(Processor::arm9, 3); // trois unités DMA, aucune instruction CPU
```

`Memory::dma_pending(cpu)` indique un transfert déclenché en attente.
`Memory::step_dma(cpu)` transfère une unité ou renvoie false sans effet.
`Cpu::step()` et `Machine::step()` le font automatiquement avant l'exécution
CPU. Le canal 0 est prioritaire, puis 1, 2 et 3 ; un canal prioritaire activé
entre deux unités suspend le canal inférieur, qui conserve sa progression.
Les copies chevauchantes lisent la RAM à chaque unité, sans copie préalable
du bloc source. Les masques et miroirs RAM propres à chaque CPU s'appliquent.

Le moteur accepte RAM, VRAM affectée au CPU, palettes et OAM comme sources ou destinations,
ainsi que DMAFILL ARM9 en source. Les quatre registres
DMAFILL à `0x040000e0–0x040000ef` fournissent 16 octets lisibles et modifiables
sur ARM9, utilisables comme source fixe. Les autres MMIO, BIOS, TCM et
cartouches ne sont pas des extrémités DMA prises en charge. Un accès refusé
ou le mode source réservé lève `DmaTransferError`, avec CPU, canal, adresses
et compteur restant. Les deux extrémités sont validées avant chaque unité :
une unité fautive ne modifie ni RAM ni progression ni IRQ ; les unités
précédemment terminées restent appliquées.

Le modèle reste sans cycles ni arbitrage temporel du bus. Les écritures aux
registres d'un canal déjà actif sont conservées pour sa prochaine activation ;
elles ne rechargent pas le transfert en cours. Une destination incrément/rechargement
agit comme incrément en immédiat. VBlank (mode 1 sur les deux CPU) et HBlank
(mode 2 ARM9, lignes visibles uniquement) sont déclenchés par l'horloge LCD.
Les modes cartouche, géométrie et autres modes vidéo restent armés sans déclencheur.

Avec repeat en VBlank/HBlank, chaque événement démarre un bloc de la longueur
programmée. La source continue d'un bloc à l'autre ; la destination continue
aussi, sauf en mode incrément/rechargement où elle reprend DAD. La longueur et
le DAD de rechargement sont relus au prochain déclenchement. Un événement reçu
pendant qu'un bloc est en attente ou en cours est ignoré, sans file de reprises.
Repeat en immédiat conserve le comportement ponctuel.

Les pas DMA n'avancent pas l'horloge explicite timers/LCD. Copier une machine
copie aussi les canaux suspendus ; un chargement réussi ou `clear_ram()` remet
canaux et DMAFILL à zéro. Référence :
[GBATEK, DS DMA Transfers](https://mgba-emu.github.io/gbatek/ds.html#dsdmatransfers).

## Horloge LCD

`LcdTiming`, dans `<ravenemu/nds/lcd_timing.hpp>`, suit le balayage commun aux
deux écrans : 256 pixels visibles par ligne, 192 lignes visibles, 355 périodes
de pixel par ligne et 263 lignes par image. Un pixel dure six cycles de
l'horloge à `timer_clock_hz`, soit 2130 cycles par ligne et 560190 par image.
Le modèle démarre à la ligne zéro, cycle zéro. Cette horloge n'effectue pas
le rendu : `render_frame()` fournit séparément un instantané des moteurs A/B.

| Registre | Adresse | Comportement |
| --- | --- | --- |
| DISPSTAT | `0x04000004` | Contrôles indépendants ARM9/ARM7, drapeaux issus du balayage commun |
| VCOUNT | `0x04000006` | Ligne courante, 9 bits, de 0 à 262 |

Dans DISPSTAT, les bits 0/1/2 sont les drapeaux VBlank, HBlank et égalité
VCOUNT/LYC. Les bits 3/4/5 autorisent les IRQ correspondantes (IF bits 0/1/2).
LYC utilise les bits 8–15 pour ses huit bits bas et le bit 7 pour son bit haut ;
le bit 6 reste zéro. Une comparaison supérieure à 262 ne correspond à aucune
ligne. Les drapeaux sont en lecture seule. Les accès 8/16/32 bits alignés à
DISPSTAT/VCOUNT respectent les voies little-endian.

VBlank commence à la ligne 192 et son drapeau reste actif jusqu'à la ligne 261 ;
il est déjà retombé à la ligne 262. Les drapeaux HBlank deviennent visibles à
1606 cycles sur ARM9 et 1613 sur ARM7, puis retombent à la ligne suivante.
Pour ce premier modèle, l'événement HBlank IRQ/DMA arrive à la fin des 1536
cycles de zone visible, distincte des drapeaux retardés. L'IRQ HBlank existe
sur toutes les lignes, tandis que le DMA HBlank ARM9 ne reçoit que les lignes
0 à 191. Les IRQ VBlank et VCOUNT sont émises à l'entrée de leur ligne ;
acquitter IF pendant un drapeau actif ne réaffirme pas la demande.
Modifier LYC recalcule immédiatement le drapeau, sans créer d'entrée de ligne.

```cpp
using ravenemu::nds::Processor;
machine.memory().write16(Processor::arm9, 0x04000004, 8); // autoriser IRQ VBlank
machine.advance_cycles(192 * ravenemu::nds::LcdTiming::line_cycles);
// VCOUNT vaut 192, IF bit 0 est mémorisé et les DMA VBlank armés sont déclenchés.
```

`Memory::advance_cycles()` et `Machine::advance_cycles()` avancent ensemble
LCD et timers, sans exécuter de CPU ni d'unité DMA. Les événements sont coalescés
sur l'intervalle demandé : un grand appel peut traverser plusieurs images,
sans boucle par cycle ni par ligne, mais il n'exécute pas les répétitions DMA.
Pour traiter un bloc à chaque HBlank/VBlank, alterner les avances jusqu'aux
événements et les pas CPU/DMA. `step()` et `run_steps()` conservent leur budget
de pas sans durée matérielle ; les latences fines IRQ/DMA et l'ordonnanceur
CPU restent à développer.

Les écritures VCOUNT sont ignorées dans ce modèle : la resynchronisation
par écriture disponible sur NDS n'est pas encore prise en charge. Les ports
de statut du moteur B ne sont pas des registres séparés. Un chargement
réussi ou `clear_ram()` remet le balayage et les contrôles à zéro ; une copie
de machine conserve une horloge LCD indépendante. Référence :
[GBATEK, DS Video Stuff](https://mgba-emu.github.io/gbatek/ds.html#dsvideostuff).

## VRAM et sortie vidéo

`VideoMemory` possède les 656 Kio des neuf banques, conservés lors des
changements d'affectation. Les registres VRAMCNT ARM9 sont accessibles par
octet, avec masques propres aux banques ; les accès alignés 16/32 bits suivent
les voies consécutives, y compris WRAMCNT intercalé à `0x04000247`.

| Banque | Taille | VRAMCNT | Adresse en mode LCDC ARM9 |
| --- | --- | --- | --- |
| A | 128 Kio | `0x04000240` | `0x06800000` |
| B | 128 Kio | `0x04000241` | `0x06820000` |
| C | 128 Kio | `0x04000242` | `0x06840000` |
| D | 128 Kio | `0x04000243` | `0x06860000` |
| E | 64 Kio | `0x04000244` | `0x06880000` |
| F | 16 Kio | `0x04000245` | `0x06890000` |
| G | 16 Kio | `0x04000246` | `0x06894000` |
| H | 32 Kio | `0x04000248` | `0x06898000` |
| I | 16 Kio | `0x04000249` | `0x068a0000` |

VRAMCNT bit 7 active la banque ; MST et OFS choisissent son affectation.
Le mode LCDC (MST 0) utilise les adresses du tableau. Les affectations BG/OBJ
ARM9 suivent GBATEK : fenêtres principales `0x06000000`/`0x06400000`, secondaires
`0x06200000`/`0x06600000`, avec offsets propres aux banques. Les textures et
palettes étendues conservent leurs octets mais n'ont pas de fenêtre CPU.
Les miroirs supplémentaires de VRAM ne sont pas encore modélisés.

C/D peuvent être allouées comme RAM de travail ARM7 (MST 2), à `0x06000000`
ou `0x06020000` selon OFS. `VRAMSTAT` ARM7 à `0x04000240` rapporte leur
allocation dans les bits 0/1, indépendamment d'OFS ; ses écritures sont ignorées.
Une banque affectée à ARM7 n'est plus lisible par ARM9. Les configurations
réservées n'exposent aucune fenêtre CPU. Un accès sans banque mappée lève une
erreur, sans simulation d'open-bus.

Des banques superposées combinent leurs octets par OU à la lecture et reçoivent
toutes les écritures. Les écritures 16/32 bits fonctionnent sur les fenêtres
CPU et par DMA. STRB ARM9 est ignoré ; STRB ARM7 fonctionne sur C/D en RAM de
travail. Les transferts multiples valident leurs destinations avant écriture ;
les DMA conservent leur garantie par unité en cas de banque suivante non mappée.

```cpp
using ravenemu::nds::Processor;
auto& bus = machine.memory();
bus.write8(Processor::arm9, 0x04000240, 0x80); // banque A en LCDC
bus.write16(Processor::arm9, 0x06800000, 0x001f); // premier pixel rouge
bus.write32(Processor::arm9, 0x04000000, 0x00020000); // DISPCNT A, affichage VRAM
bus.write16(Processor::arm9, 0x04000304, 0x8001); // LCD actif, A vers écran supérieur
const auto frame = machine.render_frame();
// frame.upper et frame.lower : chacun 256*192 valeurs RGB555 indépendantes
```

`Memory::render_frame()` et `Machine::render_frame()` retournent un `VideoFrame`
sans avancer le LCD, les CPU ou les DMA. Chaque pixel est un entier 16 bits :
rouge bits 0–4, vert 5–9, bleu 10–14, bit 15 nul. L'affichage direct (DISPCNT A
bits 16–17 = 2) lit les 256×192 pixels contigus de la banque A–D sélectionnée
par les bits 18–19 ; cette banque doit être activée en LCDC, sinon le modèle
lève une erreur. Le bit 15 des données VRAM est ignoré à l'affichage.

POWCNT1 bit 15 route A vers le haut et B vers le bas lorsqu'il vaut 1, et
inverse les deux lorsqu'il vaut 0. Chaque moteur possède son mode d'affichage :
DISPCNT A à `0x04000000`, DISPCNT B à `0x04001000`. Le mode 0 et forced blank
(DISPCNT bit 7) produisent du blanc sur l'écran associé ; LCD désactivé
(POWCNT1 bit 0 nul) produit deux écrans blancs dans ce modèle.

Le mode graphique 1 compose les fonds et sprites décrits ci-dessous ; le
mode FIFO 3 reste refusé. La luminosité maître est décrite plus bas ; capture et
3D restent absentes.
L'instantané lit toute l'image au
moment de l'appel : il n'émule pas les changements de pixels pendant le
balayage ni la présentation automatique à VBlank. Une copie de machine possède
ses propres octets et contrôles VRAM ; un chargement réussi les remet à zéro,
un échec les conserve. Référence :
[GBATEK, DS Memory Control — VRAM](https://mgba-emu.github.io/gbatek/ds.html#dsmemorycontrolvram).

## Fonds 2D, sprites et moteur B

Les deux moteurs rendent des images indépendantes de 256×192 pixels. Leurs
registres BG se trouvent à `0x04000008–0x0400003f` pour A et
`0x04001008–0x0400103f` pour B : BG0CNT à BG3CNT, défilements HOFS/VOFS et
matrices/références affines BG2/BG3. Les valeurs du modèle sont relisibles ;
les défilements sont masqués sur 9 bits et les références sur 28 bits signés.
DISPCNT B masque les fonctions réservées à A, avec `0xc0b1fff7`.

POWCNT1 bits 1 et 9 activent les moteurs A et B. Désactiver un moteur ignore
les écritures à ses ports BG et à sa palette ; les ports restent lisibles et
la palette se lit zéro. Réactiver le moteur retrouve les valeurs conservées.
Un moteur graphique éteint produit du blanc dans ce modèle ; l'affichage
direct VRAM A conserve son chemin distinct. Les autres bits POWCNT1 sont
stockés avec le masque `0x820f`, sans simulation des circuits 3D.

| Mémoire ARM9 | Moteur A | Moteur B |
| --- | --- | --- |
| Palette BG, 256 couleurs | `0x05000000` | `0x05000400` |
| Palette OBJ, 256 couleurs | `0x05000200` | `0x05000600` |
| OAM, 128 entrées de 8 octets | `0x07000000` | `0x07000400` |

Ces fenêtres canoniques acceptent les accès CPU/DMA 16 et 32 bits ; STRB est
ignoré et ARM7 n'y accède pas. L'OAM est conservée indépendamment de
l'activation du moteur. Les miroirs supplémentaires ne sont pas modélisés.

| Mode BG | BG0 | BG1 | BG2 | BG3 |
| --- | --- | --- | --- | --- |
| 0 | Texte | Texte | Texte | Texte |
| 1 | Texte | Texte | Texte | Affine |
| 2 | Texte | Texte | Affine | Affine |
| 3 | Texte | Texte | Texte | Étendu |
| 4 | Texte | Texte | Affine | Étendu |
| 5 | Texte | Texte | Étendu | Étendu |
| 6, A uniquement | 3D absente | — | Grand bitmap indexé | — |

Les fonds texte prennent en charge les tuiles 4/8 bpp, les banques de palette
4 bpp, les flips H/V, le défilement avec bouclage et les cartes 256×256,
512×256, 256×512 ou 512×512 organisées par blocs. A combine les bases de
caractères/cartes de DISPCNT avec BGxCNT ; B utilise BGxCNT seul.

Les fonds affines classiques utilisent des cartes à entrées 8 bits et des
tuiles 8 bpp. Les fonds étendus proposent cartes affines 16 bits à tuiles
8 bpp, bitmaps indexés 8 bpp ou bitmaps RGB555 avec transparence par bit 15.
Les matrices signées 8.8, références signées 20.8, clipping et bouclage sont
appliqués à l'instantané. Les grands bitmaps A atteignent 512 Kio ; les
bitmaps B dépassant ses 128 Kio sont refusés. Le mode 6 B et le mode 7 sont
réservés. Une couche BG0 sélectionnée en 3D reste refusée.

Les OBJ tuilés utilisent les palettes standard 4/8 bpp, les trois formes et
quatre tailles OAM, les flips H/V et les positions avec wrap horizontal/vertical.
Le mapping 2D utilise des rangées de 1024 octets ; le mapping 1D utilise les
frontières NDS de 32/64/128 octets, ou 256 octets sur A. Les 32 matrices OAM
permettent rotation/échelle et boîtes doubles avec clipping de la source.
Le wrap NDS peut afficher un grand OBJ affine à la fois en haut et en bas.

La composition compare d'abord les priorités BG/OBJ (0 est la plus haute).
À égalité, OBJ précède BG, BG0 précède BG1 à BG3 et le plus petit indice OAM
précède les autres OBJ. Contrairement au comportement GBA historique, une
priorité OBJ plus haute peut donc passer devant un OBJ de plus petit indice.
L'indice de couleur zéro est transparent ; la couleur BG standard zéro sert
de backdrop. Un noir provenant d'un indice non nul reste opaque.

La mosaïque reste à développer. Son activation lève une erreur plutôt que
de produire une image partiellement correcte ; le port MOSAIC n'est pas encore
mappé. Les palettes étendues sont décrites plus bas. Le moteur ne
reproduit pas les limites de pixels OBJ par ligne ni les modifications de registres au cours du balayage. Copies et chargements
conservent ou réinitialisent palettes, OAM et registres comme les autres états
NDS. Références : [GBATEK, DS Video BG Modes](https://mgba-emu.github.io/gbatek/ds.html#dsvideobgmodescontrol)
et [DS Video OBJs](https://mgba-emu.github.io/gbatek/ds.html#dsvideoobjs).

## Fenêtres et effets 2D

Chaque moteur possède WIN0H/WIN1H, WIN0V/WIN1V, WININ/WINOUT et
BLDCNT/BLDALPHA/BLDY, à `0x04000040–0x0400004b` et
`0x04000050–0x04000055` pour A, avec un décalage de `0x1000` pour B.
Ces ports ARM9 acceptent les voies 8/16 bits et les mots entièrement mappés ;
leurs valeurs sont relisibles dans ce modèle, même pour les ports matériels
à écriture seule. Bits réservés de WININ/OUT et des effets masqués, remise
à zéro et protection par POWCNT1 suivent les autres ports BG.

Les coordonnées de fenêtres sont sur 8 bits : début inclus, fin exclue,
intervalle traversant zéro si début > fin, intervalle vide si début = fin.
La priorité est WIN0, puis WIN1, puis OBJWIN, puis extérieur. Chaque région
active séparément BG0–3, OBJ et les effets ; sans fenêtre activée dans
DISPCNT, ces six autorisations sont toutes actives. Le backdrop reste visible.
Les OBJ en mode fenêtre ne dessinent aucune couleur : leurs indices non nuls
créent un masque, y compris avec flips ou matrice affine. Leur priorité OAM
et leur palette n'influencent pas ce masque ; OBJ et OBJWIN doivent être activés.

La composition retient les deux premières couches non transparentes, après
masquage, et un seul gagnant OBJ. BLDCNT sélectionne les premières et secondes
cibles : un pixel intermédiaire non sélectionné bloque le mélange. Les facteurs
EVA/EVB/EVY sont limités à 16 ; les canaux du mélange sont saturés. Les effets
couvrent mélange alpha, éclaircissement et assombrissement, backdrop compris.
Un OBJ semi-transparent force le mélange si la seconde couche est sélectionnée,
indépendamment du mode et de la première cible BLDCNT ; sinon les règles de
luminosité ordinaires s'appliquent. Les fenêtres peuvent désactiver ces effets.
Deux OBJ ne se mélangent jamais entre eux ; BG vers OBJ et OBJ vers BG restent
possibles.

Les OBJ bitmap utilisent RGB555 + bit 15 de visibilité, sans palette. En 1D,
la source est contiguë sur la largeur de l'objet, avec frontière de 128 octets,
ou 256 sur A (DISPCNT bit 22). En 2D, la source a un stride de 128 ou 256 pixels
(DISPCNT bit 5), et le numéro de tuile définit son origine par blocs de 8×8.
DISPCNT bit 6 choisit ce mapping indépendamment du mapping des OBJ tuilés.
Les flips, matrices et boîtes doubles restent applicables. Alpha-OAM zéro masque
l'objet ; les valeurs 1–15 donnent un facteur `(Alpha-OAM+1)/16`, avec complément
pour la seconde cible, indépendamment de BLDALPHA. Sans seconde cible autorisée,
le pixel visible conserve sa couleur. La combinaison 1D avec bit 5 actif est
réservée et refusée.

Les calculs de couleur sont effectués directement en RGB555 avec division
entière. Le chemin interne RGB666 du matériel DS et ses arrondis ne sont pas
encore reproduits. La luminosité maître utilise la même approximation RGB555.
Le rendu conserve son fonctionnement par instantané, sans effets de balayage.
Références : [GBATEK, fenêtres LCD](https://mgba-emu.github.io/gbatek/gba.html#lcdiowindowfeature),
[effets de couleur](https://mgba-emu.github.io/gbatek/gba.html#lcdiocolorspecialeffects)
et [OBJ DS](https://mgba-emu.github.io/gbatek/ds.html#dsvideoobjs).

## Palettes étendues et luminosité maître

DISPCNT bit 30 active les palettes BG étendues, bit 31 celles des OBJ, pour
chaque moteur indépendamment. Les tuiles 8 bpp avec carte BG sur 16 bits
(texte et affine étendu) utilisent le numéro de palette de la carte :
16 palettes de 256 couleurs par slot BG de 8 Kio. BG0..3 utilisent les slots
0..3 ; BG0CNT/BG1CNT bit 13 peut sélectionner le slot 2/3 à la place de 0/1.
Les OBJ 8 bpp sélectionnent l'une des 16 palettes par OAM, dans 8 Kio au total.
Les tuiles 4 bpp, l'affine à carte 8 bits et les bitmaps indexés conservent les
palettes standard ; les bitmaps directs BG/OBJ n'utilisent aucune palette.
L'indice zéro reste transparent et le backdrop vient de la palette standard.

| Usage GPU | Banques et mode VRAMCNT | Emplacement utilisé |
| --- | --- | --- |
| BG A | E, mode 4 | 32 Kio bas, slots 0..3 |
| BG A | F/G, mode 4, OFS 0 ou 1 | 16 Kio, slots 0/1 ou 2/3 |
| OBJ A | F/G, mode 5 | 8 Kio bas, OFS ignoré |
| BG B | H, mode 2 | 32 Kio, slots 0..3 |
| OBJ B | I, mode 3 | 8 Kio bas |

Les banques affectées aux palettes étendues n'ont pas de fenêtre CPU. Il faut
les réaffecter temporairement, par exemple en LCDC, pour charger leurs couleurs,
puis les réallouer au GPU. La réaffectation conserve les octets. Les banques
superposées sont combinées en OU, comme les vues VRAM existantes ; une palette
non allouée se lit à zéro. OFS 2/3 de F/G en mode BG étendu reste réservé et ne
fournit aucune couleur. Les entrées sont RGB555, bit 15 ignoré.

`MASTER_BRIGHT` est mappé sur ARM9 à `0x0400006c` et `0x0400106c` pour A et B.
Le masque est `0xc01f` : facteur sur 5 bits, saturé à 16, et mode sur les bits
14–15 (0 sans effet, 1 éclaircissement, 2 assombrissement). Le mode 3 réservé
lève une erreur au rendu ; les bits inutilisés, y compris les 16 bits hauts,
se lisent à zéro. Les accès 8/16/32 bits sont pris en charge. Ces registres
restent accessibles quand le moteur 2D correspondant est éteint : contrairement
aux ports BG/BLD, ils sont hors de la plage protégée par POWCNT1.

La luminosité maître s'applique après la composition et les effets BLDCNT,
indépendamment des masques de fenêtres. Elle traite aussi l'affichage VRAM
direct et suit le moteur lors de la permutation des écrans. Dans ce modèle
par instantané, les sorties blanches du mode 0, de forced blank et d'un moteur
2D éteint passent par cette étape ; LCD éteint conserve deux images blanches.
Les calculs restent en RGB555, sans les arrondis internes RGB666 ni les
changements en cours de balayage. Copies et chargements conservent ou remettent
à zéro registres et octets de palette comme le reste de la mémoire vidéo.
Références : [GBATEK, palettes étendues DS](https://mgba-emu.github.io/gbatek/ds.html#dsvideoextendedpalettes),
[contrôle VRAM](https://mgba-emu.github.io/gbatek/ds.html#dsmemorycontrolvram)
et [luminosité maître](https://mgba-emu.github.io/gbatek/ds.html#400006chnds9masterbright16bitmasterbrightnessupdown).

## Timers

Chaque processeur possède quatre timers 16 bits indépendants. Pour le timer `n`
(0 à 3), `0x04000100 + 4*n` lit le compteur courant et reçoit la valeur de
rechargement à l'écriture ; `0x04000102 + 4*n` contient le contrôle :

| Bits | Fonction |
| --- | --- |
| 0–1 | Diviseur de l'horloge : 1, 64, 256 ou 1024 |
| 2 | Cascade : compter les débordements du timer précédent ; ignoré sur timer0 |
| 6 | Demande IRQ au débordement, bit IF `3+n` |
| 7 | Marche ; le passage de 0 à 1 charge le compteur depuis reload |

Les autres bits se lisent zéro et leurs écritures sont ignorées. Les accès
8/16/32 bits respectent les voies little-endian. Écrire reload conserve le
compteur courant, même à l'arrêt ; il est chargé au prochain débordement ou
démarrage. Une écriture 32 bits de reload et contrôle utilise la nouvelle
valeur si elle démarre le timer. Réécrire start déjà actif ne recharge rien.
Un timer arrêté reste figé ; une cascade arrêtée coupe la propagation.
La cascade ignore le préscaler et fonctionne même sans IRQ sur le timer amont.

```cpp
using ravenemu::nds::Processor;
machine.memory().write32(Processor::arm9, 0x04000100, 0x00c0fffc);
machine.advance_cycles(4); // quatre cycles de l'horloge des timers, commune aux deux CPU
// IF ARM9 bit 3 est levé ; le prochain step pourra entrer en IRQ si IME/IE/CPSR le permettent.
```

`Machine::advance_cycles()` (ou `Memory::advance_cycles()`) avance les huit
timers et le LCD sans exécuter d'instructions. Leur fréquence de référence commune est
`timer_clock_hz = 33513982` Hz, distincte de la fréquence du CPU ARM9.
`step()` et `run_steps()` n'avancent pas cette horloge. Les préscalers conservent
leur reste entre appels ; les grands intervalles et cascades sont calculés
sans boucle par cycle ni par débordement. Les IRQ restent mémorisées dans IF
même avec IME/IE masqués. Copier une machine copie aussi les phases des timers ;
`clear_ram()` et un chargement de cartouche réussi les réinitialisent.

Ce modèle reste sans précision matérielle des transitions : le démarrage et
le rechargement sont immédiats, sans latence de bus. Changer marche, source ou
diviseur remet la phase du préscaler à zéro ; changer seulement l'autorisation
IRQ conserve la phase. Ces règles déterministes ne constituent pas une validation
des latences NDS. L'intégration à un ordonnanceur et les timings CPU restent à faire.
Référence : [GBATEK, DS Timers](https://mgba-emu.github.io/gbatek/ds.html#dstimers).

## Communication ARM9/ARM7

Le composant `Ipc`, déclaré dans `<ravenemu/nds/ipc.hpp>`, conserve les deux
extrémités de communication dans le bus. Il ne conserve aucune référence aux
CPU ni aux contrôleurs d'interruptions.

| Registre | Adresse | Comportement |
| --- | --- | --- |
| IPCSYNC | `0x04000180` | Bits 8–11 : sortie locale ; bits 0–3 : sortie distante ; bit 14 : autoriser l'IRQ distante ; écrire le bit 13 émet un pulse vers l'autre CPU |
| IPCFIFOCNT | `0x04000184` | Statuts plein/vide des deux directions, autorisations IRQ, activation locale et erreur W1C |
| IPCFIFOSEND | `0x04000188` | Écriture d'un mot dans la file sortante, jusqu'à 16 mots |
| IPCFIFORECV | `0x04100000` | Lecture du premier mot de la file entrante, avec retrait si la réception locale est activée |

IPCSYNC transmet une IRQ IF bit 16 uniquement si le CPU distant l'autorise
au moment du pulse. Le pulse ne reste pas mémorisé dans IPCSYNC. Les données
de synchronisation restent lisibles indépendamment des autorisations IRQ.

Dans IPCFIFOCNT, les bits 0/1 indiquent sortie vide/pleine, 8/9 entrée
vide/pleine. Le bit 2 autorise l'IRQ de sortie vide (IF bit 17), le bit 10
l'IRQ d'entrée non vide (IF bit 18) et le bit 15 active les envois/retraits
locaux. Écrire le bit 3 vide uniquement la file sortante et efface sa dernière
valeur reçue à distance. Le bit 14 mémorise les erreurs locales ; écrire 1
l'acquitte, écrire 0 le conserve. Les bits réservés se lisent zéro.

Envoyer avec la FIFO désactivée ne stocke rien et ne produit pas d'erreur.
Lire avec la réception désactivée retourne le premier mot sans le retirer.
Envoyer dans une file pleine conserve les 16 mots et lève l'erreur locale.
Lire une file vide lève l'erreur du lecteur et répète le dernier mot reçu,
ou zéro après reset/vidage, même si la réception est désactivée.

Les IRQ FIFO sont des fronts sur les conditions `autorisation && sortie vide`
et `autorisation && entrée non vide`, indépendamment du bit 15. Les acquitter
dans IF ne les réaffirme pas tant que leur condition reste vraie. Autoriser
une IRQ dont la condition de statut est déjà vraie produit un nouveau front.
Les autorisations IME/IE et le masque CPSR.I conservent leur rôle habituel.

```cpp
using ravenemu::nds::Processor;
auto& bus = machine.memory();
bus.write16(Processor::arm9, 0x04000184, 0x8000);
bus.write16(Processor::arm7, 0x04000184, 0x8000);
bus.write32(Processor::arm9, 0x04000188, 0x12345678);
auto message = bus.read32(Processor::arm7, 0x04100000); // retire exactement un mot
```

Les accès à SEND/RECV sont limités aux mots 32 bits alignés dans ce modèle.
Les accès 8/16 bits, lire SEND et écrire RECV sont explicitement refusés ;
leurs éventuels comportements matériels restent à développer. IPCSYNC et
IPCFIFOCNT acceptent les accès 8/16/32 bits par voies little-endian ; leurs
demi-mots hauts réservés se lisent zéro et ignorent les écritures.

`Memory::read8/16/32()` sont désormais des lectures bus sur une mémoire
mutable : une lecture de RECV peut retirer un mot, lever une erreur ou
demander une IRQ à l'émetteur. Pour inspecter une mémoire constante ou une
FIFO sans effet de bord, utiliser `peek8/16/32()` ; une inspection vide ne
lève aucune erreur. `read_words()` valide toute la plage avant les effets
MMIO, ce qui protège notamment LDM/POP d'un retrait partiel si une adresse
suivante est refusée. Les écritures multiples conservent la même validation
préalable, y compris avec SEND et les pulses IPCSYNC.

Les échanges prennent effet immédiatement selon l'ordre des appels à
`step()`/`run_steps()` ; aucune exécution simultanée ni latence matérielle
n'est encore modélisée. Copier une machine copie les files, erreurs et
registres IPC ; un chargement réussi les réinitialise, un échec les préserve.
Référence : [GBATEK, DS Inter Process Communication](https://mgba-emu.github.io/gbatek/ds.html#dsinterprocesscommunicationipc).

## BIOS et diagnostics

`Memory::load_bios(processor, bytes)` accepte exactement 16 Kio sur ARM7
(`0x00000000`) ou 4 Kio sur ARM9 (`0xffff0000`). Ces fenêtres sont en lecture seule.
Sans image, lire un vecteur échoue explicitement. Les tests fournissent de petits
gestionnaires synthétiques ; aucun BIOS Nintendo ni firmware n'est inclus.
Il n'y a pas de remplacement haut niveau des services SWI, de démarrage firmware,
de protection de lecture BIOS selon PC, de relocation des vecteurs par CP15 ou
d'exceptions Data/Prefetch Abort. Les accès mémoire refusés restent des diagnostics.

Une instruction non implémentée ou une erreur de lecture/écriture lève
`CpuExecutionError` avec processeur, adresse, mode et opcode (absent si la
lecture de l'instruction a échoué). Le CPU reste sur l'instruction fautive,
ses registres et son compteur sont préservés. Les écritures multiples vérifient
toutes les destinations du bus avant de modifier le premier mot ; une erreur de
chargement multiple ne valide aucun des registres chargés. Cette atomicité est
un contrat de diagnostic de ce modèle, pas une émulation des exceptions d'abandon
matérielles. Les instructions déjà terminées
d'un `run_steps` restent appliquées. Le chargement d'une nouvelle cartouche
réinitialise les deux CPU. Aucun CPU ne conserve de référence persistante au
bus : copier une machine produit une mémoire et des CPU indépendants.

`nds_cpu` exécute sur chacun des deux processeurs des programmes synthétiques
qui calculent une somme, accèdent à la RAM, appellent une fonction et échangent
ARM/Thumb. Il vérifie aussi les limites de décalage, retenues/débordements,
conditions, erreurs, budgets et un échange ARM9/ARM7 par la RAM commune.
`nds_calls` vérifie les appels ARM et Thumb imbriqués avec restauration de pile,
les quatre modes d'adressage ARM, LDMIA/STMIA Thumb, les formes BLX et les retours
entre modes sur ARM9, ainsi que les différences de chargement de PC sur ARM7.
`nds_data` vérifie les lectures signées, les adressages indexés, les limites des
décalages par registre, les produits 32/64 bits et l'accumulation. Deux programmes
ARM et Thumb calculent chacun `-3*5 + 7*(-4) + (-2)*9 = -61` à partir de tableaux
signés sur 16 bits, sur chacun des deux CPU. Les encodages non pris en charge et
les erreurs d'alignement doivent préserver registres, writeback et mémoire.
`nds_exceptions` vérifie les banques, privilèges, CPSR/SPSR, les gestionnaires
synthétiques SWI/UND/IRQ/FIQ, la priorité des lignes, les retours ARM/Thumb et
la conservation des images BIOS au chargement d'une cartouche.
`nds_interrupts` couvre IME/IE/IF, les sources propres à chaque CPU, les voies
d'octets, WRAMCNT/WRAMSTAT et les transferts refusés sans acquittement partiel.
Un programme ARM configure IME/IE, démasque le CPSR, traite une demande injectée,
acquitte IF depuis un gestionnaire synthétique et reprend son exécution sur
chacun des deux processeurs.
`nds_timers` vérifie les huit canaux, rechargements, voies d'octets, préscalers,
cascades, changements de contrôle, copies, resets et intervalles jusqu'à
`UINT64_MAX`. Il compare aussi les avances groupées à des cycles individuels.
Un programme ARM démarre timer0 sur chaque CPU, traite son débordement réel
via un BIOS synthétique, acquitte IF et reprend le programme interrompu.
`nds_ipc` vérifie la synchronisation, les deux directions FIFO, le bouclage
de l'anneau, les erreurs, le vidage, les autorisations et les fronts IRQ.
Il contrôle aussi les inspections sans effet, les transferts refusés avant
tout effet MMIO, LDM ARM et LDR Thumb. Deux programmes ARM réalisent un
aller-retour complet avec réponse calculée et gestionnaires IRQ synthétiques.
`nds_dma` vérifie les huit canaux, les masques propres à chaque CPU, les
longueurs nulles, les copies 16/32 bits, les modes d'adressage, les priorités,
le remplissage, les copies chevauchantes, les erreurs et les copies de machine.
Deux programmes ARM configurent leur DMA, restent suspendus pendant la copie,
traitent l'IRQ de fin depuis un BIOS synthétique et reprennent leur exécution.
`nds_lcd` vérifie les frontières de balayage, LYC sur 9 bits, les IRQ propres
à chaque CPU, les grands intervalles et les DMA VBlank/HBlank répétés avec
rechargement de destination. Deux programmes ARM configurent LCD et DMA,
traitent un VBlank réel, exécutent la copie avant l'IRQ et reprennent après acquittement.
`nds_video` vérifie les neuf banques, leurs masques et fenêtres CPU, les
réaffectations ARM9/ARM7, les superpositions, les accès DMA et les erreurs
sans écriture partielle. Un programme ARM configure VRAMCNT/DISPCNT/POWCNT1
et copie par DMA une image RGB555 de 256×192 pixels, vérifiée pixel par pixel.
`nds_2d` couvre les registres A/B, palettes/OAM, l'extinction, les cartes
texte, transparences et priorités, références affines négatives, bitmaps,
sprites 1D/2D, flips, matrices et wrap NDS. Un programme ARM construit deux
scènes BG/OBJ indépendantes, vérifiées sur les deux images complètes, puis
permute les écrans ; copies et échecs de chargement doivent les préserver.

`nds_effects` vérifie fenêtres superposées et traversant zéro, masques de couches,
OBJWIN, cibles alpha, coefficients et saturations, luminosité, OBJ semi-transparents
et bitmaps 1D/2D, transparence, flips et matrices sur les deux moteurs. Des STR ARM
configurent deux scènes avec mélanges différents, vérifiées pixel par pixel ;
la copie des effets et la permutation des écrans sont aussi contrôlées.

`nds_palettes` vérifie les slots BG0..3, leurs sélecteurs, les seize palettes,
les limites de banque, la transparence et les modes conservant les palettes
standard sur A/B. Il couvre les allocations E/F/G/H/I, superpositions en OU,
réaffectations CPU/GPU, les masques MASTER_BRIGHT, facteurs, ordre des effets,
fenêtres, VRAM directe et extinction. Une ROM synthétique exécute des STRH ARM9
pour configurer les deux images complètes ; permutation, copies et chargements
réussis/refusés sont aussi vérifiés.

Prochains jalons : compléter le jeu d'instructions, définir les timings et la
synchronisation des deux CPU, DMA et timers, puis mosaïque, capture, 3D,
entrées et audio.

Aucun jeu ne tourne à ce stade. Le cœur n'est pas encore exposé par `Core`,
le SDK ou Libretro : ces interfaces viendront avec une exécution fonctionnelle.
