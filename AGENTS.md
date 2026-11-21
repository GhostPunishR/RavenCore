# Consignes de RavenCore pour les agents

> Consignes pour les agents et contributeurs qui travaillent sur RavenCore.
> Les règles de contribution, l'organisation du code et la carte des fichiers
> du cœur GB/GBC sont dans [CONTRIBUTING.md](CONTRIBUTING.md) ; les tests et
> leurs mesures dans [cores/gbc/tests/README.md](cores/gbc/tests/README.md).

## Refactoriser quand le code devient trop long

Avant d'ajouter du code à un fichier source de plus de **800 lignes**, à un
fichier qui regroupe plusieurs composants matériels ou à une fonction de plus
de **80 lignes**, le découper :

- **un fichier par composant**, nommé d'après lui, dans le dossier de son
  composant matériel : `ppu/background_fetcher.hpp`, `apu/square_channel.hpp`,
  `memory/oam_dma.hpp`. La carte du cœur GB/GBC est dans CONTRIBUTING.md ;
- un fichier de test de plus de **1 000 lignes** se découpe de même, par
  composant testé, quand on y ajoute des tests ;
- exceptions : en-têtes tiers (`libretro/include/libretro.h`), fichiers
  générés et tables de données (`ppu/compatibility_palettes.hpp`).

Le refactor est un changement à part :

1. un commit dédié, **sans changement de comportement**, jamais mêlé à un
   correctif ;
2. la preuve que rien ne change : tests unitaires et ROMs de test de la CI,
   puis sorties identiques octet pour octet avant et après (états
   instantanés internes pris à des instants arbitraires, images, son,
   mesures des suites publiques) ;
3. un format d'état inchangé : l'ordre des champs suit les fonctions `save`
   et `load` ;
4. une performance mesurée (instructions exécutées, par exemple avec
   `valgrind --tool=lackey`) : un découpage ne doit rien coûter. Dans les
   boucles cadencées à chaque point, passer les fonctions de rappel par
   valeur plutôt que par référence.
