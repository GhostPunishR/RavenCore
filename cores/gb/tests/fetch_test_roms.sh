#!/usr/bin/env bash
# Télécharge l'archive publique game-boy-test-roms (Mooneye, Blargg, acid2…),
# vérifie son SHA-256 et l'extrait. Usage : fetch_test_roms.sh DOSSIER
# Puis : cmake ... -DRAVENEMU_GB_TEST_ROMS=DOSSIER && ctest -R gb_rom_
# public-test-roms.json fixe ensuite le SHA-256 de chaque ROM et capture.
set -euo pipefail

destination="${1:?usage : $0 DOSSIER}"
version="v7.0"
url="https://github.com/c-sp/game-boy-test-roms/releases/download/$version/game-boy-test-roms-$version.zip"
sha256="b9a9d7a1075aa35a3d07c07c34974048672d8520dca9e07a50178f5860c3832c"
archive="$destination/game-boy-test-roms-$version.zip"
mkdir -p "$destination"

if command -v sha256sum >/dev/null; then digest() { sha256sum "$1" | cut -d' ' -f1; }
else digest() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

if [ ! -f "$archive" ] || [ "$(digest "$archive")" != "$sha256" ]; then
    curl --fail --silent --show-error --location --retry 3 --output "$archive" "$url"
fi
actual="$(digest "$archive")"
if [ "$actual" != "$sha256" ]; then
    echo "SHA-256 inattendu pour game-boy-test-roms $version : $actual" >&2
    rm -f "$archive"
    exit 1
fi
# zipfile neutralise les chemins absolus et les « .. » des entrées de l'archive.
python3 -m zipfile -e "$archive" "$destination"
echo "game-boy-test-roms $version : OK"
