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
│   ├── cartridge/        # images .nds, port RAW/post-boot et EEPROM/flash/FRAM AUXSPI
│   ├── cpu/              # ARM, Thumb, état, PSR et exceptions
│   ├── dma/              # canaux, transferts immédiats et VBlank/HBlank
│   ├── firmware/         # image fournie par l'hôte, réglages CRC et étalonnage
│   ├── input/            # boutons, KEYCNT, capot et état /PENIRQ
│   ├── interrupts/       # contrôleurs IME/IE/IF indépendants
│   ├── ipc/              # synchronisation et FIFO ARM9/ARM7
│   ├── machine/          # assemblage des deux CPU et chargement
│   ├── memory/           # bus RAM, miroirs et WRAM
│   ├── spi/              # bus ARM7, Powerman, flash partagée et convertisseur tactile
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
conservé. Les images DSi sont explicitement refusées. `Cartridge::rom()` expose
une vue constante de toute l'image. Les copies partagent un stockage immuable
possédé par la bibliothèque : copier une machine ne duplique pas une grande ROM,
et les transferts matériels restent indépendants.

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

## Port cartouche, ressources et DMA

`Gamecard`, déclaré dans `<ravenemu/nds/gamecard.hpp>`, conserve le média et son
état de transfert. `Machine::load_cartridge()` l'installe automatiquement en mode
de données principal pour le démarrage direct. `Memory::load_gamecard()` permet
aussi de brancher seulement le média, avec `Gamecard::Mode::raw` pour les premiers
échanges non chiffrés. Les lectures ne dépendent plus de la durée de vie du buffer
ou de l'objet `Cartridge` fourni par l'hôte.

| Registre | Comportement |
| --- | --- |
| `0x040001a0` AUXSPICNT | Activation du slot, sélection ROM/SPI, IRQ de fin ; masque `0xe043` |
| `0x040001a2` AUXSPIDATA | Octet SPI reçu ; une écriture déclenche un échange |
| `0x040001a4` ROMCTRL | Taille du bloc, débit, gaps, RESB, busy et DRQ |
| `0x040001a8..1af` | Huit octets de commande, premier octet/opcode à l'adresse basse |
| `0x04100010` CARD_DATA | Mot reçu little-endian, accès 32 bits uniquement |
| `0x04000204` EXMEMCNT/STAT | Propriétaire du slot DS via bit 11 ; écriture partagée par ARM9 |

Le mode RAW accepte `00` (en-tête, répété tous les 4 Kio), `90` (identifiant) et
`9f` (dummy `ff`). Le mode principal accepte `b7` et son adresse big-endian sur
quatre octets, ainsi que `b8` pour l'identifiant. Les blocs console de 0, 4 et
512..16 384 octets sont pris en charge. B7 accepte une adresse non alignée,
boucle dans le bloc courant de 4 Kio, et redirige les adresses physiques sous
`0x8000` vers `0x8000 + (adresse & 0x1ff)`.

La capacité physique du modèle est une puissance de deux de 8 à 512 Mio,
assez grande pour le dump et pour la capacité déclarée à l'octet d'en-tête `0x14`.
Les adresses sont ramenées à cette capacité, et les octets absents d'un dump
réduit sont `ff`. Une capacité non prise en charge est refusée avant de remplacer
la machine. L'identifiant modélise un composant Macronix (`c2`), selon la capacité ;
il ne prétend pas identifier le fabricant réel à partir du dump. Sans média,
une commande retourne des mots `ffffffff` dans ce modèle.

Le modèle de transfert utilise un octet par clock cartouche : diviseur 5 ou 8
de l'horloge commune, huit clocks de commande, gap1, puis quatre clocks par mot.
Gap2 s'ajoute entre blocs de 512 octets. DRQ n'est levé qu'au terme du délai.
Un mot prêt bloque la production jusqu'à consommation ; `peek32` inspecte sans
consommer. Lire trop tôt rend le dernier mot sans avancer. Le dernier mot lu
efface busy/DRQ et demande IF.19 si AUXSPICNT.14 est activé. Un bloc sans données
termine après sa commande et son gap1, sans produire DRQ. IME/IE conservent les
mêmes contrats que les autres sources. Écritures de commandes/ROMCTRL pendant
busy ignorées ; désactiver le slot ou passer en SPI abandonne sans IRQ différée.

Le DMA cartouche utilise le mode 5 sur ARM9, 2 sur ARM7. Il exige CARD_DATA en
source fixe et des mots de 32 bits ; chaque DRQ autorise **un seul mot**, même
avec un compteur programmé plus grand. Le réglage usuel est compteur 1 avec
répétition. Un canal armé après l'apparition de DRQ peut encore lire le mot prêt.
La destination est validée avant toute consommation ; un échec conserve le
mot et le canal fautif. Les accès DMA contournent les TCM ARM9. Les IRQ de fin
DMA et de fin de bloc cartouche restent distinctes.

EXMEM bit 11 choisit ARM9 (0) ou ARM7 (1). Les bits 0..6 sont locaux à chaque CPU ;
7/11/15 sont partagés et modifiables seulement par ARM9. Les bits 13/14 sont lus
à un dans ce modèle NDS, les bits réservés à zéro. Les réglages de slot GBA et
de priorité sont mémorisés mais ne fournissent pas de bus GBA ni de timings RAM.
Le CPU non propriétaire lit zéro et ses écritures sont ignorées, sans retirer
de donnée. Changer de propriétaire pendant busy est diagnostiqué avant mutation,
ce cas n'étant pas encore validé matériellement. La sélection peut changer une
fois le bloc terminé ou abandonné.

Le démarrage direct prépare un slot activé avec RESB libéré en mode principal.
`clear_ram()` remet les registres/transferts dans cet état et conserve média et
mode ; un chargement refusé préserve aussi le transfert courant. En RAW, RESB
doit être libéré explicitement avant un transfert. Son bit reste ensuite à un
jusqu'au reset hôte du modèle ; les astuces physiques de reset par EXMEM ne sont
pas émulées. Copie et remplacement de média isolent les curseurs, délais et DMA.

Les commandes/réponses du mode principal sont traitées déchiffrées, comme attendu
après le démarrage direct. Les bits KEY2 de données/commande sont mémorisés ;
ils n'implémentent pas le chiffrement du fil physique. La séquence KEY1 de boot,
l'application des semences KEY2, les registres de semence, DSi et NAND restent
explicitement hors périmètre.
Les timings fins, l'open-bus et les variantes de fabricants restent à valider.
Cette implémentation C++ originale suit
[GBATEK, Cartridge Protocol](https://mgba-emu.github.io/gbatek/ds.html#dscartridgeprotocol),
[I/O Ports](https://mgba-emu.github.io/gbatek/ds.html#dscartridgeioports),
[Memory Control](https://mgba-emu.github.io/gbatek/ds.html#dsmemorycontrolcartridgesandmainram)
et [DMA](https://mgba-emu.github.io/gbatek/ds.html#dsdmatransfers), sans code d'émulateur importé.

## Sauvegardes cartouche EEPROM

`Backup`, dans `<ravenemu/nds/backup.hpp>`, modélise les EEPROM de 512 octets,
8 Kio et 64 Kio : adresses 8+1 ou 16 bits, pages de 16/32/128 octets, lectures
séquentielles et miroir à la capacité. Les écritures rebouclent dans la page ;
les derniers octets reçus gagnent si une transaction dépasse sa taille.
WREN/WRDI, RDSR, WRSR, WEL/WIP et protection du quart supérieur, de la moitié
supérieure ou de toute la mémoire sont pris en charge. `/W` est câblé haut ;
SRWD est mémorisé sur les grands profils sans verrouiller à lui seul WRSR.
RDID retourne `ff` sur ces EEPROM, comme sur un périphérique absent.

L'hôte choisit explicitement `BackupType::eeprom_512`, `eeprom_8k`, `eeprom_64k`
ou `none`, **après** `load_cartridge()` / `load_gamecard()`, avec
`load_backup(type, image = {}, status = 0)`. Une image vide crée une EEPROM
effacée (`ff`) ; autrement sa taille doit correspondre exactement au profil.
La ROM ne contient pas de champ fiable pour identifier cette mémoire ; aucune
détection automatique ni base de jeux n'est encore fournie. Par défaut, aucun
périphérique n'est connecté et tous les échanges retournent `ff`.

Le propriétaire EXMEM est commun à la ROM et à AUXSPI. Chaque octet prend
`64 << baud` cycles communs ; busy n'est actif que pendant cet échange. Hold
maintient CS entre octets ; il faut le retirer avant le dernier. Un strobe ou
un changement de débit/hold pendant busy est ignoré. Désactiver le slot ou
quitter le mode série interrompt un octet incomplet et abandonne son paquet.
Sans octet en cours, cette désactivation termine la transaction déjà reçue.
Une transaction valide lance à CS haut un cycle interne d'écriture modélisé à
5 ms ; RDSR reste accessible pendant WIP, les autres commandes sont ignorées.
L'écriture interne avance même si le slot est désactivé ou repassé en ROM.
Le bit IRQ d'AUXSPICNT concerne uniquement la ROM : AUXSPI ne produit ni IRQ19
ni DRQ cartouche. Le bus SPI du firmware et celui des sauvegardes sont séparés.

`Machine` et `Memory` exposent `backup_dirty()`, `snapshot_backup()` et
`acknowledge_backup_saved(generation)`. L'instantané possède les octets bruts
compatibles avec un fichier `.sav`, une génération et les bits non volatils du
registre d'état dans une métadonnée `status` séparée. Le frontend doit écrire
`data` sur disque et, s'il conserve les protections WRSR, stocker `status`
séparément puis le transmettre à `load_backup()` ; aucun suffixe propriétaire
n'est ajouté au `.sav`. Un acquittement ancien ne nettoie pas des données plus
récentes, et une écriture identique ne change pas la génération. Un instantané
est refusé pendant WIP, et le remplacement d'une sauvegarde pendant un échange
ou une écriture interne est refusé avant mutation.

`clear_ram()` conserve EEPROM, protections et écriture interne déjà acceptée,
mais abandonne les paquets série incomplets et efface WEL. Un nouveau chargement
de cartouche retire la sauvegarde précédente, même si la ROM est identique :
l'hôte doit l'exporter puis la recharger explicitement. Copier une machine
isole ses données EEPROM mutables. Les API ne réalisent pas d'accès disque.

Les tests utilisent les trois profils, les limites de pages/adresses, les
protections, les délais, les resets et la persistance, ainsi que deux programmes
synthétiques ARM9/ARM7 qui interrogent WIP puis relisent leur écriture. Cette
implémentation originale suit [GBATEK, DS Cartridge Backup](https://mgba-emu.github.io/gbatek/ds.html#dscartridgebackup).
Les EEPROM 128 Kio et interfaces infrarouges restent à
implémenter ; aucune compatibilité de sauvegarde commerciale n'est revendiquée.

## Sauvegardes cartouche flash M45PE

`BackupType::flash_256k`, `flash_512k` et `flash_1m` sélectionnent des modèles
ST M45PE20, M45PE40 et M45PE80 de 256 Kio, 512 Kio et 1 Mio. Leurs identifiants
JEDEC sont respectivement `20 40 12`, `20 40 13` et `20 40 14`, tels que documentés
dans [GBATEK, DS Firmware Serial Flash Memory](https://mgba-emu.github.io/gbatek/ds.html#dsfirmwareserialflashmemory).
Le profil est un choix de l'hôte ; il ne prétend pas identifier le fabricant
d'une cartouche à partir de sa ROM. Les API de chargement, instantanés et
acquittements sont les mêmes que pour les EEPROM ; `BackupSnapshot::status`
est zéro sur ces modèles, et un état non volatil EEPROM est refusé au chargement.

Le moteur série M45PE de RavenNDS est partagé avec le firmware, avec des états
et des images distincts. Il gère les adresses 24 bits, les miroirs à la capacité,
READ, FAST avec son octet dummy, RDID, RDSR, WREN/WRDI, PP, PW, PE, SE et DP/RDP.
PP (`02`) ne peut qu'abaisser des bits ; PW (`0a`) peut les remettre à un tout
en conservant les voisins non écrits. PE (`db`) efface une page de 256 octets ;
SE (`d8`) efface un secteur de 64 Kio, alignés à leurs tailles. Ces profils
cartouche ont `/W` haut : les premiers 64 Kio sont accessibles en écriture,
contrairement au firmware où `/W` bas les protège. Le registre d'état M45PE
expose uniquement WEL/WIP ; la commande WRSR des EEPROM est diagnostiquée.

Les opérations commencent à CS haut, avec les délais typiques du modèle
firmware : PP 1,2 ms, PW 11 ms, PE 10 ms et SE 1 s. WIP évolue indépendamment
du bus et des CPU ; seul RDSR reste accepté pendant une opération. WEL est
effacé en fin d'opération. Le mode de sommeil ignore les commandes sauf RDP.
Les délais analogiques de mise sous tension et de réveil ne sont pas modélisés.
Une écriture qui franchit une page reste refusée avant mutation, comme pour le
firmware ; les variantes de débordement de page ne sont pas simulées. Une
adresse incomplète, un programme sans données ou un effacement avec des octets
superflus ne lance pas d'opération. Les mêmes règles de reset, copie, export
différé pendant WIP et abandon des octets tronqués s'appliquent.

`nds_flash_backup` teste les trois capacités, leurs identifiants, les lectures
normales/rapides, la programmation, la réécriture, les effacements, les délais,
le sommeil, la persistance, les paquets invalides et les resets. Deux programmes
synthétiques ARM9/ARM7 attendent WIP puis relisent leur écriture. Un autre test
exécute simultanément une opération de firmware et de sauvegarde : leurs
contenus, protections, horloges et acquittements restent indépendants.

La sélection automatique, les autres fabricants (notamment Sanyo/Macronix),
les flash de 8 Mio et les interfaces infrarouges restent à développer. Aucun
jeu commercial n'est validé par ces tests synthétiques.

## Sauvegardes cartouche FRAM

`BackupType::fram_8k` et `fram_32k` ajoutent deux profils génériques de 8 et
32 Kio selon [GBATEK, DS Cartridge Backup](https://mgba-emu.github.io/gbatek/ds.html#dscartridgebackup).
Leur protocole reprend les commandes EEPROM WREN/WRDI, READ/WRITE avec adresse
16 bits, RDSR et WRSR. Ces profils ne revendiquent pas de fabricant ou
d'identifiant JEDEC : RDID renvoie `ff`, comme les profils EEPROM génériques.
Le choix reste explicite ; 8 Kio peut désigner une EEPROM ou une FRAM et la
taille seule d'un `.sav` ne permet pas de les distinguer.

Il n'y a ni page d'écriture ni délai interne : un flux peut parcourir toute
la mémoire, puis reboucle à sa capacité. Chaque octet modifie la FRAM seulement
à la fin de ses `64 << baud` cycles AUXSPI, et peut déjà être exporté alors
que CS reste bas. Les changements de registre WRSR suivent la même règle.
`Backup::busy()` et WIP restent toujours à zéro ; AUXSPICNT.busy conserve son
sens de transfert série en cours. WEL autorise toute la transaction et est
effacé à CS haut dès qu'un octet WRITE/WRSR a été reçu, ou par WRDI/reset.
Les lectures ne l'effacent pas. Sans WREN, les écritures n'ont pas d'effet.

Les protections du quart supérieur, de la moitié supérieure ou de toute la
mémoire sont vérifiées pour chaque octet, même lorsqu'un flux franchit leur
frontière. Les bits WP/SRWD sont non volatils ; `/W` est haut dans ces profils,
donc SRWD ne verrouille pas seul WRSR. Les API d'image brute, de métadonnée
`status` et d'acquittement par génération sont communes aux autres sauvegardes.
Un octet identique ne change pas la génération ; un acquittement antérieur à
l'octet suivant ne peut pas nettoyer son changement.

Annuler un octet ou réinitialiser le bus conserve les octets FRAM déjà reçus,
y compris ceux de la transaction encore sélectionnée. L'octet inachevé est
abandonné. Ceci diffère des paquets EEPROM/flash dont l'écriture interne ne
démarre qu'à CS haut. Les contrôles sans mutation du bus préparent seulement
le prochain octet ; seule la fin réelle du transfert l'applique. Copier une
machine isole image, session et acquittements. Charger une nouvelle cartouche
retire la FRAM précédente comme les autres sauvegardes.

`nds_fram` teste les capacités, les flux longs, les miroirs, les limites exactes
des octets, les annulations, les copies, les protections, le rechargement et les
quatre débits. Deux programmes synthétiques ARM9/ARM7 écrivent de part et d'autre
d'une ancienne limite de page puis relisent immédiatement, sans attente WIP.
Les variantes propres à un fabricant et les jeux commerciaux restent à valider.

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
à ARM7, prépare les informations de boot, copie les deux programmes et initialise
les deux CPU à leurs points d'entrée. Il accepte
ARM9 dans les 4 Mio canoniques de RAM principale, ARM7 dans cette même RAM,
les 32 Kio canoniques de WRAM partagée ou les 64 Kio canoniques privés. Il refuse
les miroirs comme destinations, les franchissements de région, les programmes
qui se chevauchent et les entrées mal alignées ou dont la première instruction
est tronquée. Un échec préserve mémoire, répartition WRAM, entrées et états CPU précédents.
Un nouveau chargement réussi efface le contenu antérieur de toutes les RAM et
réinitialise registres, banques, drapeaux, compteurs et lignes IRQ/FIQ. Les deux
CPU démarrent en System avec I/F masqués et leurs piles de démarrage. Les images BIOS et firmware déjà fournies
sont conservées. Avec un firmware et des réglages valides, le chargeur prépare
leurs `0x70` octets à `0x027ffc80` avant de charger les exécutables. Un segment
de programme explicitement situé dans cette zone garde ainsi la priorité.

Les `0x170` octets d'en-tête sont copiés à `0x027ffe00` ; les miroirs de la RAM
principale les rendent visibles aux deux CPU. Les blocs `0x027ff800` et
`0x027ffc00` reçoivent chacun les deux identifiants du modèle cartouche et les
CRC déclarés dans la ROM. L'identifiant provient du port cartouche lui-même,
comme la réponse B8. Le chargeur prépare aussi la tâche terminée (`ff810=ffff`,
`ff880=7`, `ff884=6`), l'adresse de chargement ARM7 (`ff860`) et l'indicateur de
boot cartouche normal (`ffc40=1`). Les réglages firmware sont accompagnés de
leur indicateur de validité (`ff864`), de la base des deux copies (`ff868`) et
du CRC part5 déclaré (`ff874`). Ces données précèdent les exécutables ; un
segment fourni dans cette RAM garde la priorité.

Les piles et registres suivent les valeurs de boot cartouche documentées dans
[GBATEK, BIOS RAM Usage](https://mgba-emu.github.io/gbatek/ds.html#biosramusage),
qui diffèrent de celles du SWI SoftReset :

| CPU | SP System | SP IRQ | SP Supervisor |
| --- | --- | --- | --- |
| ARM9 | `0x03002f7c` | `0x03003f80` | `0x03003fc0` |
| ARM7 | `0x0380fd80` | `0x0380ff80` | `0x0380ffc0` |

`r0..r11` sont nuls ; `r12` et LR contiennent l'entrée, avec son bit Thumb pour
l'interworking. Les LR/SPSR IRQ et Supervisor sont nuls. La DTCM ARM9 de 16 Kio
est activée à `0x03000000` pour rendre ses trois piles utilisables, indépendamment
de la WRAM ARM7. Le contrôle CP15 vaut `0x12078`, avec vecteurs hauts, MPU et
caches désactivés. Les programmes peuvent ensuite reconfigurer CP15.
`Cpu::reset_direct_boot()` prépare ces registres ; `Cpu::reset()` reste le reset
minimal à registres nuls de l'interpréteur. `clear_ram()` vide RAM/TCM et remet
CP15 à son état minimal sans reconstruire implicitement les paramètres de boot.

`nds_direct_boot` vérifie la RAM, les miroirs, les identifiants, les réglages
firmware, les banques, le rechargement, les copies et les erreurs sans mutation.
Des programmes ARM/Thumb utilisent leurs piles immédiatement ; des BIOS
synthétiques entrent puis reviennent de SWI/IRQ. Deux programmes simultanés
ARM9/ARM7 lisent les informations de boot depuis la RAM partagée.

Ce chargement reste une préparation expérimentale : il n'exécute pas le BIOS
ou le firmware et ne valide pas les CRC déclarés, le logo ni le chiffrement
KEY1. Les valeurs de débogueur et RTC ne sont pas encore
modélisés ; il ne revendique pas un état post-firmware complet. Les
MMIO LCD, interruption, DMA, timers, entrées, IPC, VRAMCNT et WRAMCNT/WRAMSTAT sont présents.
Les neuf banques VRAM ont des fenêtres CPU réaffectables ; palettes et OAM
standard des deux moteurs sont présentes. Les autres MMIO et
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
Un pas transfère une unité DMA, entre en exception, exécute une instruction
ou reste en attente WFI/HALT.
Un DMA déclenché en attente est prioritaire sur les interruptions et suspend
uniquement le CPU associé ; l'autre CPU n'avance que par ses propres appels.
Le compteur `instructions`
compte uniquement les instructions terminées, dont SWI, UDF et les conditions
ARM fausses. Une unité DMA, entrée d'exception ou attente ne l'incrémente pas.
Les API de pas explicites ne calculent pas de durée et ne font pas avancer les
timers/LCD. `run_cycles()` et `run_frame()` proposent désormais une exécution
commune des deux CPU et périphériques, décrite ci-dessous.

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
Le CP15 et les TCM ARM9 sont partiellement disponibles (voir ci-dessous) ;
les caches physiques et une partie des instructions propres à ARMv5 restent à développer.
Ce sous-ensemble commun ne constitue pas encore un
ARM946E-S ou un ARM7TDMI complet. L'interpréteur lit directement la RAM à chaque
pas ; il ne reproduit pas le pipeline ni le comportement du code automodifiant.

`BL` Thumb se déroule en deux pas : le préfixe place la partie haute signée de
la cible dans LR, puis le suffixe branche et fixe l'adresse de retour Thumb.
Un arrêt entre les deux conserve ce LR intermédiaire. Les formes `BLX` ARMv5
sont disponibles uniquement sur ARM9 ; elles lèvent une erreur sur ARM7.
`LDR pc`, `LDM ...,{pc}` et `POP {pc}` échangent ARM/Thumb selon le bit 0 de
l'adresse sur ARM9 quand CP15.L4 est désactivé, tandis qu'ARM7 conserve son mode courant. Les adresses
sont alignées selon le mode obtenu. Hors retours d'exception, les calculs de
PC, LR, SP et les transferts ne modifient pas NZCV. Référence : ARM Architecture Reference Manual, ARMv4T /
ARMv5T, descriptions de BL, BLX, LoadWritePC, LDM/STM et PUSH/POP.

## CP15 ARM9 et mémoires TCM

`Memory::cp15()` expose le sous-ensemble `Cp15`, déclaré dans
`<ravenemu/nds/cp15.hpp>`. Les instructions privilégiées ARM `MRC/MCR p15`
(opcode1=0) accèdent aux identifiants `c0,c0,0..7`, au contrôle `c1,c0,0`
et aux régions DTCM/ITCM `c9,c1,0/1`. Les identifiants décrivent ARM946E-S,
ses caches et ses TCM physiques de 32 Kio (instructions) et 16 Kio (données).
`MRC pc` remplace seulement NZCV. Un accès CP15 en User ou sur ARM7 entre en
exception Undefined ; les registres ou variantes non implémentés restent des
diagnostics sans mutation. Les écritures activant big-endian sont explicitement
refusées. Les bits I/D cache sont mémorisés, mais les accès restent cohérents et
directs : aucune ligne de cache physique n'est émulée. Les commandes ARM946 de
maintenance documentées sont acceptées sans effet (invalidations I/D entières
ou par adresse, nettoyage D par adresse/index, nettoyage-invalidation D par
adresse/index et préchargement I). Elles n'accèdent pas au bus. Les deux formes
WFI ARM946 sont interprétées par le CPU (voir l'ordonnanceur ci-dessous), tandis
qu'un appel natif à `Cp15::write` reste un mutateur de registres sans suspension CPU.

ITCM est fixée à zéro ; DTCM a une base programmable, que le logiciel doit
aligner sur sa taille virtuelle. Les tailles documentées de 4 Kio à 4 Gio sont
acceptées, les tailles réservées sont refusées à l'écriture. Les fenêtres plus
grandes que la mémoire physique produisent des miroirs ; les petites n'exposent
que leur préfixe. ITCM a priorité sur DTCM pour les accès qui la sélectionnent.
Le mode de chargement détourne seulement les écritures vers TCM : les lectures
utilisent la mémoire sous-jacente. `Memory::fetch16/32` consulte ITCM mais ignore
DTCM ; les lectures/écritures CPU ordinaires et multiples consultent les deux.
ARM7 et DMA ignorent toujours les TCM, même lorsqu'elles masquent de la RAM ou
un registre MMIO pour ARM9. Le CPU reste suspendu pendant les unités DMA dans
le modèle actuel ; l'exécution parallèle depuis TCM n'est pas encore reproduite.

Le contrôle sélectionne les vecteurs bas/hauts (bit V) et l'interworking des
chargements vers PC (bit L4). L'état minimal de `Memory` / `clear_ram()` conserve
les vecteurs hauts (`0x2078`), désactive les TCM et laisse leurs régions à zéro.
Le chargement direct de `Machine` vide aussi les TCM puis active la DTCM des
piles de boot à `0x03000000` (`0x12078`), sans prétendre reproduire tout le
démarrage BIOS/firmware. `Cpu::reset` réinitialise les registres du CPU seulement.
Un chargement refusé préserve CP15 et ses octets. Les copies de machine
possèdent leurs propres registres et octets TCM.
Références : [GBATEK, CP15](https://mgba-emu.github.io/gbatek/ds.html#armcp15systemcontrolcoprocessor),
[contrôle](https://mgba-emu.github.io/gbatek/ds.html#armcp15controlregister) et
[TCM](https://mgba-emu.github.io/gbatek/ds.html#armcp15tightlycoupledmemorytcm).
Les durées des instructions, les caches et les attentes de bus restent à faire.

## Ordonnanceur et attente d'interruption

`Machine::run_cycles(n)` exécute un budget de cycles communs à
`timer_clock_hz` (33 513 982 Hz). Chaque cycle offre successivement un pas ARM9,
un pas ARM7, un second pas ARM9 puis une avance d'un cycle des timers/LCD,
FIFO et capture. Les horloges nominales `arm9_clock_hz` et `arm7_clock_hz`
conservent le rapport 2:1 ; un événement produit par ARM7 peut donc être observé
par le second pas ARM9 du même cycle. Les IRQ et DMA produits en fin de cycle
sont observés aux créneaux suivants. Le DMA ne suspend que son CPU associé.

Ce premier ordonnanceur est un modèle fonctionnel déterministe, pas une mesure
des durées ARM : une instruction, unité DMA, entrée d'exception ou réveil coûte
un créneau du CPU associé. Les attentes RAM/VRAM/cartouche, conflits de bus,
pipeline, caches et durées propres aux instructions ne sont pas encore calculés.
Le nombre d'instructions par seconde obtenu ne constitue pas un timing matériel
validé. Chaque cycle est traité individuellement pour conserver les événements
et écritures des deux CPU ; aucune accélération par saut de CPU inactifs n'est
encore appliquée.

`Machine::run_frame()` termine le balayage LCD en cours ; à sa phase zéro, il
exécute une image complète de 560190 cycles. Il utilise la phase LCD réelle,
y compris après une avance manuelle. `render_frame()` reste un instantané sans
effet. Les API `step`, `run_steps` et `advance_cycles` conservent leurs contrats
séparés et n'avancent pas le compteur de l'ordonnanceur.

```cpp
machine.run_cycles(1000); // deux CPU, DMA, timers et vidéo
machine.run_frame();
const auto frame = machine.render_frame();
const auto clock = machine.scheduler_state(); // cycles terminés et phase en cours
```

`SchedulerState`, déclaré dans `<ravenemu/nds/scheduler.hpp>`, conserve la phase
`arm9_first`, `arm7`, `arm9_second` ou `peripherals`. Une erreur laisse cette
phase en attente et préserve tout le travail précédent. Après correction de
l'opcode, du DMA ou de la source vidéo, `run_cycles()` reprend cette phase sans
rejouer les CPU déjà exécutés. Son budget compte les cycles communs terminés,
y compris le cycle partiel repris. Un budget nul n'effectue aucun travail ;
un budget qui ferait déborder le compteur est refusé avant tout effet. La copie
conserve une phase indépendante ; un chargement réussi remet l'horloge à zéro,
un chargement refusé la préserve.

Les instructions privilégiées `MCR p15,0,Rd,c7,c0,4` et `c7,c8,2` terminent puis
activent `CpuState::waiting_for_interrupt`. Les prochains pas ARM9 ne lisent
plus d'instruction jusqu'à une ligne IRQ/FIQ. La ligne IRQ du contrôleur dépend
de IME et IE/IF sur ARM9 ; le masque I du CPSR ne bloque pas le réveil. Si IRQ/FIQ
est démasquée, le CPU entre en exception ; sinon un pas enlève l'attente et le
pas suivant reprend le code. Les lignes externes sont des entrées physiques
déjà affirmées. Un CPU en attente continue de servir ses unités DMA ; dans
`run_cycles`, ARM7 et les horloges continuent. WFI conditionnelle ignorée ne
suspend pas, et les accès en User ou sur ARM7 entrent en Undefined. Le reset CPU
ou un chargement réussi efface l'attente. HALTCNT ARM7 est décrit ci-dessous ; Sleep reste absent.
Références : [GBATEK, HALTCNT et attente ARM9](https://mgba-emu.github.io/gbatek/ds.html#4000301hnds7haltcntlowpowermodecontrolrw),
[commandes CP15](https://mgba-emu.github.io/gbatek/ds.html#armcp15cachecontrol).

## POSTFLG et HALT ARM7

`0x04000300` expose deux POSTFLG indépendants. Le bit 0 indique la fin du boot
et ne peut être effacé que par `Memory::clear_ram()`. ARM9 peut aussi modifier
son bit 1. ARM7 peut seulement fixer le bit 0 depuis une instruction située
dans ses 16 Kio de BIOS ; les écritures depuis la RAM ou l'API hôte sont
ignorées. Le bus de données transporte l'adresse d'origine de l'instruction,
sans conserver d'autorisation d'un accès BIOS précédent. Cette règle couvre
les stores ARM/Thumb 8/16/32 bits et STM. Le chargement direct de `Machine`
initialise les deux flags à un, sans exécuter de BIOS. Les bits réservés sont
nuls, et les octets ARM9 `0x301..303` sont sans effet. La redirection du vecteur
de reset vers un débogueur Nintendo n'est pas implémentée.

`0x04000301` expose HALTCNT sur ARM7. Le mode `80` termine l'instruction qui
l'écrit puis active `CpuState::waiting_for_interrupt`. Les bits bas sont
réservés ; le mode zéro ne demande aucune action. Le registre conserve la
dernière sélection et la demande HALT est consommée une seule fois par le CPU.
Un mode zéro ultérieur n'annule pas une demande déjà acceptée. Les écritures
hôte peuvent aussi demander HALT ; celui-ci devient visible dans l'état du
CPU à son prochain pas, après les éventuelles unités DMA prioritaires.

HALT suspend uniquement les instructions ARM7 : DMA, timers, LCD et ARM9
continuent. Le réveil matériel teste `IE & IF` sans IME, contrairement au WFI
ARM9. IME et CPSR.I conservent leur rôle pour entrer en IRQ : un événement peut
donc réveiller ARM7 avec IME à zéro sans exécuter de gestionnaire. Un réveil
masqué consomme un pas sans instruction, comme WFI. Les lignes externes IRQ/FIQ
de l'API CPU restent des entrées physiques affirmées. Un DMA terminé peut
réveiller ARM7 sur le pas suivant grâce à son IF. Un reset CPU efface son
attente ; `clear_ram()` remet aussi flags, HALTCNT et demande en attente à zéro.
Les copies isolent ces états et un chargement refusé les conserve.

Les modes `40` (GBA) et `c0` (Sleep de la console) restent diagnostiqués avant
mutation. Une écriture combinée POSTFLG/HALTCNT ou un STM refusé ne fixe donc
aucun flag et ne laisse aucune demande HALT. Le modèle ne simule pas l'arrêt
global des périphériques propre à Sleep. Les SWI peuvent fonctionner avec un
BIOS fourni ; aucun code BIOS tiers ni remplacement HLE n'est ajouté.

`nds_power` vérifie les masques, les bits persistants, les origines BIOS/RAM,
les quatre familles de stores, les erreurs transactionnelles, les copies, les
resets, IE/IF/IME/CPSR et les DMA pendant HALT. Un programme ARM7 est réveillé
par un timer sous l'ordonnanceur tandis qu'ARM9 continue. Un BIOS synthétique
exécute HALT pendant un SWI puis revient au programme après réveil. Ces tests
originaux suivent [GBATEK, POSTFLG](https://mgba-emu.github.io/gbatek/ds.html#4000300hnds7nds9postflgbytepostbootflagrw)
et [HALTCNT](https://mgba-emu.github.io/gbatek/ds.html#4000301hnds7haltcntlowpowermodecontrolrw).

## Protection mémoire MPU ARM9

Le bit M du contrôle CP15 active les huit régions unifiées `c6,c0..7,0/1` ;
les deux sélecteurs sont des alias sur ARM946. Chaque région peut être désactivée
ou couvrir une taille de 4 Kio à 4 Gio. Les bits réservés sont masqués ; une
région active avec taille réservée ou base non alignée est refusée sans mutation.
La région de plus grand numéro gagne, sans repli vers une région inférieure
si ses permissions refusent l'accès. L'arrière-plan est interdit même en System.
Les adresses virtuelles du CPU sont testées avant leurs miroirs physiques.

Les AP données et instructions sont distinctes : `c5,c0,0/1` expose huit
champs de 2 bits ; `c5,c0,2/3` les expose sur 4 bits. Une écriture compacte
remplace tous les champs étendus correspondants, une lecture compacte restitue
leurs deux bits bas. AP=0 interdit tout, AP=1 réserve R/W au privilégié, AP=2
ajoute R à User, AP=3 autorise R/W aux deux modes, AP=5 réserve R au privilégié,
AP=6 autorise R aux deux. Les AP réservées sont refusées à l'écriture.
Les attributs cacheables `c2,c0,0/1` et bufferables `c3,c0,0` conservent leurs
huit bits, sans créer de caches ou de tampon d'écriture physiques.

Une vue de bus privée à l'interpréteur vérifie les données et TCM avant tout
effet MMIO ; le fetch vérifie séparément les AP instructions avec le privilège
courant. Les API natives `Memory::read/write/peek/fetch` restent des outils
d'inspection/chargement sans MPU. ARM7, DMA et les accès vidéo sont indépendants.
`Cp15::permits(address, Access, privileged)` permet d'inspecter une permission
sans lire le bus ni modifier les registres.

Une faute de fetch entre en Prefetch Abort (`+0x0c`, LR=PC+4) ; une faute de
données entre en Data Abort (`+0x10`, LR=PC+8), en ARM comme en Thumb. Le CPU
sauvegarde CPSR dans SPSR_abt, sélectionne la banque Abort, masque IRQ et utilise
le vecteur bas/haut choisi par CP15.V. L'instruction fautive ne termine pas et
n'incrémente pas son compteur. Un gestionnaire peut modifier CP15 puis retenter
l'accès avec `SUBS pc,lr,#4/8`. Les transferts multiples conservent actuellement
la validation transactionnelle du cœur : une faute MPU ne valide aucun mot ni
registre. Cela ne reproduit pas les transferts partiels possibles sur matériel.
Les erreurs de bus non implémenté/non aligné restent des diagnostics, pas des
aborts externes. Ces limites, les caches et les timings fins restent à développer.
La copie et le rechargement suivent le même contrat que les autres registres CP15.
Référence : [GBATEK, ARM CP15 Protection Unit](https://mgba-emu.github.io/gbatek/ds.html#armcp15protectionunitpu).

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
sur ARM9 et `0x01df3fff` sur ARM7 : géométrie sur ARM9, RTC/capot/SPI/Wi-Fi sur
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
ainsi que DMAFILL ARM9 en source, MOSAIC A/B, DISPCAPCNT et DISP_MMEM_FIFO en destination ARM9. Les quatre registres
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
Le mode ARM9 4 (mémoire principale vers FIFO vidéo) est décrit plus bas.
Les modes cartouche, géométrie et début d'affichage (ARM9 3) restent armés sans déclencheur.

Avec repeat en VBlank/HBlank, chaque événement démarre un bloc de la longueur
programmée. La source continue d'un bloc à l'autre ; la destination continue
aussi, sauf en mode incrément/rechargement où elle reprend DAD. La longueur et
le DAD de rechargement sont relus au prochain déclenchement. Un événement reçu
pendant qu'un bloc est en attente ou en cours est ignoré, sans file de reprises.
Repeat en immédiat conserve le comportement ponctuel.

Les pas DMA explicites n'avancent pas les timers/LCD ; l'ordonnanceur avance
l'horloge commune après ses créneaux CPU/DMA. Copier une machine
copie aussi les canaux suspendus ; un chargement réussi ou `clear_ram()` remet
canaux et DMAFILL à zéro. Référence :
[GBATEK, DS DMA Transfers](https://mgba-emu.github.io/gbatek/ds.html#dsdmatransfers).

## Horloge LCD

`LcdTiming`, dans `<ravenemu/nds/lcd_timing.hpp>`, suit le balayage commun aux
deux écrans : 256 pixels visibles par ligne, 192 lignes visibles, 355 périodes
de pixel par ligne et 263 lignes par image. Un pixel dure six cycles de
l'horloge à `timer_clock_hz`, soit 2130 cycles par ligne et 560190 par image.
Le modèle démarre à la ligne zéro, cycle zéro. L'horloge effectue les écritures
VRAM d'une capture armée ; `render_frame()` fournit séparément un instantané
des moteurs A/B sans effet sur les horloges ni la capture.

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
LCD, timers, FIFO vidéo et capture, sans exécuter de CPU ni d'unité DMA. Les événements
IRQ/DMA sont coalescés sur l'intervalle demandé. Sans capture ni FIFO, un grand appel
traverse plusieurs images sans boucle par cycle ni par ligne. Avec capture,
seules ses lignes sont traitées individuellement. La FIFO vidéo ajoute des
étapes de huit pixels dans les lignes visibles ; les images entières suivantes
sont coalescées une fois son contenu stable pendant un grand appel. Aucun appel n'exécute les répétitions DMA.
Pour traiter un bloc à chaque HBlank/VBlank, utiliser `run_cycles()`/`run_frame()`
ou alterner explicitement les avances jusqu'aux événements et les pas CPU/DMA.
`step()` et `run_steps()` conservent leur budget de pas sans durée matérielle ;
les latences fines IRQ/DMA restent à développer.

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
mode FIFO 3 affiche les pixels déjà consommés par l'horloge LCD, comme décrit
plus bas. La capture 2D/VRAM/FIFO et la luminosité maître sont également
décrites plus bas ; la 3D reste absente.
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

La mosaïque et les palettes étendues sont décrites plus bas. Le moteur ne
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

## Mosaïque 2D

Chaque moteur possède son registre `MOSAIC`, à `0x0400004c` pour A et
`0x0400104c` pour B. Les quatre champs de 4 bits indiquent les tailles moins
un : BG horizontal/vertical dans les bits 0–3/4–7, OBJ horizontal/vertical
dans les bits 8–11/12–15. Les tailles vont donc de 1 à 16 ; zéro donne 1×1.
Les écritures ARM9 sur 8/16/32 bits sont acceptées, les 16 bits hauts sont
ignorés. Le registre est en écriture seule : lectures et inspections renvoient
zéro. POWCNT1 protège les écritures de chaque moteur indépendamment. Les DMA
ARM9 peuvent aussi écrire ces ports sur 16/32 bits ; les autres restrictions
actuelles des destinations DMA restent applicables.

BGnCNT bit 6 active l'effet par fond. La grille commence au coin de l'écran,
avant le défilement et les transformations affines : chaque bloc utilise la
coordonnée de son coin supérieur gauche. Cela s'applique aux fonds texte
4/8 bpp, affines, affines étendus, bitmaps indexés/directs et grand bitmap A.
Les palettes standard ou étendues et les règles de transparence sont ensuite
celles du mode utilisé. Les fenêtres, priorités et effets de couleur continuent
de se calculer pour chaque pixel affiché, même au milieu d'un bloc.

L'attribut OAM bit 12 active la mosaïque OBJ. La sélection verticale utilise
une grille écran avant les flips ou la matrice ; le premier bloc partiel au
sommet de la boîte est borné à sa première ligne. Le clipping et le wrap
NDS restent applicables. L'axe horizontal traite ensuite la couche OBJ après
la sélection de ses gagnants. À l'intérieur d'un bloc, une activation différente
ou une priorité BG plus haute commence un nouveau segment. La couleur et
son alpha sont répétés ensemble, y compris la transparence ; un OBJ sans
mosaïque reste intact et la répétition ne déborde pas sur une zone sans OBJ.
Les sprites servant de fenêtre OBJ ignorent la mosaïque sur les deux axes.

Les deux registres sont copiés ou remis à zéro avec la mémoire vidéo. Les
paramètres suivent le moteur lors de la permutation des écrans. Le modèle
reste un instantané : il ne reproduit pas les compteurs internes ni les
changements de taille ou d'activation pendant le balayage. La 3D et l'affichage
VRAM direct ne reçoivent pas cette mosaïque 2D. Les tests utilisent des scènes
et une ROM synthétiques, sans validation sur console NDS physique.
Références : [GBATEK, mosaïque LCD](https://mgba-emu.github.io/gbatek/gba.html#lcdiomosaicfunction)
et [moteurs vidéo DS](https://mgba-emu.github.io/gbatek/ds.html#dsvideo).

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

## Capture vidéo vers la VRAM

`DISPCAPCNT`, à `0x04000064` sur ARM9, contrôle la capture du moteur A.
Le registre accepte les accès 8/16/32 bits et les écritures DMA ARM9, avec
masque `0xef3f1f1f`. Il reste accessible quand les moteurs 2D sont éteints.
Le moteur B ne possède pas de registre de capture séparé.

| Bits | Fonction |
| --- | --- |
| 0–4, 8–12 | Coefficients EVA/EVB, saturés à 16 lors du mélange |
| 16–17 | Banque destination A, B, C ou D, affectée en LCDC |
| 18–19 | Offset destination : 0, 32, 64 ou 96 Kio |
| 20–21 | Taille : 128×128, 256×64, 256×128 ou 256×192 |
| 24 | Source A : composition 2D ou 3D seule (cette dernière reste non prise en charge) |
| 25 | Source B : VRAM ou FIFO vidéo |
| 26–27 | Offset source VRAM, ignoré en affichage VRAM direct |
| 29–30 | Source A seule, B seule, ou A+B (modes 2 et 3) |
| 31 | Activation/occupé ; effacé automatiquement à la ligne 192 |

La source A prise en charge est la composition 2D du moteur A : fonds,
sprites, fenêtres, mosaïque et effets BLDCNT, avant `MASTER_BRIGHT` et avant
la sélection de l'affichage. Un affichage VRAM direct, une permutation des
écrans ou les paramètres du moteur B ne remplacent donc pas cette source.
La sortie composée est opaque, backdrop compris, et forced blank fournit du
blanc. Un moteur 2D A éteint fournit du blanc dans ce modèle.

Quand le bit 25 est nul, la source B lit la banque A–D choisie par DISPCNT
bits 18–19, en LCDC.
Son stride reste de 256 pixels même pour une capture large de 128 pixels ;
la destination utilise la largeur de capture. Les offsets et adresses font
le tour de la banque de 128 Kio. Une source non affectée en LCDC fournit zéro ;
une destination non affectée en LCDC ne reçoit aucune écriture. Les
réaffectations et modifications de VRAM entre deux lignes sont prises en compte.

Les copies simples préservent les 16 bits du pixel, dont le bit 15 de validité.
Le mélange ignore la couleur d'une entrée dont ce bit est nul, puis calcule
chaque canal RGB555 par `(A*EVA+B*EVB)/16`, avec saturation à 31. Le bit 15
résultant vaut un si au moins une entrée valide possède un coefficient non nul.
Les calculs restent en RGB555 entier, sans les arrondis du chemin interne RGB666.
Les deux sources d'une ligne sont lues avant son écriture ; les lignes suivantes
peuvent relire des données capturées, y compris dans la même banque.

L'activation attend la ligne zéro. Si elle est programmée au cycle zéro initial,
la première avance positive peut démarrer immédiatement. Chaque ligne est
écrite à la fin de ses 1536 cycles visibles, avant le déclenchement DMA HBlank.
Le bit occupé demeure actif jusqu'à la ligne 192, même pour une taille plus petite.
L'effacer arrête la capture dans ce modèle. `render_frame()` et les inspections
mémoire ne déclenchent aucune capture. Un grand intervalle de cycles traite au
plus une capture ponctuelle, puis rejoint le chemin temporel coalescé.

La source A 3D seule est refusée par `ravenemu::Error` uniquement si elle
est utilisée et la destination allouée. La source B FIFO est prise en charge. Les bits d'une
source inutilisée restent sans effet. `advance_cycles()` peut donc maintenant
lever une erreur : les lignes déjà terminées restent écrites, mais la ligne
fautive et ses cycles ne sont pas appliqués. Le programme peut corriger les
paramètres ou désactiver la capture. Copies et chargements conservent ou remettent
à zéro les paramètres, l'état en attente/en cours et les octets VRAM.

Ce modèle échantillonne les données à la fin de chaque ligne ; les lectures
et écritures pixel par pixel et les compteurs internes de mosaïque restent à
développer. Les tests sont synthétiques, sans validation sur console physique.
Références : [GBATEK, capture DS](https://mgba-emu.github.io/gbatek/ds.html#dsvideocaptureandmainmemorydisplaymode)
et [circuit vidéo DS](https://mgba-emu.github.io/gbatek/ds.html#dsvideodisplaysystemblockdiagram).

## FIFO vidéo et DMA mémoire principale

`DISP_MMEM_FIFO`, à `0x04000068` sur ARM9, alimente le mode d'affichage A 3
et la source B de capture lorsque DISPCAPCNT bit 25 est actif. Une écriture
32 bits fournit deux pixels RGB555, le premier dans les bits bas. Le port
se lit à zéro sans retirer de données, y compris avec les inspections du bus.
Les écritures 8/16 bits ne sont pas encore prises en charge et lèvent une
erreur avant tout effet ; ARM7 et le port B inexistant restent non mappés.

Le modèle utilise un anneau de huit mots, soit seize pixels. Ses emplacements
retiennent leurs valeurs : en cas de sous-alimentation, ils sont relus à leur
prochain passage ; trop d'écritures sans consommation remplacent les anciennes
valeurs. Il n'existe pas de drapeaux plein/vide comme pour IPC. À VBlank, les
pointeurs reviennent à zéro sans effacer ces mots. Un reset ou chargement réussi
efface aussi l'anneau et l'image consommée ; les copies possèdent leur état propre.

Le flux démarre au début d'une image si le mode A 3, une capture utilisant
la FIFO ou un DMA vidéo ARM9 est armé. Une activation en milieu d'image attend
le prochain passage à la ligne zéro. La décision reste active jusqu'à VBlank.
Chaque groupe de huit pixels est consommé après 48 cycles visibles, sur les
256 pixels des 192 lignes ; une capture large de 128 utilise la moitié gauche
mais ne réduit pas cette consommation. Le dernier groupe est préparé avant
la capture de la ligne, sans être retiré si la composition échoue.

L'affichage et la capture partagent les mêmes pixels échantillonnés. Le bit 15
est ignoré à l'affichage mais préservé pour la copie ou le mélange en capture.
Le mode 3 contourne les fonds, fenêtres et effets BLDCNT ; MASTER_BRIGHT et
la permutation des écrans restent appliqués. Il fonctionne même lorsque le
moteur 2D A est éteint. `render_frame()` lit uniquement l'image déjà consommée,
initialement noire : il n'alimente ni ne vide la FIFO. Une inspection en cours
de balayage peut donc montrer un préfixe récent et le reste de l'image précédente.

Le mode DMA ARM9 4 se déclenche par demandes de quatre mots, à la première
avance positive de chaque ligne visible puis après chaque groupe sauf le
dernier. Il requiert des mots 32 bits depuis la RAM principale, avec destination
`0x04000068` fixe. La longueur programmée est conservée séparément du quota de
rafale : un transfert de 24576 mots fournit une image entière, tandis qu'une
longueur de quatre avec repeat réarme un bloc à chaque demande. Le pointeur
source continue entre les rafales ; l'IRQ arrive à la fin de la longueur, pas
à la fin de chaque quota. VBlank suspend les rafales non servies. Le prochain
début d'image peut reprendre un canal encore armé ; aucun rembobinage source
implicite n'est effectué. Les autres canaux conservent leurs priorités.

L'horloge déclenche les demandes mais n'exécute pas les unités DMA. Pour un flux
complet, alterner l'avancée LCD et les pas CPU/DMA. Par exemple, après avoir
configuré SAD vers une image en RAM, DAD vers la FIFO et CNT en mode 4, destination
fixe, mots 32 bits et longueur 24576 :

```cpp
using ravenemu::nds::Processor;
using ravenemu::nds::LcdTiming;
for (unsigned y = 0; y < 192; ++y) {
    machine.advance_cycles(1); // première demande de la ligne
    for (unsigned group = 0; group < 32; ++group) {
        machine.run_steps(Processor::arm9, 4); // quatre unités DMA, CPU suspendu
        machine.advance_cycles(group == 0 ? 47 : 48);
    }
    machine.advance_cycles(LcdTiming::line_cycles - 1536);
}
```

Les accès CPU et DMA restent sans durée propre ; le préchargement et les
fronts matériels fins ne sont pas encore reproduits. La cadence de huit pixels
est un modèle par groupes, vérifié sur scènes et ROM synthétiques. Un grand
appel sans intervention CPU/DMA peut coalescer les images entières après deux
passages à VBlank, sans changer les pixels, pointeurs, IRQ ou durées observables.
Références : [GBATEK, FIFO d'affichage](https://mgba-emu.github.io/gbatek/ds.html#dsvideocaptureandmainmemorydisplaymode)
et [DMA NDS](https://mgba-emu.github.io/gbatek/ds.html#dsdmatransfers).

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
des latences NDS. L'ordonnanceur avance les timers ; les timings CPU restent à faire.
Référence : [GBATEK, DS Timers](https://mgba-emu.github.io/gbatek/ds.html#dstimers).

## Boutons, capot et état du stylet

`Input`, `InputState` et l'enum DS `Key` sont déclarés dans
`<ravenemu/nds/input.hpp>`. L'interface hôte `Machine::set_key(Key, pressed)`
change un bouton, `set_input(InputState)` applique atomiquement l'ensemble des
entrées. Les mêmes méthodes sont disponibles sur `Memory`. `input_state()`
expose une vue constante. `pressed_keys` utilise des bits actifs à un dans
l'ordre A, B, Select, Start, Right, Left, Up, Down, R, L, X, Y ; les quatre bits
hauts sont ignorés. Un enum hors de ces douze boutons est refusé sans mutation.
L'enum commun `ravenemu::Button` et les autres cœurs ne sont pas modifiés.

| Registre | CPU | Comportement |
| --- | --- | --- |
| `0x04000130` KEYINPUT | ARM9/ARM7 | Dix boutons historiques communs, actifs à zéro ; bits 10..15 nuls |
| `0x04000132` KEYCNT | ARM9/ARM7 | Contrôle indépendant, masque `0xc3ff`, sélection de dix boutons, IRQ et OR/AND |
| `0x04000136` EXTKEYIN | ARM7 | X/Y bits 0/1, DEBUG bit 3 et /PENIRQ bit 6 actifs à zéro ; capot fermé bit 7 actif à un |

KEYINPUT et EXTKEYIN sont en lecture seule. Les voies d'octets et les accès
16 bits respectent little-endian ; le mot à KEYINPUT combine aussi KEYCNT.
Les bits 2/4/5 d'EXTKEYIN se lisent à un, son octet haut à zéro. EXTKEYIN reste
non mappé sur ARM9 ; RCNT et RTC voisins ne sont pas introduits implicitement.

KEYCNT demande IF.12 sur le passage de sa combinaison de faux à vrai, avec
son autorisation IRQ activée. OR exige au moins un bouton sélectionné ; AND les
exige tous. L'historique de condition est séparé pour chaque CPU. Un appui
maintenu, une lecture ou une avance d'horloge ne redemande pas l'IRQ après
acquittement. Une écriture KEYCNT change la référence de comparaison sans
produire de front ; activer une combinaison déjà maintenue ne produit donc
pas de nouvelle demande. Une sélection vide reste constante et ne produit
aucun front. X/Y, DEBUG et le stylet ne sont pas sélectionnables par KEYCNT.

L'ouverture après fermeture du capot demande IF.22 sur ARM7 seulement ;
répéter le même état ne redemande pas d'IRQ. Le masque ARM7 inclut désormais
ce bit (`0x01df3fff`) : l'ancien masque excluait à tort la source capot.
Comme les autres sources, les demandes sont mémorisées même si IME/IE sont
coupés ; une IRQ clavier activée peut réveiller WFI ARM9 via l'ordonnanceur.
Aucun mode Sleep ni arrêt matériel des écrans n'est encore associé au capot.

```cpp
machine.set_key(ravenemu::nds::Key::a, true);
machine.run_cycles(1000);
auto input = machine.input_state();
input.lid_closed = true;
machine.set_input(input);
```

`pen_down` fournit le contact physique. La ligne /PENIRQ d'EXTKEYIN tient aussi
compte du mode d'alimentation du contrôleur tactile décrit ci-dessous ; aucune
IRQ tactile supplémentaire n'est inventée.
La copie conserve indépendamment entrées, contrôles et historique ; un
chargement refusé les préserve. `clear_ram()` ou un chargement réussi remet
boutons et stylet relâchés, DEBUG relâché et capot ouvert, sans fausse IRQ
d'ouverture. L'hôte doit ensuite fournir son état courant.

Les bits et sources viennent de [GBATEK, DS Keypad](https://mgba-emu.github.io/gbatek/ds.html#dskeypad)
et [KEYCNT](https://mgba-emu.github.io/gbatek/gba.html#4000132hkeycntkeyinterruptcontrolrw).
Les transitions d'IRQ ont aussi été comparées au comportement de
[melonDS](https://github.com/melonDS-emu/melonDS/blob/master/src/NDS.cpp),
sans importer son code. Cette vérification ne remplace pas une mesure sur
console ; les latences fines des entrées restent non validées matériellement.

## Bus SPI ARM7 et contrôleur tactile

`Spi` et `Touchscreen` sont déclarés dans `<ravenemu/nds/{spi,touchscreen}.hpp>`.
Ils possèdent leur état et restent copiables sans référence au bus ni aux CPU.
L'implémentation C++ est originale, écrite à partir de
[GBATEK, DS SPI](https://mgba-emu.github.io/gbatek/ds.html#dsserialperipheralinterfacebusspi)
et [DS Touch Screen Controller](https://mgba-emu.github.io/gbatek/ds.html#dstouchscreencontrollertsc),
sans importer de code d'émulateur tiers.

| Registre | CPU | Comportement |
| --- | --- | --- |
| `0x040001c0` SPICNT | ARM7 | Débit, busy en lecture seule, périphérique, maintien CS, IRQ, activation ; masque `0xcf03` |
| `0x040001c2` SPIDATA | ARM7 | Écrire l'octet bas lance un échange ; octet haut toujours nul et sans strobe |

Les accès 8/16 bits et le mot combiné contrôle/données sont pris en charge.
Le mot est validé avant mutation et applique le nouveau contrôle au strobe.
SPI reste non mappé sur ARM9. Chaque octet dure `64 << débit` cycles communs,
soit huit horloges série divisées par 8/16/32/64 à partir de l'horloge ARM7.
Busy reste actif jusqu'à la fin ; la donnée précédente reste lisible pendant
le transfert. `advance_cycles`, `run_cycles` et `run_frame` avancent le bus ;
`step` et `run_steps` seuls ne font pas avancer son horloge.
La fin demande IF.23 sur ARM7 si l'IRQ est autorisée, même avant activation
d'IME/IE. Une lecture ou un acquittement ne répète pas cette demande.

Le périphérique 2 utilise le protocole TSC2046 de la DS originale : commande
MSB-first, bit muet, ADC 12 ou 8 bits puis bourrage nul. Les commandes peuvent
commencer pendant les six derniers bits du résultat précédent, y compris
sans alignement à l'octet. Maintenir CS conserve ce flux ; retirer hold avant
le dernier transfert termine d'abord ce transfert, puis désélectionne le TSC.
Changer de périphérique ou désactiver SPI abandonne le flux. La désactivation
annule aussi un transfert en attente sans IRQ. Les autres écritures pendant
busy sont ignorées dans ce modèle ; ce cas n'est pas mesuré sur console.

`InputState::touch` contient un `TouchSample` avec les mesures brutes `x`, `y`,
`z1`, `z2`. Leurs quatre bits hauts sont ignorés, comme pour un ADC 12 bits.
Ce ne sont pas des pixels : le logiciel doit utiliser son étalonnage firmware.
Les valeurs de pression sont fournies explicitement par l'hôte, sans prétendre
simuler une résistance électrique. Sans contact, X/Z1 donnent 0 et Y/Z2 `0xfff`.
La batterie donne 0 (entrée câblée à GND). Microphone et températures donnent
provisoirement 0 : aucun modèle audio/thermique n'est encore implémenté.

```cpp
auto input = machine.input_state();
input.pen_down = true;
input.touch = {0x800, 0x600, 0x400, 0xc00}; // mesures ADC, pas des pixels
machine.set_input(input);
```

Les mesures sont échantillonnées au départ de chaque transfert ; une commande
terminée applique sa nouvelle alimentation et le résultat reste mémorisé
pendant sa lecture. PD0 à un désactive /PENIRQ ; PD0 à zéro l'autorise si
`pen_down` est vrai. CS n'efface pas le mode d'alimentation. Le modèle vise
la DS originale ; les variantes DS Lite, le contrôleur DSi, le bruit et les
temps analogiques fins restent hors périmètre.

Le périphérique réservé et le mode de transfert SPI 16 bits restent refusés
au strobe actif, avec un diagnostic et sans mutation. Le périphérique firmware
est décrit ci-dessous. Le SPI auxiliaire des EEPROM, flash et FRAM cartouche est décrit plus
haut ; le DMA SPI reste à développer. Copie, `clear_ram()` et rechargement conservent les mêmes
contrats que les autres périphériques, sans IRQ différée après reset.

## Powerman, batterie et extinction

Le périphérique SPI 0 émule le **Mitsumi 3152A de la DS originale**, dans
`PowerManagement` (`<ravenemu/nds/power_management.hpp>`). Cette implémentation
C++ originale suit [GBATEK, DS Power Management Device](https://mgba-emu.github.io/gbatek/ds.html#dspowermanagementdevice),
sans code ni dépendance d'un autre émulateur. Un premier octet sélectionne
l'index (bits 0..6) et la direction (bit 7 : lecture), puis un second octet
transfère la donnée. CS doit rester actif entre les deux octets.

| Index | Fonction |
| --- | --- |
| 0 | Amplificateur audio, mute, rétroéclairages bas/haut, clignotement LED et extinction ; masque `0x7f` |
| 1 | Batterie bonne/basse sur bit 0, lecture seule |
| 2 | Amplificateur microphone sur bit 0 |
| 3 | Gain microphone sur bits 0..1 |
| 4..127 | Miroirs de 0..3, conformément au modèle DS originale |

Les écritures deviennent visibles à la fin de l'octet SPI, avant même de
relâcher CS. Désactiver SPI pendant un octet l'abandonne sans modifier les
registres ni produire d'IRQ. Les octets déjà terminés restent appliqués.
Changer de périphérique oublie l'index sélectionné. Les quatre débits, busy,
les accès combinés contrôle/données et IF.23 suivent le bus SPI commun.

`Machine::power_management()` et `Memory::power_management()` exposent l'état
en lecture seule, dont les deux rétroéclairages. `set_battery_low(bool)` fournit
l'entrée batterie de l'hôte, bonne par défaut, conservée lors des resets et
rechargements. Les valeurs initiales `control=0x0d`, microphone désactivé et
gain 1 sont une convention du démarrage direct ; elles ne prétendent pas
reproduire les valeurs analogiques au branchement de l'alimentation.

Le bit 6 du registre 0 éteint la machine au terme de son octet. `powered_off()`
permet à l'hôte de l'observer : CPU, DMA, timers et balayage LCD cessent d'avancer.
Un grand budget `run_cycles()` s'arrête à cette frontière ; le compteur de
l'ordonnanceur indique le temps réellement exécuté. Les IRQ et boutons ne
remettent pas sous tension. Un chargement de cartouche réussi réinitialise
l'alimentation et les registres ; un chargement refusé conserve l'état éteint.
`Memory::clear_ram()` réinitialise aussi Powerman. Charger seulement une image
firmware ne rétablit pas l'alimentation. Les sauvegardes restent accessibles
par les API de snapshot et d'acquittement après extinction.

Les images RGB555 représentent toujours la sortie des moteurs vidéo : Powerman
ne change ni les pixels ni POWCNT1. L'hôte doit appliquer les rétroéclairages
et l'état éteint à leur présentation. Le clignotement LED et les commandes
d'amplificateurs sont mémorisés ; leur restitution dépend des futurs adaptateurs
et de l'audio. Les variantes Lite/DSi, les effets analogiques du rétroéclairage,
la décharge réelle de batterie et la corruption d'une flash lors d'une coupure
électrique ne sont pas simulés. Sleep et POWCNT2 restent à implémenter.

`nds_powerman` vérifie les 128 indices, les masques, les quatre débits, les IRQ,
la désélection, les écritures annulées, les copies en cours de transfert et les
resets. Un programme ARM7 synthétique écrit puis lit le contrôleur, publie sa
batterie en RAM et éteint les deux CPU. Les tests contrôlent aussi la frontière
d'arrêt sur une grande avance, l'absence de réveil par IRQ, l'export d'une FRAM
après extinction et le redémarrage. Aucune ROM commerciale n'est utilisée.

## Firmware SPI, réglages et étalonnage tactile

`Firmware`, `FirmwareSettings` et `TouchCalibration` sont déclarés dans
`<ravenemu/nds/firmware.hpp>`. `Machine::load_firmware(bytes)` ou la même méthode
sur `Memory` reçoit une image DS de **256 ou 512 Kio**, copiée dans le cœur.
L'hôte fournit les octets : aucune image Nintendo ni image de remplacement
n'est incluse. DSi 128 Kio/4 Kio n'est pas pris en charge. La validation porte
sur la taille et les réglages, pas sur l'authenticité du code de démarrage.

Le périphérique SPI 1 accepte les commandes suivantes. Adresses sur trois
octets, poids fort en premier ; les lectures bouclent à la capacité physique.

| Commande | Comportement |
| --- | --- |
| `03` READ | Adresse 24 bits puis flux continu |
| `0b` FAST | Même lecture, avec un octet muet après l'adresse |
| `9f` RDID | Modèle M45PE20 `20 40 12` pour 256 Kio, M25PE40 `20 80 13` pour 512 Kio |
| `05` RDSR | Statut répété : WEL bit 1, WIP bit 0 pendant une opération flash |
| `06` WREN / `04` WRDI | Active/désactive WEL à la remontée de CS |
| `02` PP | Programme 1..256 octets d'une page : bits de 1 vers 0 seulement |
| `0a` PW | Écrit 1..256 octets d'une page : bits vers 0 ou 1, voisins conservés |
| `db` PE | Efface une page de 256 octets à `0xff` |
| `d8` SE | Efface un secteur de 64 Kio à `0xff` |
| `b9` DP / `ab` RDP | Sommeil/réveil à la remontée de CS ; les commandes ordinaires sont ignorées pendant le sommeil |

Ce choix d'identifiant modélise un composant documenté selon la capacité ; il
ne prétend pas retrouver le fabricant à partir du dump. Les octets sans réponse
définie donnent `0xff` dans ce modèle ; l'open-bus n'est pas mesuré sur console.
Les mêmes busy, horloges, CS et IRQ IF.23 que le tactile s'appliquent. Chaque
transfert prépare seulement une petite session, sans recopier l'image flash.
Une session n'est appliquée qu'à la fin du transfert ; annulation et copie
de machine conservent des curseurs indépendants.

PP/PW/PE/SE exigent WREN. Sans WEL, la commande est ignorée. Le transfert de
commande/adresse/données ne modifie pas encore la flash : une commande complète
est lancée à la remontée de CS. WIP est alors actif, WEL reste actif jusqu'à la
fin et les octets deviennent visibles atomiquement au terme du délai. Pendant
WIP, seule RDSR est acceptée ; WREN, sommeil et lectures sont ignorés. Finir le
cycle flash ne produit aucune IRQ supplémentaire : IF.23 concerne les bytes SPI.

Les durées déterministes utilisent les valeurs typiques GBATEK, arrondies au
cycle commun supérieur : PP 1,2 ms (40 217 cycles), PW 11 ms (368 654), PE 10 ms
(335 140), SE 1 s (33 513 982). Les avances groupées et fractionnées ne débitent
pas le temps du dernier byte SPI sur une opération qui n'a pas encore commencé.
Les tolérances selon le composant et les délais analogiques de mise sous tension,
sommeil et réveil restent à implémenter.

PE/SE alignent l'adresse sur la page/le secteur ; les adresses d'écriture sont
miroitées à la capacité du composant. Le modèle fixe `/W` bas : les 256 premières
pages, soit 64 Kio, sont protégées, même à travers un miroir. Une commande sur
cette zone ne modifie ni les octets ni WIP ; WEL est conservé dans ce modèle,
ce détail n'étant pas validé sur console. Aucun forçage de la broche n'est exposé.
Les flux PP/PW doivent tenir dans la page à partir de l'adresse initiale. Le
retour en début de page pour les flux trop longs reste à valider ; le byte
excédentaire est diagnostiqué avant toute mutation de ce transfert. L'hôte peut
réinitialiser le bus pour abandonner le paquet déjà accumulé. Un effacement
exige exactement trois octets d'adresse ; les paquets incomplets sont ignorés.

Un strobe firmware sans image ou une commande inconnue hors sommeil produit
un diagnostic, sans modifier les registres ni les octets de l'image.

L'offset des réglages vient du demi-mot à `0x20`, multiplié par huit, et non
d'une position de fin d'image imposée. Les deux blocs de `0x100` octets doivent
tenir dans l'image après l'en-tête. Chaque candidat exige la version 5, un
compteur 0..127 et un CRC16 sur les `0x70` premiers octets (polynôme réfléchi
`0xa001`, graine `0xffff`). Avec deux copies valides, la seconde est retenue
si son compteur suit immédiatement le premier modulo 128 ; sinon la première.
Avec une seule copie valide, celle-ci est utilisée. Aucune copie valide ne
produit aucun réglage de remplacement. `firmware().settings()` est alors vide,
mais les octets restent accessibles par SPI. La copie RAM de boot n'inclut
ni compteur ni CRC, conformément au format de la console. Après une modification
effective de la flash, le cache des réglages et de l'étalonnage est recalculé.
Une nouvelle copie valide peut ainsi devenir courante ; un CRC invalidé fait
revenir à l'autre copie. Cette mise à jour ne réécrit pas la RAM du jeu en cours.

Un étalonnage exige deux points ADC 12 bits distincts par axe et deux points
écran distincts et valides. Un étalonnage dégénéré n'empêche pas de lire le
reste des réglages. L'API `set_input` conserve toujours ses ADC bruts ; l'API
`Machine::set_touch_position(x,y,pressed=true)` convertit des **pixels d'origine
(0,0)** dans les limites 256×192 à partir de l'étalonnage sélectionné. Elle
applique l'inverse de la formule GBATEK, arrondit à l'ADC le plus proche et
sature l'extrapolation à 0..4095. Elle conserve boutons, capot, DEBUG et Z1/Z2.
Un contact sans étalonnage ou un pixel hors écran est refusé sans mutation.
`pressed=false` relâche le stylet sans exiger d'étalonnage ni changer les ADC.

```cpp
machine.load_firmware(firmware_bytes);
machine.load_cartridge(cartridge);
machine.set_touch_position(128, 96);       // centre de l'écran inférieur
machine.run_frame();
machine.set_touch_position(0, 0, false);  // relâchement ; position ignorée
```

Charger une image valide réinitialise le bus SPI, ses sessions et le TSC, en
conservant les entrées physiques et les IRQ déjà acquises. Un chargement refusé
conserve aussi le transfert en attente. La RAM et les autres périphériques ne
changent pas : charger le firmware **avant la cartouche** permet d'obtenir la
copie RAM correspondante au prochain démarrage direct. `clear_ram()` et le
rechargement de cartouche conservent l'image et son état de sauvegarde,
réinitialisent sommeil/WEL et les entrées physiques. Un paquet SPI non terminé
est abandonné ; une opération flash déjà lancée reste autonome et termine avec
les cycles suivants, même après reset du bus ou changement de périphérique.
Désactiver SPI au milieu d'un byte abandonne le paquet entier sans programmer
son préfixe. Remplacer explicitement l'image abandonne aussi l'ancienne opération
interne ; un remplacement refusé conserve celle-ci. `clear_ram()` seul ne prépare
pas les réglages RAM. Le boot firmware et son menu de recalibrage restent à développer.

`Machine` et `Memory` exposent `firmware_dirty()`, `snapshot_firmware()` et
`acknowledge_firmware_saved(generation)`. Le snapshot possède ses octets, au format
brut de l'image chargée (256/512 Kio), et un jeton de génération. Aucun trailer
n'est ajouté. L'hôte écrit ce fichier firmware séparément des sauvegardes de jeux,
puis acquitte exactement ce jeton **après** une écriture réussie. Un acquittement
ancien ne nettoie pas une modification plus récente ni une image remplacée.
Lire un snapshot ne le marque pas sauvegardé. Un cycle terminé sans changement
d'octets ne salit pas l'image. Copie et reset conservent indépendamment les générations.

Un snapshot sans image ou pendant WIP est refusé : l'hôte doit laisser finir
l'opération avant de sauvegarder/fermer. Les bytes d'un paquet encore sous CS
ne sont pas des écritures validées et ne figurent pas dans le snapshot. Le cœur
n'écrit aucun fichier lui-même ; le rechargement utilise `load_firmware(snapshot.data)`.
Les adaptateurs NDS SDK/Libretro et leur persistance automatique restent à développer.

```cpp
if (machine.firmware_dirty() && !machine.memory().firmware().busy()) {
    auto saved = machine.snapshot_firmware();
    // L'hôte écrit saved.data dans son fichier firmware et vérifie le succès.
    // Seulement après cette réussite :
    machine.acknowledge_firmware_saved(saved.generation);
}
```

Cette implémentation C++ indépendante suit
[GBATEK, Firmware Serial Flash](https://mgba-emu.github.io/gbatek/ds.html#dsfirmwareserialflashmemory),
[Header](https://mgba-emu.github.io/gbatek/ds.html#dsfirmwareheader) et
[User Settings](https://mgba-emu.github.io/gbatek/ds.html#dsfirmwareusersettings).
La graine CRC a aussi été vérifiée dans la bibliothèque pour console
[Calico](https://github.com/devkitPro/calico/blob/master/source/nds/arm7/env.c),
sans importer son code. Les tests n'utilisent que des images synthétiques.

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
`step()`/`run_steps()` ou des créneaux de `run_cycles()`. L'ordre commun est
déterministe, mais aucune latence matérielle de synchronisation n'est modélisée. Copier une machine copie les files, erreurs et
registres IPC ; un chargement réussi les réinitialise, un échec les préserve.
Référence : [GBATEK, DS Inter Process Communication](https://mgba-emu.github.io/gbatek/ds.html#dsinterprocesscommunicationipc).

## BIOS et diagnostics

`Memory::load_bios(processor, bytes)` accepte exactement 16 Kio sur ARM7
(`0x00000000`) ou 4 Kio sur ARM9 (`0xffff0000`). Ces fenêtres sont en lecture seule.
Sans image, lire un vecteur échoue explicitement. Les tests fournissent de petits
gestionnaires synthétiques ; aucun BIOS Nintendo ni firmware n'est inclus.
Il n'y a pas de remplacement haut niveau des services SWI, de démarrage firmware
ni de protection de lecture BIOS selon PC. Les fautes MPU utilisent les exceptions
Data/Prefetch Abort ; les accès bus non implémentés restent des diagnostics.

Une instruction non implémentée ou une erreur de lecture/écriture lève
`CpuExecutionError` avec processeur, adresse, mode et opcode (absent si la
lecture de l'instruction a échoué). Le CPU reste sur l'instruction fautive,
ses registres et son compteur sont préservés. Les écritures multiples vérifient
toutes les destinations du bus avant de modifier le premier mot ; une erreur de
chargement multiple ne valide aucun des registres chargés. Cette atomicité est
un contrat transactionnel de ce modèle, distinct des effets partiels possibles
lors des abandons matériels. Les fautes de protection MPU entrent toutefois dans
les vecteurs d'abandon décrits plus haut. Les instructions déjà terminées
d'un `run_steps` restent appliquées. Le chargement d'une nouvelle cartouche
réinitialise les deux CPU. Aucun CPU ne conserve de référence persistante au
bus : copier une machine produit une mémoire et des CPU indépendants.

`nds_gamecard` vérifie RAW et B7/B8, commandes et octets, tailles, débits/gaps,
DRQ/busy, inspection, IRQ, alignements, mirroring et dumps réduits. Il couvre
les droits EXMEM et les refus atomiques, slots vides, copies, reset et remplacement
de média. Les deux CPU sont testés avec DMA, compteurs, répétition, erreurs sans
consommation, armement tardif et TCM. Quatre programmes synthétiques ARM9/ARM7
lisent leurs ressources par CPU ou DMA ; ARM9 en tire la couleur de l'écran.

`nds_firmware_write` vérifie WREN/WEL/WIP, PP/PW, délais, PE/SE, alignements,
zone protégée et miroirs, paquets incomplets/trop longs, annulations, grandes
avances et fractionnements, copies et reset pendant WIP. Il contrôle l'export
possédé, les générations/acquittements et le remplacement d'image, la sélection
des réglages après écriture et la restauration de l'étalonnage après rechargement.
Un programme ARM7 fait WREN, programme la flash, attend RDSR puis relit le byte et
le publie à ARM9. Les fixtures synthétiques CRC sont partagées par les tests.

`nds_firmware` vérifie taille/propriété des images, offsets 256/512 Kio, versions,
CRC indépendants, sélection et wrap des compteurs, étalonnages valides/dégénérés,
pixels, arrondis, saturation et pression conservée. Il contrôle READ/FAST, adresse
24 bits, bouclage, JEDEC, WEL, sommeil/réveil, IRQ, refus sans mutation, reset et
copies en attente. Deux programmes ARM9/ARM7 lisent les réglages en RAM, un ADC
de référence par SPI, puis la mesure issue des pixels hôte sous l'ordonnanceur.
Les chargements refusés/réussis et la priorité des exécutables sont vérifiés.

`nds_spi` vérifie les débits et busy, les voies d'octets/demi-mots/mots,
CS, ADC X/Y/Z1/Z2 en 12/8 bits, référence, relâchement, commandes en pipeline
et non alignées, alimentation/PENIRQ, échantillonnage et copies en attente.
Il contrôle les erreurs sans mutation, annulations, reset, grandes avances et
IF.23 jusque dans le gestionnaire ARM7. Un exécutable ARM7 configure le bus,
attend busy, assemble les octets ADC et publie X en RAM partagée sous
l'ordonnanceur ; un rechargement vérifie ensuite le contact relâché.

`nds_input` vérifie les douze boutons, voies d'octets/mots, contrôles par CPU,
fronts OR/AND, écritures pendant maintien, acquittements, masque de source capot,
WFI et gestionnaires IRQ, TCM/MPU, copies et chargements. Un programme ARM9 lit A
et choisit la couleur du backdrop ; un programme ARM7 Thumb publie simultanément
KEYINPUT en RAM. Les tests appuient puis relâchent A et vérifient la couleur
rendue et l'état publié, sans modifier directement l'image.

`nds_scheduler` vérifie le rapport des créneaux, budgets nuls/débordants et
fractionnés, les horloges partagées, les échanges RAM/IPC entre deux programmes,
le réveil timer et les gestionnaires IRQ/MPU, les DMA des deux CPU et HBlank,
la FIFO vidéo avec capture, les quatre phases d'erreur et leur reprise, les
copies et chargements en attente, ainsi qu'une image complète et les API manuelles.
`nds_wait` vérifie les deux WFI, IME/IE/IF, les masques CPSR, IRQ/FIQ, conditions
ARM, privilèges, DMA pendant l'attente, MPU après réveil et reset CPU.

`nds_mpu` couvre les huit régions et leurs priorités, alias et masques CP15,
les six AP par mode et par type d'accès, les frontières jusqu'à 4 Gio, les AP
compactes/étendues, l'initialisation par ROM synthétique, la copie et le reset.
Les tests exécutent des fautes et reprises ARM/Thumb via les vecteurs bas/hauts,
un gestionnaire MCR réparant les AP, les transferts multiples transactionnels,
les refus MMIO sans effet FIFO, les TCM, DMA et ARM7, et les commandes de cache.

`nds_cp15` vérifie les MRC/MCR et leurs privilèges, identifiants, masques,
NZCV/Q, vecteurs et interworking, fenêtres/miroirs/priorités TCM, chargement,
exécution ARM/Thumb depuis ITCM, transferts CPU et DMA séparés, atomicité des
échecs, copies et rechargements d'une ROM synthétique configurant CP15.

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

`nds_mosaic` vérifie les tailles 1..16, les champs A/B, écritures seules,
protection d'alimentation et DMA, défilement, flips, références affines négatives,
wrap et bitmaps. Il contrôle les blocs partiels OBJ, matrices, palettes standard
et étendues, transparence, chevauchements et changements de priorité, alpha
bitmap, fenêtres et luminosité maître. Un programme ARM écrit les registres
et construit deux images complètes avec des grilles différentes ; permutation,
copie, reset et chargements réussis/refusés sont aussi vérifiés.

`nds_capture` couvre le registre, les DMA, tailles et banques, offsets, wrap,
stride source/destination, validité des pixels, coefficients, saturation et
rétroaction VRAM. Il vérifie l'attente de ligne zéro, busy jusqu'à VBlank,
réaffectations, annulation, ordre capture/DMA et séparation de MASTER_BRIGHT.
Une image composée avec mosaïque, fenêtre et OBJ affine wrappé est capturée
ligne par ligne, comparée à l'instantané, puis réaffichée sur B. Un programme
ARM configure la capture, attend busy et publie sa fin ; les grandes avances,
copies, erreurs et chargements sont aussi contrôlés.

`nds_fifo` couvre le port CPU 32 bits, lectures sans retrait, bouclage et
sous-alimentation, reset des pointeurs à VBlank, copies et activations différées.
Il contrôle les quotas DMA, longueurs continues/répétées, IRQ, erreurs et
suspension CPU. Une image entière est affichée et capturée simultanément, sans
double consommation ; mélange, validité, rollback et très grandes avances sont
vérifiés. Une ROM ARM9 configure son DMA, attend l'IRQ et produit les 256×192
pixels, avec copies et chargements refusés/réussis.

Prochains jalons : compléter les instructions et durées CPU/bus, le démarrage,
les autres mémoires de sauvegarde cartouche et leur sélection automatique,
les variantes du protocole ROM, Sleep/POWCNT2 et le démarrage firmware,
puis 3D et audio.
L'ordonnanceur actuel reste un modèle fonctionnel à coût uniforme.

Aucun jeu commercial n'est validé à ce stade. Les programmes synthétiques
peuvent déjà lire leurs ressources cartouche par CPU/DMA, écrire puis relire une
EEPROM, une flash ou une FRAM, réagir aux boutons et modifier le rendu, lire le firmware fourni
et mesurer le tactile à partir de pixels étalonnés. La bibliothèque native
NDS est installée avec le paquet C++, mais les adaptateurs `Core`, `sdk::System`
et Libretro NDS restent à développer.
