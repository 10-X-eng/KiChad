#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${KICHAD_INSTALL_ROOT:-${repo_root}/build/install}"
library_root="${KICHAD_LIBRARY_ROOT:-${repo_root}/build/libraries}"
kicad_binary="${install_root}/bin/kichad"
kicad_cli="${install_root}/bin/kichad-cli"

if [[ ! -x "$kicad_binary" || ! -x "$kicad_cli" ]]; then
    echo "KiChad is not installed in ${install_root}." >&2
    echo "Run: ./tools/build-kichad.sh" >&2
    exit 1
fi

version="$($kicad_cli version)"
major="${version%%.*}"

if [[ ! "$major" =~ ^[0-9]+$ ]]; then
    echo "Could not determine the KiCad major version from: ${version}" >&2
    exit 1
fi

set_versioned_path() {
    local suffix="$1"
    local path="$2"
    local variable="KICAD${major}_${suffix}"

    if [[ -d "$path" ]]; then
        export "${variable}=${path}"
    fi
}

set_versioned_path SYMBOL_DIR "${library_root}/kicad-symbols"
set_versioned_path FOOTPRINT_DIR "${library_root}/kicad-footprints"
set_versioned_path 3DMODEL_DIR "${library_root}/kicad-packages3D"
set_versioned_path TEMPLATE_DIR "${library_root}/kicad-templates"

export LD_LIBRARY_PATH="${install_root}/lib:${install_root}/lib/kicad${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

exec "$kicad_binary" "$@"
