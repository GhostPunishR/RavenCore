# Contribuer à RavenEmu Cores

Merci de contribuer aux cœurs maison GB, GBC et GBA de RavenEmu. Les petites corrections documentées et les tests de régression sont aussi utiles que les nouvelles fonctions.

Avant de contribuer, consultez le [code de conduite](CODE_OF_CONDUCT.md), la [licence AGPL-3.0](LICENSE) et les [attributions](NOTICE.md).

## Par où commencer ?

- **Un bug ou un jeu incompatible ?** Ouvrez une issue avec le modèle correspondant, après avoir recherché les signalements existants.
- **Une idée de fonctionnalité ?** Décrivez le besoin dans une issue avant une modification importante.
- **Une correction ?** Proposez une pull request courte, ciblée et accompagnée de tests quand cela est pertinent.

Ne publiez jamais de ROM commerciale, de BIOS, de sauvegarde personnelle, de clés ou de contenu protégé dans une issue, une PR ou les tests. Les ROMs de conformité doivent être fournies séparément et utilisées dans le respect de leurs licences.

## Préparer l'environnement

Prérequis : CMake 3.22.1 ou plus, Git et un compilateur C++20.

```bash
git clone https://github.com/GhostPunishR/RavenEmu-Cores.git
cd RavenEmu-Cores
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Pour ne compiler que les moteurs, désactivez Libretro avec `-DRAVENEMU_BUILD_LIBRETRO=OFF`. La CI compile et teste les moteurs et les trois adaptateurs Libretro sur Linux, y compris sous ASan/UBSan.

Les tests GB/GBC disposent aussi d'un [runner de conformité](cores/gb/tests/README.md). Ses ROMs de test sont externes au dépôt. N'annoncez pas une suite comme réussie sans l'avoir exécutée et sans indiquer les résultats.

## Organisation du code

| Dossier | Contenu |
| --- | --- |
| `cores/common/` | Contrats et outils C++ partagés |
| `cores/gb/` | Moteur Game Boy et composants GB/GBC communs |
| `cores/gbc/` | Composants CGB et façade du cœur couleur |
| `cores/gba/` | Moteur Game Boy Advance |
| `libretro/` | Adaptateurs, métadonnées et tests Libretro |
| `sdk/` | API C (`bridge.h`), fabrique C++ et paquet CMake |

Conservez la séparation entre la logique matérielle et les frontends. Les dossiers `cores/` ne doivent pas dépendre d'Android, de JNI ou de RetroArch.

## Règles de contribution

1. Écrivez du C++20 lisible, cohérent avec le module modifié et sans nouvelle dépendance injustifiée.
2. Pour une correction de CPU, PPU, APU, timer, DMA, cartouche ou sauvegarde, ajoutez un test de régression ou expliquez pourquoi ce n'est pas possible.
3. Documentez les changements de compatibilité, de timings, de sauvegardes batterie et de format des états instantanés. Évitez de casser les anciennes sauvegardes sans avertissement.
4. N'intégrez pas de code copié, traduit ou adapté d'un autre émulateur. Utilisez des spécifications et documentations matérielles publiques, avec leurs références lorsque cela aide à vérifier un comportement.
5. Pour Libretro, préservez l'interface C et vérifiez le chargement, la vidéo, l'audio, les entrées et les sauvegardes. N'annoncez pas RetroAchievements ou Hardcore sans une implémentation et une validation effectives.
6. Ne joignez aucun fichier de jeu, BIOS, firmware propriétaire ou donnée personnelle. Un nom de jeu et une empreinte SHA-256 facultative peuvent aider à reproduire un bug.

## Proposer une pull request

Créez une branche dédiée à partir de `main`, limitez les changements au problème traité et expliquez ce que vous avez testé. Utilisez le [modèle de PR](.github/PULL_REQUEST_TEMPLATE.md).

Une PR doit idéalement mentionner : le moteur concerné, le comportement avant et après, les tests exécutés et les limites restantes. Signalez si le changement mérite aussi d'être synchronisé avec le dépôt [RavenEmu Android](https://github.com/GhostPunishR/RavenEmu).

Les changements sont examinés avant intégration. Les tests automatisés doivent réussir, mais ne remplacent pas la validation des jeux et du matériel concernés.
