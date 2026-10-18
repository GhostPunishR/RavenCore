# CPU ARM9 / ARM7

| Fichier | Responsabilité |
| --- | --- |
| `cpu.cpp` | API publique, pas CPU, échantillonnage IRQ/FIQ et diagnostics |
| `cpu_state.cpp` | CPSR/SPSR et banques de registres des sept modes |
| `arm.cpp` | Décodage ARM, ALU, adressages et multiplications |
| `thumb.cpp` | Décodage Thumb, pile et appels en deux demi-instructions |
| `transfer.cpp` | Transferts multiples et restauration SPSR par LDM |
| `psr.cpp` | MRS/MSR, masques des champs et restrictions du mode User |
| `exceptions.cpp` | Sauvegarde d'état, banques et vecteurs d'entrée d'exception |
| `interpreter.hpp` | Copie de travail d'un pas et déclarations des décodeurs |
| `support.hpp` | Conditions, extension de signe, retenues et décalages |

`CpuState` est une valeur copiable : la copie de travail est validée avant de
remplacer l'état vivant. Les instructions absentes restent des erreurs de
diagnostic ; elles ne sont pas toutes assimilées à une instruction indéfinie.
SWI et les encodages explicites UDF entrent dans leurs modes d'exception.
Une ligne FIQ démasquée est prioritaire sur IRQ, à la frontière d'un pas.

Les modes User/System partagent SP/LR ; SVC, IRQ, FIQ, Abort et Undefined
ont chacun SP/LR et SPSR. FIQ possède en plus ses r8-r12. Les retours restaurent
le CPSR avant d'aligner le PC, et LDM réécrit d'abord la base dans l'ancienne
banque. Les vecteurs sont hauts sur ARM9 et bas sur ARM7 dans ce modèle NDS.

Références comportementales : ARM Architecture Reference Manual, ARMv4T/ARMv5TE,
chapitres du modèle programmeur, exceptions, MRS/MSR et retours d'exception ;
GBATEK pour l'organisation mémoire NDS. CP15, gestion des abandons mémoire,
instructions DSP, timings et pipeline restent absents.
