# Organisation du cœur NDS

Le code reste indépendant des frontends, d'Android et de Libretro.
Les contrats publics sont dans `../include/ravenemu/nds/` ; les helpers de
l'interpréteur restent privés à ce répertoire.

| Dossier | Responsabilité |
| --- | --- |
| [`cartridge/`](cartridge/README.md) | En-tête `.nds`, limites des programmes ARM9/ARM7 et propriété des octets |
| [`memory/`](memory/README.md) | Décodage du bus RAM, miroirs, répartition WRAM, transferts alignés |
| [`bios/`](bios/README.md) | Images BIOS optionnelles et fenêtres en lecture seule des vecteurs |
| [`cpu/`](cpu/README.md) | ARM/Thumb, registres bancarisés, CPSR/SPSR, exceptions |
| [`machine/`](machine/README.md) | Chargement direct des exécutables, deux CPU et budgets de pas |

Il n'y a pas encore de PPU, APU, DMA, timers, IPC ni contrôleur d'interruptions
MMIO NDS. Ces composants auront leurs dossiers lorsqu'ils seront implémentés.
Les [tests](../tests/README.md) construisent leurs programmes et leurs images
BIOS en mémoire, sans fichiers tiers. La [documentation du cœur](../README.md)
précise l'état et les limites de l'émulation.
