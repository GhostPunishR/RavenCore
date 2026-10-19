# RTC GBA : détection des pilotes Pokémon et validation

La correction est une implémentation C++ propre à RavenCore. Aucun code de mGBA,
VBA ou CFRU n'est incorporé au cœur ou aux tests ; ces projets servent à observer
les comportements et à identifier les données. Aucune ROM commerciale n'est
incluse. Les tests fabriquent leurs en-têtes, littéraux et instructions ARM.

## Cause et indices vérifiés

`RomTraits::detect()` ne recherchait que `SIIRTC_V`. Le pilote public de Skeli
[CFRU, siirtc.c](https://github.com/Skeli789/Complete-Fire-Red-Upgrade/blob/b637a27898b14e25dd24d0f69a3e302f0069deb8/src/siirtc.c)
mentionne `SIIRTC_V001` dans un commentaire, sans objet chaîne correspondant.
Ses fonctions utilisent les trois ports `080000C4`, `080000C6`, `080000C8`.
La chaîne de commentaire n'est donc pas un identifiant binaire du pilote.

L'identifiant interne `pokemon red version\0`, en `0x108`, est documenté par
[le header Game Freak de Rouge Feu](https://github.com/pret/pokefirered/blob/037335f4c725d7c9aecdac87066f2002b4bd7e14/src/rom_header_gf.c).
Il est distinct du code cartouche `BPRE` en `0xAC`. Les sources de Rouge Feu
n'embarquent pas le pilote RTC CFRU.

Un [patch public de traduction fondé sur Unbound 1.0.1](https://github.com/pokemon-unbound-de/pokemon-unbound-de/releases/tag/v0.1.4)
a été inspecté, sans reconstruire une ROM commerciale. Fichier
`pokemon_unbound_de_v0.1.4_von_firered_usa.bps`, SHA-256 :

```
dadf38512ea9ba220a882a5816519f3e1f686893bba2f5be7158cf8d8ed2338e
```

Le CRC du patch BPS est valide, sa source fait 16 Mio (CRC `DD88761C`) et sa
cible 32 Mio (CRC `90897064`). L'analyse des commandes BPS distingue les octets
ajoutés connus de ceux hérités de la source indisponible. Elle établit que :

- l'identifiant Game Freak à `0x108` est conservé depuis Rouge Feu ;
- parmi les 18 476 883 octets connus ajoutés, aucun `SIIRTC_V` n'est présent ;
- les littéraux alignés `080000C4`, `080000C6`, `080000C8` sont notamment présents
  aux offsets cible `A54CDC`, `A54D74`, `A54CF0`, donc dans un groupe de 152 octets.

Ces observations corroborent le pilote sans marqueur décrit par CFRU. Elles ne
constituent ni une recherche exhaustive dans une ROM complète, ni un essai de
Pokémon Unbound 2.x. Aucun identifiant spécifique « UNBOUND » n'est inventé.

## Stratégie RavenCore

La signature Seiko historique garde sa priorité. À défaut, le détecteur exige
**ensemble** l'identité interne exacte de Rouge Feu et les trois références GPIO
alignées, regroupées sur au plus 1 Kio. Le code `BPRE`, la taille de la ROM ou
une modification du checksum ne suffisent pas. Un en-tête seul, un groupe
incomplet, des références dispersées ou un jeu non Pokémon sont refusés.
L'original sans pilote reste sans RTC. Le choix forcé on/off continue de primer.

C'est une détection structurelle de la famille de pilotes observée, pas une
preuve d'exécution du code. Un hack supprimant l'identifiant interne, calculant
les ports sans ces littéraux ou plaçant leurs références plus loin peut échapper
à ce critère. Un hack intégrant un pilote inutilisé pourrait être reconnu.
L'examen n'a lieu qu'au chargement ; les traits sont réutilisés au reset.

## Comparaison avec mGBA actuel

Version examinée et compilée :
[`fd9dc5c2cbf234365068855851107e762fc6211b`](https://github.com/mgba-emu/mgba/tree/fd9dc5c2cbf234365068855851107e762fc6211b).

Dans [overrides.c](https://github.com/mgba-emu/mgba/blob/fd9dc5c2cbf234365068855851107e762fc6211b/src/gba/overrides.c#L381),
mGBA identifie les hacks par les noms internes Rouge Feu/Émeraude, ou `AXVE`,
puis exclut une liste de CRC de ROM originales. Il force alors Flash 1 Mbit,
`HW_RTC` et `vbaBugCompat`, avec priorité aux réglages explicites. Son entrée
standard pour `BPRE` utilise `HW_NONE`. La détection RavenCore est plus ciblée :
elle exige les indices du pilote, ne reprend pas cette table de CRC et ne change
pas la détection des sauvegardes.

Dans [gpio.c](https://github.com/mgba-emu/mgba/blob/fd9dc5c2cbf234365068855851107e762fc6211b/src/gba/cart/gpio.c),
`vbaBugCompat` permet au latch DATA de modifier aussi des broches configurées en
entrée et supprime la propagation immédiate des changements de DIR. Ce n'est
pas le comportement matériel normal. Un observateur externe, compilé contre
mGBA, a rejoué par son bus la séquence du pilote CFRU : DATA=1, DATA=5, DIR=7,
commande MSB avec trois écritures basses par bit, DIR=5, cinq écritures basses
par bit lu, puis DATA=1 deux fois. À l'époque Unix `1767225600`, fuseau `UTC0` :

| Mode mGBA | État | Date complète | Heure seule |
| --- | --- | --- | --- |
| `vbaBugCompat=false` | `40` | `26 01 01 04 00 00 00` | `00 00 00` |
| `vbaBugCompat=true` | `40` | `26 01 01 04 00 00 00` | `00 00 00` |

RavenCore rend les mêmes valeurs dans ses tests. Aucun besoin de cette exception
n'est donc démontré pour le pilote vérifié ; aucun mode VBA n'a été ajouté.
Cela ne prouve pas qu'aucune ancienne variante d'Unbound n'utilise un autre pilote.

## Protocole, heure et sauvegardes

Les changements respectent les directions GPIO, traitent la libération de CS
comme une annulation et rendent SIO haut au repos. STRB vers le bus ROM/GPIO est
ignoré ; les écritures 16/32 bits restent disponibles. Les données entrent au
front montant de SCK et sortent au front descendant. Les écritures répétées au
même niveau ne produisent pas de fronts supplémentaires. Le décodage historique
des commandes inversées reste accepté ; un code fixe invalide est refusé.

L'état démarre à `40` (24 h, pile valide). Les bits écrits sont masqués par `6A` :
IRQ fréquence/minute/alarme et choix 12/24 h. Le mode 12 h et le drapeau PM
bit 7 sont maintenant appliqués à la lecture et à l'écriture ; midi se lit
`80` en 12 h et `92` en 24 h. Le PM est aussi positionné en 24 h, conformément
à GBATEK ; le pilote CFRU le masque lorsqu'il lit l'heure. mGBA actuel ne rend
pas ce PM et n'applique pas les écritures de calendrier : ce ne sont pas des
comportements repris par RavenCore.

`GbaCore::current_epoch()` utilisait déjà `system_clock`, puis `Rtc` convertissait
ces secondes par `localtime_r`/`localtime_s`. Ce chemin est conservé et testé sans
override. Chaque lecture verrouille la date complète au début de la commande.
L'offset reste exprimé en secondes par rapport à l'hôte, et ne dépend pas du
nombre de trames, de la pause ou de l'accélération de l'émulation.

Une écriture du jeu capture une seule fois l'heure hôte. Les champs BCD et la
date réelle sont vérifiés, puis les candidats heure d'été/hiver sont comparés
à l'instant courant. Cela gère les heures répétées, y compris les transitions
de trente minutes, sans normaliser silencieusement un 30 février.

Le reset de l'émulateur conserve les réglages et l'offset par `keep_battery_of`.
La commande RTC force-reset `60` garde la politique existante de RavenCore de
resynchronisation avec l'hôte, mais remet désormais le contrôle à zéro (12 h).
Elle est distincte du reset de l'émulateur. Le jour de semaine reste dérivé de
la date locale (dimanche=0), comme auparavant : le compteur hebdomadaire
indépendant du composant, ses IRQ et ses courses de setup/hold ne sont pas simulés.

Le bloc batterie garde exactement **16 octets** : date/heure, contrôle, latch
Unix 64 bits little-endian. L'heure du bloc reste en 24 h sans drapeau PM,
comme dans les anciens fichiers RavenCore, même si le protocole série est en
12 h. Les données Flash/SRAM précédentes ne sont ni déplacées ni réécrites.
Un fichier sans bloc continue d'utiliser l'hôte ; les blocs incohérents sont
ignorés et le remplissage FlashGBX `01` du contrôle reste pris en charge.
Le bloc historique ne mémorise pas le nom du fuseau : après un changement
complet de fuseau entre deux sessions, il ne permet pas de distinguer un
décalage réglé par le jeu d’un changement de zone de l’appareil. Aucun nouveau
format incompatible n’est introduit pour masquer cette ambiguïté.
Le format d'état instantané n'est pas agrandi. Un ancien état d'un hack créé
sans RTC n'a pas de bloc GPIO et doit être remplacé par un reset et une `.sav`.

## Tests et limites de validation

`gba_raven_rtc` vérifie détection positive/négative, forçages, séquence CFRU,
GPIO et transitions de direction, BCD, modes 12/24 h, PM, calendrier bissextile,
jour de semaine, invalides, temps vivant/verrouillé, bloc de 16 octets et DST.
Les fuseaux POSIX de test sont UTC, EST/EDT et Lord Howe (décalage de 30 minutes),
sans dépendre de la présence d'une base tzdata particulière. Les tests DST
explicites sont exécutés sur POSIX ; les chemins Windows restent compilables.

`gba_raven_core` exécute un programme ARM synthétique à travers l'interface
publique, sans forçage, et vérifie état/date/heure, progression, reset, rechargement,
offset, deux banques Flash et accès à l'horloge système réelle.
`libretro_smoke_gba` vérifie le mode auto et la taille stable de SAVE_RAM dès le
chargement, l'import du frontend, le reset, le forçage off et le cas sans pilote.

Commandes depuis la racine, avec CMake dans PATH :

```sh
cmake -S . -B build
cmake --build build --parallel 4
ctest --test-dir build -R '^(gba_raven_rtc|gba_raven_core|libretro_smoke_gba)$' --output-on-failure
ctest --test-dir build --output-on-failure --parallel 4
```

Vérification ASan/UBSan reproductible, dans un répertoire séparé :

```sh
cmake -S . -B build-rtc-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS'
cmake --build build-rtc-sanitize --parallel 2
ctest --test-dir build-rtc-sanitize \
  -R '^(gba_raven_.*|gba_save_state|libretro_smoke_gba|ravenemu_c_api)$' --output-on-failure
```

Résultats obtenus sur la branche de correction issue de `main` : **245/245 tests**
au total, **12/12 suites** GBA/API/Libretro sous ASan/UBSan, consommateur du paquet
CMake fraîchement installé réussi. La même correction avait aussi passé les
260 tests du contexte de développement contenant les jalons NDS précédents ;
ces jalons ne font pas partie de cette branche de correction.
Les sources GB/GBC, NDS, `GbaCore::current_epoch` et le frontend Libretro de
production ne nécessitent aucune modification pour cette correction.

**Validation réelle restante :** charger la ROM Unbound exacte qui pose problème,
vérifier ses indices binaires, constater l'absence de l'avertissement RTC,
contrôler la date et l'heure en jeu, sauvegarder, fermer, rouvrir et réinitialiser.
Aucune exécution réelle de Pokémon Unbound ou de Rouge Feu commercial n'a été
réalisée ici. La variante 2.x n'a pas été validée par le patch 1.0.1 inspecté.

Références matérielles : [GBATEK GPIO](https://mgba-emu.github.io/gbatek/gba.html#gbacartioportgpio),
[RTC GBA](https://mgba-emu.github.io/gbatek/gba.html#gbacartrealtimeclockrtc),
[transfert et calendrier Seiko](https://mgba-emu.github.io/gbatek/ds.html#dsrealtimeclockrtc).
