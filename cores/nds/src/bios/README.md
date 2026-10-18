# Fenêtres BIOS

`boot_rom.cpp` implémente la partie BIOS du bus public `Memory` :

| Processeur | Taille exigée | Fenêtre en lecture seule |
| --- | --- | --- |
| ARM7 | 16 Kio | `0x00000000–0x00003fff` |
| ARM9 | 4 Kio | `0xffff0000–0xffff0fff` |

`Memory::load_bios` copie une image fournie par l'appelant et refuse une taille
différente. Sans image, la lecture d'un vecteur échoue explicitement. Un BIOS
chargé est conservé par `clear_ram` et par le chargement d'une cartouche.

Les tests fabriquent des images synthétiques avec de petits gestionnaires
SWI/IRQ/FIQ et des retours d'exception. Aucun BIOS Nintendo, firmware ou ROM
commerciale n'est fourni. Le code ne prétend pas démarrer le firmware NDS ni
reproduire les restrictions de lecture du BIOS ARM7 selon le PC. La relocation
des vecteurs ARM9 par CP15 n'est pas encore implémentée.
