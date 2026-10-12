# RavenEmu C ABI

`ravenemu_c_api` est une interface C opaque par-dessus les vrais moteurs GB, GBC et GBA. L'API est accessible dans `include/ravenemu/bridge.h`. Les écrans et les entrées restent la responsabilité de l'application hôte.

Elle propose le chargement de ROMs, les dimensions et le rendu ARGB8888 d'une frame, les boutons, l'audio stéréo PCM16, les sauvegardes batterie et les save states. Chaque appel est synchrone. Le frontend doit gérer la cadence, l'audio temps réel, le cycle de vie et la persistance.

La bibliothèque est liée statiquement aux moteurs C++20. Les structures C++ ne traversent jamais cette interface. La version 0 de l'API est une préversion : ne pas supposer une ABI stabilisée. Pour connaître les erreurs, utiliser `ravenemu_v3_last_error()` sur le même thread que l'appel ayant échoué.

Pour la compiler :

```sh
cmake -S . -B build -DRAVENEMU_BUILD_LIBRETRO=OFF -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Aucune ROM commerciale ni BIOS n'est nécessaire pour les tests synthétiques.
