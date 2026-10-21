# Mémoire RetroAchievements

Les succès lisent la mémoire du jeu aux adresses que fixe rcheevos pour chaque console. RavenCore la rend par `read_achievement_memory()` (API C : `ravenemu_read_achievement_memory()`), que RavenEmu utilise directement. Les cœurs Libretro en déclarent une copie à RetroArch (`RETRO_ENVIRONMENT_SET_MEMORY_MAPS`), dans la même disposition.

## Copie Libretro

RetroArch garde ses pointeurs pendant toute la partie, alors que RavenCore recrée sa machine à chaque reset et à chaque chargement d'état. Le cœur expose donc une copie de l'espace, à adresse fixe, rafraîchie après chaque image (quand les succès sont évalués), après un reset et après un chargement d'état. Une écriture du frontend dans cette copie n'atteint pas le jeu : les codes de triche ne sont pas pris en charge.

Chaque région est découpée en descripteurs dont la longueur est une puissance de deux et le départ aligné sur elle, comme l'exige `libretro.h` sans masque `select`. Une région absente du jeu (pas de RAM de cartouche, Game Boy sans les banques de la Game Boy Color) n'a pas de descripteur : aucune zone n'est remplie de zéros à la place de données.

## Game Boy et Game Boy Color

Adresse RetroAchievements = adresse de la console, puis les banques supplémentaires au-delà de 0xFFFF, comme dans la table de rcheevos.

| Adresses | Taille | Contenu |
| --- | --- | --- |
| 0x0000-0x3FFF | 16 Kio | ROM, première fenêtre : la banque 0, ou celle que choisissent le MBC1 en mode 1 et le MMM01 |
| 0x4000-0x7FFF | 16 Kio | ROM, banque paginée choisie par le jeu |
| 0x8000-0x9FFF | 8 Kio | VRAM, banque 0 |
| 0xA000-0xBFFF | 8 Kio au plus | RAM de cartouche, banque 0 |
| 0xC000-0xDFFF | 8 Kio | WRAM, banques 0 et 1 (rcheevos : « System RAM (bank 1) » en 0xD000) |
| 0xE000-0xFDFF | | miroir de la WRAM : RetroArch le renvoie sur 0xC000 |
| 0xFE00-0xFE9F | 160 octets | OAM |
| 0xFF00-0xFF7F | 128 octets | registres d'E/S, tels que le CPU les lit |
| 0xFF80-0xFFFF | 128 octets | HRAM, puis IE |
| 0x10000-0x15FFF | 24 Kio | WRAM de la Game Boy Color, banques 2 à 7 |
| 0x16000-0x33FFF | 120 Kio au plus | RAM de cartouche, banques 1 à 15 |

Ce que lit un succès est ce que lit le CPU :
- **ROM.** Les deux fenêtres passent par le contrôleur de la cartouche et suivent chaque changement de banque, d'une image à l'autre. Pour une fenêtre qui est une simple banque (MBC0, MBC1, MBC2, MBC3, MBC5, MBC7, HuC1, HuC3, MMM01 après son menu), la copie se fait d'un bloc. Le MBC6 et le menu du MMM01 se lisent octet par octet.
- **Registres d'E/S.** Ils se lisent par le même code que les lectures du CPU, bits inutilisés et registres absents compris, sans effet de bord sur l'émulation.
- **Coût.** Moins de 2 µs par image sur un PC pour les deux fenêtres de ROM et les E/S.

Limites :
- La zone 0xFEA0-0xFEFF, inutilisée, n'est pas exposée : un succès qui la lit est signalé non pris en charge.
- La banque 1 de la VRAM de la Game Boy Color n'est pas exposée ; rcheevos ne la place pas dans son espace.
- **MBC2 :** sa RAM de 512 demi-octets est exposée telle qu'elle est stockée, bits hauts à 0, alors que le CPU relit `0xF0 | valeur`.
- **MBC7 :** sa mémoire est une EEPROM série, pas une RAM de cartouche ; elle n'est pas exposée.
- **ROM de démarrage :** pendant le démarrage, les fenêtres montrent la ROM de la cartouche, pas la ROM de démarrage que le CPU exécute.

## Game Boy Advance

| Adresses | Taille | Contenu |
| --- | --- | --- |
| 0x00000-0x07FFF | 32 Kio | IWRAM (0x03000000) |
| 0x08000-0x47FFF | 256 Kio | EWRAM (0x02000000) |
| 0x48000-0x57FFF | 64 Kio au plus | puce de sauvegarde (0x0E000000) |

Limites :
- La puce de sauvegarde est exposée telle qu'elle est stockée : SRAM 32 Kio sans miroir, Flash 64 Kio, EEPROM 512 octets ou 8 Kio. D'une Flash de 128 Kio, seule la banque 0 est visible, quelle que soit la banque choisie par le jeu.
- Coût : 24 µs par image sur un PC (0,7 % d'une image de Pokémon Émeraude).

## Vérification

- `gb_raven_core` :
  - bornes de chaque région, et lecture par blocs égale aux lectures d'un octet, y compris à cheval sur deux régions ;
  - registres d'E/S égaux à ce qu'une ROM de test en recopie par le CPU ;
  - banque paginée choisie par un MBC5, première fenêtre commutée par un MBC1 en mode 1.
- `gb_cartridge_mbc` : pour chaque contrôleur et chaque banque choisie, la copie d'une fenêtre est, octet pour octet, la lecture du CPU.
- `libretro_smoke_gb`, `libretro_smoke_gbc` et `libretro_smoke_gba` :
  - carte déclarée aux adresses de la console, en descripteurs que RetroArch accepte ;
  - copie qui suit chaque image, un changement de banque, un reset et un chargement d'état.
- Vérifié en local avec `rc_libretro`, le code de rcheevos qui relie un cœur Libretro aux succès dans RetroArch, compilé hors du dépôt. Pas encore essayé en jeu dans RetroArch, ni testé par RetroAchievements.
