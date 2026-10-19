# RavenNDS — cœur expérimental

La cible C++20 `nds_raven_core` prépare le futur moteur Nintendo DS.
Elle expose `ravenemu::nds::Cartridge`, `Memory`, `Cpu`, `CpuState` et `Machine`, dans
`<ravenemu/nds/{cartridge,memory,cpu,machine}.hpp>` ; elle est aussi liée par `RavenCore::ravencore`.
L'état et les modes CPU sont déclarés dans `<ravenemu/nds/cpu_state.hpp>`.

## Organisation

```text
nds/
├── include/ravenemu/nds/  # contrats publics
├── src/
│   ├── bios/             # images et fenêtres BIOS
│   ├── cartridge/        # cartouches .nds
│   ├── cpu/              # ARM, Thumb, état, PSR et exceptions
│   ├── interrupts/       # contrôleurs IME/IE/IF indépendants
│   ├── machine/          # assemblage des deux CPU et chargement
│   ├── memory/           # bus RAM, miroirs et WRAM
│   └── timers/           # compteurs, préscalers et cascade
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
MMIO d'interruption, timers et WRAMCNT/WRAMSTAT sont présents. TCM, VRAM, autres MMIO et
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
Un pas exécute une instruction ou entre en IRQ/FIQ. Le compteur `instructions`
compte uniquement les instructions terminées, dont SWI, UDF et les conditions
ARM fausses. Une entrée IRQ/FIQ ne l'incrémente pas. Il n'y a pas
encore d'ordonnanceur, de calcul des cycles CPU ou de rapport de fréquence ARM9/ARM7.
Les timers disposent d'une horloge explicite indépendante, décrite ci-dessous.

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

IRQ/FIQ sont des entrées externes de niveau, échantillonnées à chaque pas :
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
avancement explicite. PPU et DMA restent absents. Le CPU consulte la ligne du bus à chaque pas :
un gestionnaire qui acquitte IF supprime la demande avant le retour. Référence :
[GBATEK, DS Interrupts](https://mgba-emu.github.io/gbatek/ds.html#dsinterrupts).

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
timers sans exécuter d'instructions. Leur fréquence de référence commune est
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

Prochains jalons : compléter le jeu d'instructions, définir les timings et la
synchronisation des deux CPU et des timers, puis DMA, IPC, vidéo
2D à deux écrans, entrées et audio.

Aucun jeu ne tourne à ce stade. Le cœur n'est pas encore exposé par `Core`,
le SDK ou Libretro : ces interfaces viendront avec une exécution fonctionnelle.
