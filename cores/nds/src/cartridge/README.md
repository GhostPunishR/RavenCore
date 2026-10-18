# Cartouches NDS

`cartridge.cpp` possède l'image ROM et lit l'en-tête NDS en little-endian :
titre, code du jeu, offsets, destinations, tailles et entrées ARM9/ARM7.
Les limites sont vérifiées avant de copier les octets. Une cartouche reste
indépendante du buffer passé par le frontend.

La validation des destinations RAM appartient à [Machine](../machine/README.md).
Cette séparation permet de lire la structure sans prétendre que toute image
acceptée est exécutable. DSi, authentification et déchiffrement KEY1 restent
absents. Aucun contenu commercial ne fait partie des tests.
