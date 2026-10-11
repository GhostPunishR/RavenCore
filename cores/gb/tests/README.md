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
- l'arrêt inattendu du CPU, les exceptions et le timeout.

`--minimum-pass-frames` retarde uniquement les voies de réussite ; les erreurs
série restent immédiates. Pour une réussite mémoire, l'orchestrateur impose en
plus `--memory-require-mismatch` : l'ensemble des attentes doit d'abord avoir
été observé dans un état différent, puis rester conforme pendant le nombre de
points d'observation déclaré par `memory_stability_samples` (au moins deux).
Un point est pris après chaque `Machine::step` — normalement une instruction,
ou un M-cycle lorsque le CPU est arrêté. Une valeur initiale inchangée ne peut
donc pas produire un faux positif.

`--hardware cgb` sélectionne le CGB natif pour une cartouche couleur et le mode
de compatibilité CGB pour une cartouche DMG. Une boot ROM DMG/CGB acquise
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

`--category ppu` peut filtrer une famille. Par défaut, une ROM absente est une
erreur ; `--allow-missing` la marque explicitement `skipped`. Un SHA-256
incorrect reste toujours une erreur. Le rapport distingue `pass`, `fail`,
`timeout`, `error`, `crash` et `skipped`, conserve les sorties du runner et
retourne un code non nul dès qu'un test exécuté échoue. `--list` valide le
manifeste et affiche son inventaire sans accéder aux ROMs.

Les catégories acceptées pour les manifests de CI sont : `cpu`, `timing`,
`interrupts`, `timer`, `halt`, `ei`, `stop`, `memory`, `oam-dma`, `vram-dma`,
`ppu`, `stat`, `palettes`, `speed-switch`, `serial`, `apu` et `mbc`.

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
explicitement le matériel CGB en mode compatibilité.

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
