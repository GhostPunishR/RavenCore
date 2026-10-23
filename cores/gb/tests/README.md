# Harness de conformité GB/GBC

`gb_conformance_runner` exécute une ROM de test externe sans l'ajouter au
dépôt. `conformance_manifest.py` orchestre une suite entière autour de ce
runner. Aucun des deux ne télécharge ou ne copie une ROM : le développeur ou la
CI fournit séparément des artefacts qu'il est légalement autorisé à utiliser.

Après le build natif, exemple :

```bash
build/native-host/gb/gb_conformance_runner chemin/test.gb \
  --hardware dmg --category timer --timeout-frames 600
```

Le runner détecte :

- les protocoles série contenant `Passed` ou `Failed` (textes configurables) ;
- la signature de registres `B,C,D,E,H,L = 3,5,8,13,21,34` utilisée par des
  suites matérielles publiques ;
- une ou plusieurs attentes mémoire stables, par exemple
  `--expect-memory 0xC000=0x42` ;
- un framebuffer ARGB8888 exact de `160 × 144 × 4` octets, sérialisé en
  pixels 32 bits big-endian, via `--expect-frame-argb8888`, ou son hash via
  `--expect-frame-sha256` ;
- une capture RGB888 brute de `160 × 144 × 3` octets via
  `--expect-frame-rgb888`. L'image du cœur est d'abord convertie dans la
  palette commune des captures de game-boy-test-roms : les gris DMG `FF`,
  `AA`, `55`, `00` et, en CGB, chaque composante 5 bits étendue en
  `(x << 3) | (x >> 2)`. En DMG, chaque gris attendu est ramené au plus
  proche de ces quatre niveaux. `--compatibility-palette boot-default` charge,
  en compatibilité CGB sans boot ROM, les palettes que la boot ROM CGB donne à
  un jeu inconnu, celles des captures de référence ;
- l'arrêt inattendu du CPU, les exceptions et le timeout.

`--expect-memory-before ADRESSE=VALEUR` (répétable, avec `--expect-memory`)
n'autorise la réussite mémoire qu'après avoir observé cet état intermédiaire.
Les ROMs de Blargg écrivent `$80` en `$A000` pendant le test : sans cette
garde, le `$00` d'une RAM tout juste effacée, déjà suivi de la signature,
passait pour une réussite.

`--minimum-pass-frames` retarde uniquement les voies de réussite ; les erreurs
série restent immédiates. Pour une réussite mémoire, l'orchestrateur impose en
plus `--memory-require-mismatch` : l'ensemble des attentes doit d'abord avoir
été observé dans un état différent, puis rester conforme pendant le nombre de
points d'observation déclaré par `memory_stability_samples` (au moins deux).
Un point est pris après chaque `Machine::step` — normalement une instruction,
ou un M-cycle lorsque le CPU est arrêté. Une valeur initiale inchangée ne peut
donc pas produire un faux positif.

`--hardware cgb` sélectionne le CGB natif pour une cartouche couleur et le mode
de compatibilité CGB pour une cartouche DMG. `--hardware agb` fait de même
pour la GBC d'une Game Boy Advance, qui ne s'en distingue que par ses
registres de démarrage. Une boot ROM DMG/CGB acquise
légalement peut être passée par `--boot-rom`; aucune image n'est fournie par
RavenEmu.

## Manifeste versionné

Le schéma normatif lisible par les outils se trouve dans
`conformance-manifest.schema.json`. Chaque test déclare explicitement son
matériel, son timeout, ses mécanismes de réussite et, pour chaque ROM ou boot
ROM ainsi que pour un éventuel framebuffer exact :

- un chemin POSIX relatif à `--rom-root`, sans traversée ni lien symbolique
  permettant de sortir de cette racine ;
- le SHA-256 attendu, vérifié avant toute exécution ;
- l'URL HTTPS d'origine, la licence déclarée et la politique
  `external-only` ou `redistributable`.

Ces métadonnées constituent une garde de provenance reproductible, pas un avis
juridique. RavenEmu n'intègre, ne télécharge et ne redistribue pas l'artefact.
Une licence vide, inconnue ou implicite est refusée par l'orchestrateur.

Exemple minimal (remplacer les métadonnées et l'empreinte par celles de la ROM
réellement fournie) :

```json
{
  "schema_version": 1,
  "suite": { "name": "Conformité locale GB/GBC" },
  "tests": [
    {
      "id": "timer-div",
      "category": "timer",
      "hardware": "dmg",
      "rom": {
        "path": "timer/div.gb",
        "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "source": "https://example.org/project/releases",
        "license": "SPDX-ou-nom-explicite",
        "redistribution": "external-only"
      },
      "timeout_frames": 600,
      "success": {
        "serial": { "pass": "Passed", "fail": "Failed" },
        "register_signature": true
      }
    }
  ]
}
```

Exécution et rapport JSON atomique :

```bash
python3 cores/gb/tests/conformance_manifest.py suite.json \
  --runner build/native-host/gb/gb_conformance_runner \
  --rom-root /chemin/artefacts-gb \
  --report build/conformance-report.json
```

`--category ppu` peut filtrer une famille et `--id` (répétable) un test
précis. Par défaut, une ROM absente est une
erreur ; `--allow-missing` la marque explicitement `skipped`. Un SHA-256
incorrect reste toujours une erreur. Le rapport distingue `pass`, `fail`,
`timeout`, `error`, `crash` et `skipped`, conserve les sorties du runner et
retourne un code non nul dès qu'un test exécuté échoue. `--list` valide le
manifeste et affiche son inventaire sans accéder aux ROMs.

Les catégories acceptées pour les manifests de CI sont : `cpu`, `timing`,
`interrupts`, `timer`, `halt`, `ei`, `stop`, `memory`, `oam-dma`, `vram-dma`,
`ppu`, `stat`, `palettes`, `speed-switch`, `serial`, `apu` et `mbc`.

Deux critères de réussite supplémentaires existent dans `success` :
`screenshot` désigne une capture PNG de 160 × 144 pixels, avec son SHA-256,
que l'orchestrateur décode sans dépendance externe et passe au runner en
RGB888 ; `memory_before` liste l'état intermédiaire exigé par
`--expect-memory-before`. Un test déclare au plus un critère d'image.

## Suites publiques exécutées par la CI

`public-test-roms.json` fige 200 tests issus de l'archive
[game-boy-test-roms v7.0](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0)
de Christoph Sprenger (SHA-256
`b9a9d7a1075aa35a3d07c07c34974048672d8520dca9e07a50178f5860c3832c`), avec le
SHA-256 de chaque ROM et de chaque capture. `fetch_test_roms.sh` télécharge
l'archive, vérifie son empreinte et l'extrait ; aucune ROM n'entre dans le
dépôt :

```bash
cores/gb/tests/fetch_test_roms.sh build/gb-test-roms
cmake -S . -B build -DRAVENEMU_GB_TEST_ROMS=build/gb-test-roms
cmake --build build
ctest --test-dir build -R gb_rom_ --output-on-failure
```

Chaque test devient un test CTest `gb_rom_<id>`. La CI Linux et la passe
ASan/UBSan les exécutent tous.

| Suite | Tests | Critère de réussite |
| --- | --- | --- |
| Mooneye Test Suite `8d742b9d55` (MIT), profils DMG, CGB et AGB décrits ci-dessous | 66 DMG + 58 CGB + 2 AGB | Signature de registres ; `0x42` partout signale un échec |
| Mooneye `emulator-only` (MBC1, MBC2, MBC5) | 28 DMG | Signature de registres |
| Blargg `cpu_instrs`, `instr_timing`, `mem_timing` | 3 DMG + 3 CGB | Texte série `Passed` |
| Blargg `mem_timing-2`, `dmg_sound`, `cgb_sound`, `oam_bug` (ROMs séparées) | 20 DMG + 13 CGB | Résultat en `$A000` précédé de `$80` |
| Blargg `halt_bug`, `interrupt_time` (CGB) et `oam_bug` combiné (DMG) | 2 DMG + 2 CGB | Capture de référence de game-boy-test-roms |
| dmg-acid2 (DMG et compatibilité CGB), cgb-acid2 (MIT) | 1 DMG + 2 CGB | Capture de référence |

La ROM séparée `oam_bug/7-timing_effect` est remplacée par la ROM combinée
`oam_bug.gb` : son texte, écrit en `$A004` sans borne, dépasse les 8 Kio de
RAM de cartouche et déborde sur son propre programme en WRAM. La ROM combinée
exécute le même test et sa capture a été vérifiée sur console.

Les autres suites de l'archive sont plus exigeantes et ne sont pas encore
exécutées par la CI. Mesure avant les corrections de cette campagne, pour
orienter les suivantes : AGE (CGB 2/27, DMG 3/11), gbmicrotest (304/513),
Mealybug Tearoom (CGB 0/27, DMG 1/24), SameSuite (CGB 8/70), scribbltests
(CGB 3/5, DMG 4/5), TurtleTests (2/2), Bully (0/2), mbc3-tester (0/2),
Strikethrough (0/2, 7 pixels) et cgb-acid-hell (0/1, 2 pixels).

## Profil Mooneye de référence

La campagne de référence utilise Mooneye Test Suite au commit complet
`31510e12eea6286d36eea060a6adde755e1067aa` (MIT), archive officielle
`mts-20260714-0944-31510e1.tar.xz`, SHA-256
`6d4fdda2f1d8d2f5f51b0ff3f6f3cc2fbae047aa395a39c82bda3a0e7cbd2641`.
L'archive reste externe au dépôt.

Le profil DMG contient 66 ROMs : les 53 tests `acceptance` sans suffixe de
modèle, les neuf suffixés `-GS`, puis les quatre variantes `-dmgABC` ou
`-dmgABCmgb`. Le profil CGB en mode compatibilité contient les 53 tests
`acceptance` sans suffixe, plus ces cinq tests `misc` :

- `bits/unused_hwio-C.gb` ;
- `boot_div-cgbABCDE.gb` ;
- `boot_hwio-C.gb` ;
- `boot_regs-cgb.gb` ;
- `ppu/vblank_stat_intr-C.gb`.

Les ROMs destinées à un autre modèle ne sont ni exécutées ni comptées comme
des échecs. Chaque rapport doit conserver le commit de la suite, le SHA-256 de
l'archive, le SHA du cœur RavenEmu testé et les deux compteurs séparés. Le
runner reçoit `--hardware dmg` pour le premier profil et `--hardware cgb` pour
le second ; comme ces ROMs ont un en-tête monochrome, ce dernier sélectionne
explicitement le matériel CGB en mode compatibilité. Le profil AGB compte les
deux tests `misc` de la Game Boy Advance, `boot_div-A.gb` et `boot_regs-A.gb`,
exécutés avec `--hardware agb`.

CTest lance un auto-test de l'orchestrateur lorsque Python 3.10 ou ultérieur est
disponible. Il génère sa propre ROM originale en répertoire temporaire, vérifie
les voies série, mémoire et framebuffer ainsi que le refus d'une empreinte
erronée, puis détruit tous
les artefacts temporaires.

Les tests synthétiques internes couvrent en complément les fronts des portes
VRAM/OAM/CRAM, leur échantillonnage à la frontière d'un M-cycle normal ou
double, le démarrage LCD DMG/CGB, le blocage entre sources STAT et
l'auto-incrément des palettes après une écriture refusée. Une suite MBC6 dédiée
génère sa ROM et vérifie séparément les fenêtres ROM/SRAM, les commandes flash,
le tampon de programmation, les protections, la région cachée, la persistance
et les états intermédiaires. Ces tests ne contiennent aucune ROM ou donnée
Nintendo.

La suite MBC7 génère elle aussi son image synthétique. Elle couvre les deux
portes d'activation, les miroirs de registres, le latch des axes, les commandes
EEPROM READ/WRITE/ERASE/WRAL/ERAL et EWEN, le signal occupé/RDY, la persistance
et la restauration au milieu d'un transfert série.

La suite HuC1 vérifie sans ROM externe les six bits de banque ROM, les quatre
banques SRAM toujours accessibles, la sélection exacte du registre IR, ses
miroirs et sa persistance. Deux machines synthétiques partagent aussi un endpoint
local : une LED HuC1 ou `RP` doit atteindre les deux récepteurs de l'autre
machine sans provoquer d'auto-réception.

La suite HuC3 construit une image synthétique `$FE` et teste les sept bits de
banque ROM, les modes SRAM lecture seule/lecture-écriture, les registres miroir
B/C/D/E, le sémaphore occupé, l'index et les commandes sur 256 nibbles. Une
horloge injectable force les rebouclages minute/jour et plusieurs tours de
4 096 jours sans lire l'heure de la CI. Elle couvre aussi le pied de page
versionné `RVH3`, l'import d'une ancienne SRAM brute, les entrées corrompues,
l'IR en compatibilité CGB et la restauration au milieu d'une commande MCU.

La suite DMA interne fixe également le comportement documenté de HDMA pendant
`HALT` : un bloc déjà demandé reste suspendu, puis reprend avant le premier
accès CPU qui suit le réveil. Elle ne prétend pas caractériser les courses de
révisions encore non mesurées lors d'un démarrage HDMA au milieu du HBlank.
