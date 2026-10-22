# Câble link distant

Le câble link distant relie une console émulée à une console d'un **autre appareil** : deux téléphones, ou un téléphone et un PC. Chaque appareil fait tourner son propre jeu, avec sa ROM et sa sauvegarde. Seules les données du câble voyagent.

RavenCore ne gère ni le réseau, ni la recherche du partenaire. L'application (l'« hôte ») transporte les messages et décide quand brancher le câble. RavenEmu, par exemple, cherche un partenaire sur le réseau local quand le jeu se sert de son port link.

## Principe

Sur une vraie console, un transfert échange les bits des deux consoles au même instant. Par le réseau, la réponse de l'autre console arrive quelques millisecondes plus tard. RavenCore règle cet écart ainsi :

- **La console qui cadence le transfert envoie sa donnée puis attend la réponse**, en temps réel, au cycle exact du transfert. Son temps émulé ne bouge pas pendant l'attente : le jeu ne voit aucun retard, seule l'image marque une pause.
- **L'autre console répond quand elle lit le message.** Elle se comporte comme une console qui attendait l'horloge d'un partenaire : elle reçoit la donnée, rend la sienne et lève son interruption.
- **Les deux consoles appliquent la même décision.** Chaque transfert reçoit une seule réponse, si bien qu'un échange ne peut pas donner deux résultats différents.

Le coût est donc d'**un aller-retour réseau par transfert** pour la console qui cadence. Sur un réseau local (2 à 10 ms), un échange Pokémon prend quelques secondes de plus. Par Internet (50 à 100 ms par aller-retour), c'est trop lent pour jouer.

## Côté hôte (API C)

```c
ravenemu_remote_link* link = ravenemu_remote_link_create(handle, player, send, user);
// ... connexion établie avec l'autre appareil :
ravenemu_remote_link_set_connected(link, 1);
// chaque message reçu de l'autre appareil :
ravenemu_remote_link_receive(link, bytes, size);
// à chaque image, sur le fil d'émulation :
ravenemu_run_frame(handle, pixels, capacity);
ravenemu_remote_link_service(link);  // entre deux images, sans attendre la suivante
// connexion perdue :
ravenemu_remote_link_set_connected(link, 0);
ravenemu_remote_link_destroy(link);
```

L'hôte s'engage à :

- **livrer les messages dans l'ordre et sans perte**, par exemple sur une connexion TCP. RavenCore ne découpe rien : chaque appel de `send` est un message entier, de 14 octets au plus, que l'hôte doit délimiter lui-même, par exemple avec une longueur devant ;
- **brancher le câble une fois la connexion établie**, et le débrancher dès qu'elle est perdue. Débrancher termine sur-le-champ une attente en cours : le transfert finit comme sans câble, et le jeu affiche son propre message d'erreur de liaison ;
- **débrancher avant de détruire**, pour qu'aucun fil n'attende un câble disparu.

Côté fils :

- `send` est appelé depuis le fil qui fait tourner la console, ainsi que depuis celui qui branche le câble. Il doit donc être sûr entre fils et ne pas bloquer longtemps ; une file d'envoi suffit.
- `ravenemu_remote_link_receive` et `ravenemu_remote_link_set_connected` s'appellent de n'importe quel fil.
- Les autres appels suivent la règle du handle : un seul fil à la fois.

`ravenemu_remote_link_state` renvoie l'un de ces états :

| État | Signification |
|---|---|
| `RAVENEMU_REMOTE_UNPLUGGED` | Câble débranché. |
| `RAVENEMU_REMOTE_WAITING` | Câble branché, l'autre ne s'est pas encore annoncé. |
| `RAVENEMU_REMOTE_PLUGGED` | Le câble porte les transferts. |
| `RAVENEMU_REMOTE_INCOMPATIBLE` | Autre version du protocole ou autre famille de consoles. |

Une GB et une GBC se relient entre elles. Une GBA ne se relie qu'à une GBA. Un handle relié par `ravenemu_link_create` (câble local) ne prend pas de câble distant, et l'inverse.

`ravenemu_link_wanted(handle)` vaut 1 tant que le jeu se sert de son port link. Concrètement, il a demandé ou attend un transfert dans les trois dernières secondes, ou une GBA est en mode multijoueur. L'hôte peut s'en servir pour ne chercher un partenaire que lorsque le jeu en veut un.

## Protocole

Tous les messages sont petit-boutistes et commencent par leur nature.

| Nature | Octets | Contenu |
|---|---|---|
| 1 : annonce | 3 | version du protocole (1), famille (1 GB/GBC, 2 GBA) |
| 2 : transfert | 14 | numéro (u32), mode (u8), donnée (u32), durée en cycles (u32) |
| 3 : réponse | 9 | numéro du transfert (u32), donnée (u32) |
| 4 : lignes d'état | 3 | mode (u8), ligne SO basse (0 ou 1) |

Les modes sont :

| Valeur | Mode |
|---|---|
| 0 | Game Boy |
| 1 | GBA normal 8 bits |
| 2 | GBA normal 32 bits |
| 3 | GBA multijoueur |
| 4 | Autre (UART, GPIO) |

Un message mal formé est refusé et ignoré. Sont considérés comme mal formés : une taille inattendue, un mode d'une autre famille, une durée supérieure à un million de cycles. Rien n'est retenu avant l'annonce de l'autre console. Chaque nouveau branchement repart de zéro : les messages et attentes du branchement précédent sont oubliés.

## Game Boy et Game Boy Color

- **Un octet par transfert.** La console qui cadence, horloge interne, envoie son octet entier au premier bit et attend la réponse. Ses huit bits suivent ensuite leur cours au rythme de son horloge, 8 kHz ou 256 kHz en GBC.
- **Lecture du message par l'autre console.** Elle reçoit les huit bits d'un coup, par le même chemin que le câble local. Si elle attend avec une horloge externe, son registre SB change et son interruption se lève ; sinon, elle ne change pas. Dans tous les cas, elle renvoie les bits qu'elle sortait : son bit 7 répété si elle n'attendait pas.
- **Deux consoles qui cadencent en même temps** reçoivent chacune l'octet de l'autre.
- **Fréquence de lecture.** Pendant une image, la console lit les messages à chaque ligne, soit 456 cycles. Entre deux images, l'hôte les fait lire par `ravenemu_remote_link_service`.

## Game Boy Advance

- **Deux joueurs.** `player` 0 est le parent des jeux multijoueurs ; l'autre appareil prend 1.
- **Lignes d'état.** Chaque console envoie son mode et sa ligne SO à chaque changement. Le parent voit ainsi la ligne SD (« tous prêts ») quand l'enfant passe en mode multijoueur ; en mode normal, la ligne SI suit la ligne SO de l'autre.
- **Mode multijoueur.**
  - Le parent envoie sa donnée au départ du transfert, puis attend la réponse à la fin, à son propre cycle.
  - L'enfant entre dans le transfert quand il lit le message, pour sa durée, sur sa propre horloge. Il reçoit SIOMULTI0 (parent) et SIOMULTI1 (lui-même), et 0xFFFF pour les joueurs absents.
  - Un enfant qui n'est pas en mode multijoueur répond 0xFFFF.
- **Mode normal 8 ou 32 bits.**
  - La console à horloge interne cadence. L'autre répond si elle attend : horloge externe et bit Start levé.
  - Sinon, l'autre rend le niveau de repos de sa ligne. C'est aussi le cas si les deux consoles cadencent en même temps.
- **Fréquence de lecture.** Pendant une image, la console avance par tranches de 1024 cycles et lit les messages entre deux, comme le câble local.

## Limites

- **Réseau.** Prévu pour un réseau local. La console qui cadence attend un aller-retour réseau par transfert : un jeu GBA qui enchaîne plusieurs transferts par image peut passer sous 60 images par seconde.
- **Consoles.** Deux consoles seulement, y compris en GBA.
- **Non émulés** : le mode UART, le JOY Bus et l'adaptateur sans fil.
- **États instantanés.** Le câble distant n'entre pas dans l'état d'une console. Recharger un état pendant une liaison casse l'échange en cours ; l'hôte doit l'empêcher.

## Tests

- **`gbc_raven_core` (deux ports série Game Boy reliés par deux câbles distants) :**
  - octets échangés ;
  - interruptions des deux côtés ;
  - transfert sans câble ;
  - collision de deux consoles qui cadencent ;
  - partenaire qui n'attend pas ;
  - débranchement pendant une attente.
- **`gbc_raven_core`, protocole :**
  - annonce ;
  - refus d'une autre famille ou d'une autre version ;
  - messages mal formés ;
  - attente sur un branchement passé.
- **`ravenemu_remote_link`, par l'API C :**
  - deux consoles sur deux fils, reliées par un faux réseau qui livre les messages dans l'ordre avec 0, 5 et 20 ms de latence ;
  - en Game Boy (GBC et GB), un maître et un esclave échangent leurs octets ;
  - en GBA, deux consoles font un transfert multijoueur ; le résultat est celui du câble local ;
  - exclusion du câble local, paramètres refusés, état incompatible et destruction dans les deux ordres.
