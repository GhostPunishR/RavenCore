# Bus mémoire NDS

`bus.cpp` décode la RAM principale partagée (4 Mio), les 32 Kio de WRAM
partagée et les 64 Kio privés ARM7, avec leurs miroirs. Les quatre modes
WRAMCNT changent l'attribution des mêmes octets sans les effacer.

Les lectures consultent aussi les fenêtres [BIOS](../bios/README.md).
Les écritures n'atteignent que la RAM ; `write_words` valide toutes les
destinations avant le premier mot. Les accès 16/32 bits doivent être alignés.
`clear_ram()` remet RAM et WRAMCNT à zéro sans supprimer les images BIOS.

Il n'y a pas encore de MMIO, VRAM, TCM, timing ni open-bus. Un accès non
mappé est une erreur de diagnostic ; il ne lève pas un Data Abort matériel.
