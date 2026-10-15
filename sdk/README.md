# RavenEmu C ABI

`ravenemu_c_api` est une interface C opaque par-dessus les vrais moteurs GB, GBC et GBA. L'API est accessible dans `include/ravenemu/bridge.h`. Les écrans et les entrées restent la responsabilité de l'application hôte.

Elle propose le chargement de ROMs, les dimensions et le rendu ARGB8888 d'une frame, les boutons, l'audio stéréo PCM16, les sauvegardes batterie et les save states. Chaque appel est synchrone. Le frontend doit gérer la cadence, l'audio temps réel, le cycle de vie et la persistance.

`ravenemu_api_version()` doit être comparé à `RAVENEMU_API_VERSION` avant tout autre appel, et `ravenemu_version()` donne la version de la bibliothèque. Pour la sauvegarde batterie, `ravenemu_battery_dirty()` indique si elle a changé ; il suffit alors de la copier avec `ravenemu_copy_battery_generation()`, de l'écrire, puis d'acquitter la génération reçue avec `ravenemu_acknowledge_battery()`. Une écriture du jeu survenue après la copie la laisse marquée comme modifiée.

Pour un jeu GBA dont la détection échoue, `ravenemu_set_gba_save_type()` impose le type de sauvegarde et `ravenemu_set_gba_rtc()` la présence de l'horloge de cartouche. Ces réglages prennent effet au prochain `ravenemu_load()` ou `ravenemu_reset()`. Ils échouent sur un handle GB ou GBC. Après chaque `ravenemu_run_frame()`, `ravenemu_rumble_active()` indique si le moteur de vibration de la cartouche tourne (GB MBC5 Rumble, Drill Dozer, WarioWare: Twisted!) ; le frontend le transmet au vibreur de l'appareil.

`ravenemu_sensors()` indique les capteurs de la cartouche chargée : solaire (Boktai 1 à 3), rotation (WarioWare: Twisted!) et inclinaison (Yoshi Topsy-Turvy, Koro Koro Puzzle et cartouches GB MBC7). Le frontend les alimente avec les capteurs de l'appareil ou avec ses commandes : `ravenemu_set_solar_light()` (0, obscurité, à 255, plein soleil), `ravenemu_set_rotation()` (-32768 à 32767, positif dans le sens horaire) et `ravenemu_set_tilt()` (-32768 à 32767 par axe pour -1 g à 1 g). Ces valeurs survivent au chargement, au reset et aux états instantanés ; elles sont ignorées sans le capteur correspondant.

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
cmake --install build --prefix /chemin/ravenemu
```

```cmake
find_package(RavenCore 0.2 REQUIRED)
target_link_libraries(mon_app PRIVATE RavenEmu::ravenemu_c_api)  # ou RavenEmu::ravenemu_cores en C++
```

Passer `-DCMAKE_PREFIX_PATH=/chemin/ravenemu` à la configuration du projet consommateur. Avant la version 1.0, seules les versions de même mineure sont déclarées compatibles. Les adaptateurs Libretro ne sont pas installés : ils se copient dans les dossiers du frontend.

L'ancien nom du paquet, `find_package(RavenEmuCores)`, fonctionne jusqu'à la version 1.0 : il charge `RavenCore` et affiche un avertissement de dépréciation. Les cibles `RavenEmu::` sont les mêmes.
