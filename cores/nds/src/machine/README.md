# Machine NDS

`machine.cpp` assemble la cartouche, le bus et deux états CPU indépendants.
Le chargement direct valide les deux programmes avant de préparer une RAM
neuve ; un échec conserve l'ancienne machine. Les BIOS optionnels survivent
au rechargement, tandis que RAM, registres, banques et lignes IRQ/FIQ sont remis
à leur état initial. Les deux CPU démarrent en System avec I/F masqués.

`run_steps(cpu, budget)` avance uniquement le CPU choisi. Le budget est un
nombre de pas, sans horloge : une entrée IRQ/FIQ en consomme un sans exécuter
d'instruction. Le compteur `instructions` n'augmente que pour une instruction
terminée (dont SWI, UDF et condition ARM fausse).

`set_interrupt_lines` est le point de raccordement des futurs contrôleurs
d'interruptions ; il n'implémente ni IME/IE/IF, ni la synchronisation ARM9/ARM7.
Les CPU ne gardent aucune référence persistante vers le bus ; une copie de
machine possède ses propres banques, RAM et images BIOS.
