# RavenNDS — première base

La cible C++20 `nds_raven_core` prépare le futur moteur Nintendo DS.
Elle expose `ravenemu::nds::Cartridge`, `Memory`, `Cpu` et `Machine`, dans
`<ravenemu/nds/{cartridge,memory,cpu,machine}.hpp>` ; elle est aussi liée par `RavenCore::ravencore`.

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
cmake --build build --target nds_cartridge_tests nds_memory_tests nds_boot_tests nds_cpu_tests nds_calls_tests --parallel 4
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
réinitialise registres, drapeaux et compteurs CPU.

Ce chargement est une préparation expérimentale, pas une séquence de démarrage
BIOS : pas de firmware, de déchiffrement, de paramètres de démarrage, de copie
de l'en-tête dans la RAM ni d'initialisation complète des registres CPU. Les TCM,
MMIO (y compris le registre WRAMCNT), VRAM et comportements open-bus ne sont pas
implémentés ; les accès non mappés lèvent `ravenemu::Error`. Les lectures ARM
non alignées et leurs rotations ne sont pas encore prises en charge par le CPU.

## Premier interpréteur ARM/Thumb

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

Ces appels exécutent le nombre demandé d'instructions sur le CPU sélectionné,
sans faire avancer l'autre. Le budget est borné même sur une boucle infinie.
Une condition ARM fausse compte comme une instruction terminée. Il n'y a pas
encore d'ordonnanceur, de compteur de cycles ou de rapport de fréquence ARM9/ARM7.

| Mode | Instructions actuellement prises en charge |
| --- | --- |
| ARM | Les 16 opérations de traitement de données (AND à MVN), opérande immédiate tournée ou registre décalé par une constante ; conditions EQ à AL ; B, BL, BX ; LDR/STR et LDRB/STRB à décalage immédiat, pré/post-indexés ; LDM/STM IA/IB/DA/DB |
| Thumb | Décalages immédiats ; ADD/SUB registre ou immédiat ; MOV/CMP/ADD/SUB immédiats ; les 16 opérations ALU sur registres bas, dont décalages par registre et MUL ; ADD/CMP/MOV sur registres hauts, BX ; LDR littéral ; LDR/STR octet, demi-mot et mot à décalage immédiat, transferts relatifs à SP ; branches conditionnelles, B et BL ; PUSH/POP, LDMIA/STMIA, ADD relatif à PC/SP et ajustement de SP |
| ARM9 uniquement | BLX par registre en ARM/Thumb et par immédiat en ARM/Thumb ; chargement de PC avec échange ARM/Thumb |

Les stockages de PC, les writebacks avec base dans la liste transférée (sauf
LDMIA Thumb, qui préserve la base chargée), les listes vides, les variantes
LDM/STM avec bit S, les décalages ARM par registre, les transferts ARM par demi-mot et les autres
variantes absentes sont refusés explicitement. MUL Thumb conserve C/V dans ce
modèle ; ces drapeaux ne doivent pas servir à revendiquer une fidélité aux
comportements indéterminés du matériel. Les transferts non alignés sont refusés.
Les registres bancarisés, CPSR/SPSR, interruptions, exceptions, SWI, CP15, TCM,
caches et autres instructions propres à ARMv5
restent à développer. Ce sous-ensemble commun ne constitue pas encore un
ARM946E-S ou un ARM7TDMI complet. L'interpréteur lit directement la RAM à chaque
pas ; il ne reproduit pas le pipeline ni le comportement du code automodifiant.

`BL` Thumb se déroule en deux pas : le préfixe place la partie haute signée de
la cible dans LR, puis le suffixe branche et fixe l'adresse de retour Thumb.
Un arrêt entre les deux conserve ce LR intermédiaire. Les formes `BLX` ARMv5
sont disponibles uniquement sur ARM9 ; elles lèvent une erreur sur ARM7.
`LDR pc`, `LDM ...,{pc}` et `POP {pc}` échangent ARM/Thumb selon le bit 0 de
l'adresse sur ARM9, tandis qu'ARM7 conserve son mode courant. Les adresses
sont alignées selon le mode obtenu. Les calculs de PC, LR, SP et les transferts
ne modifient pas NZCV. Référence : ARM Architecture Reference Manual, ARMv4T /
ARMv5T, descriptions de BL, BLX, LoadWritePC, LDM/STM et PUSH/POP.

Une instruction non implémentée ou une erreur de lecture/écriture lève
`CpuExecutionError` avec processeur, adresse, mode et opcode (absent si la
lecture de l'instruction a échoué). Le CPU reste sur l'instruction fautive,
ses registres et son compteur sont préservés. Les écritures multiples vérifient
toutes les destinations RAM avant de modifier le premier mot ; une erreur de
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

Prochains jalons : compléter le jeu d'instructions, ajouter les modes et
exceptions, définir les timings et la synchronisation des deux CPU, puis
interruptions, IPC, vidéo 2D à deux écrans, entrées et audio.

Aucun jeu ne tourne à ce stade. Le cœur n'est pas encore exposé par `Core`,
le SDK ou Libretro : ces interfaces viendront avec une exécution fonctionnelle.
