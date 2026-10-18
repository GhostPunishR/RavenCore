# Tests NDS

Toutes les données sont synthétiques, générées en mémoire par les tests.
Aucun téléchargement, BIOS externe ni ROM de jeu n'est nécessaire.

| CTest | Vérifications principales |
| --- | --- |
| `nds_cartridge` | En-têtes, bornes, troncatures et propriété de la ROM |
| `nds_memory` | RAM, miroirs, quatre modes de WRAM et accès refusés |
| `nds_boot` | Copie transactionnelle des programmes et entrées ARM/Thumb |
| `nds_cpu` | ALU, drapeaux, branches, interworking et budgets |
| `nds_calls` | Piles et appels imbriqués, transferts multiples, BLX ARM9 |
| `nds_data` | Lectures signées, produits 32/64 bits et décalages |
| `nds_exceptions` | Banques, CPSR/SPSR, privilèges, SWI/UDF, IRQ/FIQ et retours |

Depuis la racine du dépôt :

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build -R '^nds_' --output-on-failure
```

La CI native et le build ASan/UBSan utilisent aussi ces tests. Les résultats
ne prouvent pas le fonctionnement d'un jeu NDS : les timings, périphériques
et une partie des instructions restent à développer.
