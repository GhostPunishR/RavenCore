# Tests GBA

Les tests synthétiques (`gba_*_test.cpp`) n'ont besoin d'aucune ROM :

- `gba_cpu_test.cpp` vérifie les comportements de l'ARM7TDMI et du bus : de petits programmes ARM et Thumb sont placés en IWRAM, puis exécutés instruction par instruction  ; il éprouve aussi le multiplieur de Booth qui fixe le drapeau C des multiplications : ses rangées doivent sommer au produit exact ;
- `gba_hardware_test.cpp` vérifie le matériel décrit par GBATEK : appels du BIOS (dont ArcTan, ArcTan2 et Div, valeurs de la console comprises), HALTCNT, port série sans câble, capture vidéo par DMA3, temps de rendu des sprites, bus ouvert, durée des multiplications et reprise exacte d'un état instantané ;
- `gba_cycle_test.cpp` mesure les cycles CPU d'instructions isolées (ALU, multiplications, accès mémoire, sauts, en ARM et en Thumb, en IWRAM et en ROM) et les compare aux sommes tirées des tables de GBATEK : chaque cas indique sa formule ;
- `gba_timing_test.cpp` vérifie la cadence : coûts du tampon de préchargement de la cartouche, bug du préchargement désactivé, démarrage des timers, registres d'E/S non répétés, DMA et frontière de trame ;
- `gba_rtc_test.cpp` dialogue avec l'horloge de la cartouche par le GPIO, comme la bibliothèque Seiko des jeux, et vérifie que ses réglages suivent la sauvegarde (bloc de 16 octets de mGBA) ;
- `gba_core_test.cpp` passe par l'interface publique `ravenemu::Core` : détection des sauvegardes, vibration du GPIO, compatibilité des anciens états, reprise d'un état en plein préchargement, sauvegarde et horloge gardées par le reset, et câble link : deux consoles reliées s'échangent leurs données en multijoueur et en mode normal 32 bits (programmes ARM assemblés avec llvm-mc).

## ROMs de test publiques

`gba_conformance_runner` exécute une ROM de test fournie séparément et rend un verdict. Aucune ROM n'est stockée dans le dépôt.

[`external_test_roms.txt`](external_test_roms.txt) liste les suites utilisées. Chacune est figée sur un commit et accompagnée de son SHA-256 :

| Suite | Licence | Contenu |
| --- | --- | --- |
| [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests) | MIT | ARM, Thumb, mémoire, BIOS, sauvegardes, code en VRAM, lectures hors ROM |
| [DenSinH/FuzzARM](https://github.com/DenSinH/FuzzARM) | GPL-3.0 | 5 × 10 000 tests ARM et Thumb aléatoires |

Pour les exécuter, comme le fait la CI :

```sh
cores/gba/tests/fetch_test_roms.sh build/gba-test-roms
cmake -S . -B build -DBUILD_TESTING=ON -DRAVENEMU_GBA_TEST_ROMS=build/gba-test-roms
cmake --build build --parallel
ctest --test-dir build -R gba_rom_ --output-on-failure
```

Le script vérifie le SHA-256 de chaque ROM, et le runner le revérifie (`--rom-sha256`). Une autre version d'une ROM ne peut donc pas fausser le verdict en silence.

## Protocole du runner

Une ROM est réussie quand trois conditions sont réunies :

1. Le CPU reste sur une boucle `b .` pendant plusieurs images.
2. Le registre de résultat vaut 0. C'est `r12` par défaut ; `--result-register 7` pour la ROM Thumb, `none` pour ignorer le registre.
3. Chaque attente `--expect-word ADRESSE=VALEUR` est vérifiée.

Pour FuzzARM, l'attente est `0x02000000=0` : en cas d'échec, la ROM y écrit `AAAA` ou `TTTT`, puis attend une touche.

Le verdict indique aussi les compteurs de diagnostics du cœur : SWI non pris en charge, instructions indéfinies et accès mémoire inconnus.

```sh
build/cores/gba/gba_conformance_runner chemin/test.gba --result-register 12 --timeout-frames 600
```

`--self-test` éprouve le runner sur des ROMs synthétiques. CTest l'exécute même sans ROM externe.
