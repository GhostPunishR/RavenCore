# Provenance et licences

- **Cœurs GB/GBC/GBA** : code de [GhostPunishR/RavenEmu](https://github.com/GhostPunishR/RavenEmu), importé depuis le commit [312e4af](https://github.com/GhostPunishR/RavenEmu/commit/312e4af765e2005e1f21713f4caf79e96c6f4da5), sous **AGPL-3.0** (voir `LICENSE`), puis modifié dans ce dépôt : validation des états instantanés, correctifs audio et de compilation. L'historique Git de `cores/` détaille chaque changement. Les moteurs restent les cœurs maison RavenEmu ; aucune source d'un cœur tiers n'est substituée.
- **Interface Libretro** : fichier `libretro/include/libretro.h` récupéré depuis [libretro-common@a3194c18ba65ef89734e1898a2f61403c255d368](https://github.com/libretro/libretro-common/blob/a3194c18ba65ef89734e1898a2f61403c255d368/include/libretro.h). Copyright (C) 2010–2024 The RetroArch team, distribué selon les termes MIT figurant en tête de ce fichier.
- **Adaptateurs, exemples et configuration de ce dépôt** : AGPL-3.0, sauf indication contraire.

L'API Libretro est indépendante de RetroArch. L'inclusion de son en-tête ne signifie pas que ce projet est approuvé, référencé ou distribué officiellement par RetroArch ou RetroAchievements.

Les ROMs, firmwares et BIOS commerciaux ne sont pas distribués avec ce dépôt.
