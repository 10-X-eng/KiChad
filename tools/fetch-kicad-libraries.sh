#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
library_root="${KICHAD_LIBRARY_ROOT:-${repo_root}/build/libraries}"

mkdir -p "$library_root"

sync_library() {
    local name="$1"
    local url="$2"
    local destination="${library_root}/${name}"

    if [[ -d "${destination}/.git" ]]; then
        echo "Updating ${name}..."
        git -C "$destination" pull --ff-only
    elif [[ -e "$destination" ]]; then
        echo "Refusing to replace non-Git path: ${destination}" >&2
        return 1
    else
        echo "Fetching ${name}..."
        git clone --depth 1 "$url" "$destination"
    fi
}

sync_library kicad-symbols https://gitlab.com/kicad/libraries/kicad-symbols.git
sync_library kicad-footprints https://gitlab.com/kicad/libraries/kicad-footprints.git
sync_library kicad-packages3D https://gitlab.com/kicad/libraries/kicad-packages3D.git
sync_library kicad-templates https://gitlab.com/kicad/libraries/kicad-templates.git

echo "KiCad libraries are available in ${library_root}"
