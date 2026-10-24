# API C de RavenCore

`ravenemu_c_api` est une interface C opaque par-dessus les vrais moteurs GB, GBC et GBA. L'API est accessible dans `include/ravenemu/bridge.h`. Les écrans et les entrées restent la responsabilité de l'application hôte.

Elle propose le chargement de ROMs, les dimensions et le rendu ARGB8888 d'une frame, les boutons, l'audio stéréo PCM16, les sauvegardes batterie et les save states. Chaque appel est synchrone. Le frontend doit gérer la cadence, l'audio temps réel, le cycle de vie et la persistance.

`ravenemu_api_version()` doit être comparé à `RAVENEMU_API_VERSION` avant tout autre appel, et `ravenemu_version()` donne la version de la bibliothèque. Pour la sauvegarde batterie, `ravenemu_battery_dirty()` indique si elle a changé ; il suffit alors de la copier avec `ravenemu_copy_battery_generation()`, de l'écrire, puis d'acquitter la génération reçue avec `ravenemu_acknowledge_battery()`. Une écriture du jeu survenue après la copie la laisse marquée comme modifiée. Pour un jeu GBA à horloge, la sauvegarde se termine par les réglages de l'horloge, dans le bloc de 16 octets de mGBA ; le frontend la conserve telle quelle.

Pour un jeu GBA dont la détection échoue, `ravenemu_set_gba_save_type()` impose le type de sauvegarde et `ravenemu_set_gba_rtc()` la présence de l'horloge de cartouche. Ces réglages prennent effet au prochain `ravenemu_load()` ou `ravenemu_reset()`. Ils échouent sur un handle GB ou GBC. `ravenemu_set_gba_bios()` donne de même le BIOS d'une console (16 384 octets, copiés) à la place de celui de RavenCore ; une taille 0 y revient. Le vrai BIOS joue son animation de démarrage, sauf si `skip_intro` est non nul. Un état instantané ne se recharge qu'avec le BIOS qui l'a créé, et garde le BIOS en place. Après chaque `ravenemu_run_frame()`, `ravenemu_rumble_active()` indique si le moteur de vibration de la cartouche tourne (GB MBC5 Rumble, Drill Dozer, WarioWare: Twisted!) ; le frontend le transmet au vibreur de l'appareil.

`ravenemu_sensors()` indique les capteurs de la cartouche chargée : solaire (Boktai 1 à 3), rotation (WarioWare: Twisted!) et inclinaison (Yoshi Topsy-Turvy, Koro Koro Puzzle et cartouches GB MBC7). Le frontend les alimente avec les capteurs de l'appareil ou avec ses commandes : `ravenemu_set_solar_light()` (0, obscurité, à 255, plein soleil), `ravenemu_set_rotation()` (-32768 à 32767, positif dans le sens horaire) et `ravenemu_set_tilt()` (-32768 à 32767 par axe pour -1 g à 1 g). Ces valeurs survivent au chargement, au reset et aux états instantanés ; elles sont ignorées sans le capteur correspondant.

`ravenemu_link_create()` relie des handles de la même application par un câble link, par exemple deux jeux côte à côte sur un même appareil : deux Game Boy ou Game Boy Color (mélangées si besoin), ou deux à quatre GBA dont la première est la console parente des jeux multijoueurs. Chaque handle garde sa ROM, sa sauvegarde, ses états et son audio. Une fois reliés, les handles avancent ensemble par `ravenemu_link_run_frame()`, qui remplit une image par handle ; `ravenemu_run_frame()` échoue sur eux. Les consoles avancent à tour de rôle par tranches : une ligne (456 cycles) sur Game Boy, 1024 cycles sur GBA. Côté GBA, un transfert multijoueur voit chaque console au cycle exact, un transfert en mode normal plus court qu'une tranche lit l'autre console là où elle en est. Détruire un handle relié débranche le câble pour tous ; `ravenemu_link_destroy()` reste à appeler.

`ravenemu_remote_link_create()` relie un handle à une console d'**un autre appareil** : l'application transporte les messages dans l'ordre et sans perte (une connexion TCP, par exemple), puis branche le câble par `ravenemu_remote_link_set_connected()`. La console qui cadence un transfert attend la réponse de l'autre en temps réel, dans `ravenemu_run_frame()` : son temps émulé ne bouge pas, le jeu ne voit aucun retard. L'autre répond depuis sa propre `ravenemu_run_frame()`, ou entre deux images depuis `ravenemu_remote_link_service()`. Deux consoles : GB/GBC entre elles, GBA entre elles, le joueur 0 étant le parent GBA. `ravenemu_link_wanted()` dit quand le jeu se sert de son port link, pour ne chercher un partenaire qu'à ce moment. Protocole, fils et limites : [docs/remote_link.md](../docs/remote_link.md).

`ravenemu_set_cheats()` active une liste de codes de triche à partir de l'image suivante ; une liste vide les retire. Chaque entrée est un code d'un menu de triche, en une ou plusieurs lignes séparées par des espaces, des retours à la ligne ou des « + » :

- Game Boy et Game Boy Color : Game Genie (`ABC-DEF-GHI` ou `ABC-DEF`) et GameShark (8 chiffres, comme `010238CD`) ;
- GBA : Codebreaker non chiffré (`XAAAAAAA YYYY`), GameShark v1/v2 et Pro Action Replay v3 chiffrés tels que l'appareil les affiche (`XXXXXXXX YYYYYYYY`), reconnus à leur déchiffrement.

`ravenemu_check_cheat()` vérifie un code sans handle, au moment où le joueur le saisit. Un code refusé laisse sa raison, en français, dans `ravenemu_last_error()`, par exemple « le code GameShark « 01021234 » vise la ROM : c'est le rôle d'un code Game Genie ». Si une entrée de `ravenemu_set_cheats()` est refusée, la liste précédente reste active, et l'erreur nomme l'entrée, numérotée à partir de 1. Les codes survivent au chargement, au reset et aux états instantanés, sans y être enregistrés. Restent refusés : les patchs de ROM GBA, le changement de chiffrement `DEADFACE` et les codes Codebreaker chiffrés.

`ravenemu_set_gbc_colors()` choisit les couleurs d'un jeu Game Boy sur un handle `RAVENEMU_GBC` ou `RAVENEMU_GBC_ON_GBA` sans boot ROM. `RAVENEMU_GBC_COLORS_AUTO` donne les couleurs que la boot ROM de la GBC donne à un jeu qu'elle ne reconnaît pas, `RAVENEMU_GBC_COLORS_GRAY` quatre gris. Le réglage s'applique tout de suite et survit au chargement, au reset et aux états.

`ravenemu_create(RAVENEMU_GBC_ON_GBA)` crée la Game Boy Color d'une Game Boy Advance. Elle lance les jeux GBC et Game Boy comme `RAVENEMU_GBC`, mais les démarre avec B = 1 (B + 1 pour un jeu Game Boy). Les jeux qui testent ce registre débloquent leurs bonus GBA, comme Zelda Oracle ou Shantae. Elle se relie par câble comme une GBC.

`ravenemu_read_achievement_memory()` lit l'espace d'adresses de RetroAchievements sans faire avancer l'émulation. Une zone non exposée donne une lecture plus courte que demandé : le frontend doit utiliser le nombre d'octets renvoyé.

La bibliothèque est liée statiquement aux moteurs C++20. Les structures C++ ne traversent jamais cette interface. La version 0 de l'API est une préversion : ne pas supposer une ABI stabilisée. Pour connaître les erreurs, utiliser `ravenemu_last_error()` sur le même thread que l'appel ayant échoué.

Pour la compiler :

```sh
cmake -S . -B build -DRAVENEMU_BUILD_LIBRETRO=OFF -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Aucune ROM commerciale ni BIOS n'est nécessaire pour les tests synthétiques.

## Utiliser le paquet CMake

Installer les moteurs, l'API C et leurs en-têtes, puis les consommer depuis un autre projet :

```sh
cmake --install build --prefix /chemin/ravencore
```

```cmake
find_package(RavenCore 0.14 REQUIRED)
target_link_libraries(mon_app PRIVATE RavenCore::ravenemu_c_api)  # ou RavenCore::ravencore en C++
```

Passer `-DCMAKE_PREFIX_PATH=/chemin/ravencore` à la configuration du projet consommateur. Avant la version 1.0, seules les versions de même mineure sont déclarées compatibles. Les adaptateurs Libretro ne sont pas installés : ils se copient dans les dossiers du frontend.
