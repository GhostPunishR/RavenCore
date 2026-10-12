#!/usr/bin/env bash
# Télécharge les ROMs de test GBA publiques listées dans external_test_roms.txt
# et vérifie leur SHA-256. Usage : fetch_test_roms.sh DOSSIER
# Puis : cmake ... -DRAVENEMU_GBA_TEST_ROMS=DOSSIER && ctest -R gba_rom_
set -euo pipefail

destination="${1:?usage : $0 DOSSIER}"
manifest="$(dirname "$0")/external_test_roms.txt"
mkdir -p "$destination"

if command -v sha256sum >/dev/null; then digest() { sha256sum "$1" | cut -d' ' -f1; }
else digest() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

while read -r name url sha256 _; do
    case "$name" in ''|\#*) continue ;; esac
    rom="$destination/$name.gba"
    if [ ! -f "$rom" ] || [ "$(digest "$rom")" != "$sha256" ]; then
        curl --fail --silent --show-error --location --retry 3 --output "$rom" "$url"
    fi
    actual="$(digest "$rom")"
    if [ "$actual" != "$sha256" ]; then
        echo "SHA-256 inattendu pour $name : $actual" >&2
        rm -f "$rom"
        exit 1
    fi
    echo "$name : OK"
done < "$manifest"
