# Provenance et licences

- **Cœurs GB/GBC/GBA** : code de [GhostPunishR/RavenEmu](https://github.com/GhostPunishR/RavenEmu), importé depuis le commit [312e4af](https://github.com/GhostPunishR/RavenEmu/commit/312e4af765e2005e1f21713f4caf79e96c6f4da5), sous **AGPL-3.0** (voir `LICENSE`), puis modifié dans ce dépôt : validation des états instantanés, correctifs audio et de compilation. L'historique Git de `cores/` détaille chaque changement. Les moteurs restent les cœurs maison RavenEmu ; aucune source d'un cœur tiers n'est substituée.
- **Interface Libretro** : fichier `libretro/include/libretro.h` récupéré depuis [libretro-common@a3194c18ba65ef89734e1898a2f61403c255d368](https://github.com/libretro/libretro-common/blob/a3194c18ba65ef89734e1898a2f61403c255d368/include/libretro.h). Copyright (C) 2010–2024 The RetroArch team, distribué selon les termes MIT figurant en tête de ce fichier.
- **Palettes de compatibilité de la GBC** : les tables de `cores/gb/src/ppu/compatibility_palettes.hpp` (couleurs que la boot ROM de la GBC donne à chaque jeu Game Boy, et celles des combinaisons de touches) reprennent les données de [The Cutting Room Floor, « Notes:Game Boy Color Bootstrap ROM »](https://tcrf.net/Notes:Game_Boy_Color_Bootstrap_ROM) (révision 2183057), sous licence [Creative Commons Attribution 3.0 Unported](https://creativecommons.org/licenses/by/3.0/). Modification : couleurs rendues de leur RGB 24 bits au RGB555 de la console. Le choix d'après le titre suit Pan Docs, « Compatibility palettes ».
- **Adaptateurs, exemples et configuration de ce dépôt** : AGPL-3.0, sauf indication contraire.

L'API Libretro est indépendante de RetroArch. L'inclusion de son en-tête ne signifie pas que ce projet est approuvé, référencé ou distribué officiellement par RetroArch ou RetroAchievements.

Les ROMs, firmwares et BIOS commerciaux ne sont pas distribués avec ce dépôt.
