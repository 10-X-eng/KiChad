#!/usr/bin/env bash

set -euo pipefail

launcher_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
install_root="$(cd -- "${launcher_dir}/.." && pwd)"
launcher_name="$(basename -- "$0")"

case "$launcher_name" in
    kicad)
        target_name="kicad"
        ;;
    kicad-cli)
        target_name="kicad-cli"
        ;;
    *)
        echo "Unsupported KiChad launcher name: ${launcher_name}" >&2
        exit 2
        ;;
esac

target="${launcher_dir}/_${target_name}"

if [[ ! -x "$target" ]]; then
    echo "KiChad target is missing: ${target}" >&2
    exit 1
fi

export LD_LIBRARY_PATH="${install_root}/lib:${install_root}/lib/kicad${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# This wrapper always launches the installed tree.  A stale developer-shell setting would make
# KiCad reinterpret installed paths as build-tree paths and look for schemas and KIFACEs in the
# wrong directories.
unset KICAD_RUN_FROM_BUILD_DIR

kicad_cli="${launcher_dir}/_kicad-cli"

if [[ ! -x "$kicad_cli" ]]; then
    echo "KiChad CLI target is missing: ${kicad_cli}" >&2
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

    if [[ -z "${!variable:-}" && -d "$path" ]]; then
        export "${variable}=${path}"
    fi
}

set_versioned_path SYMBOL_DIR "${install_root}/share/kicad/symbols"
set_versioned_path FOOTPRINT_DIR "${install_root}/share/kicad/footprints"
set_versioned_path 3DMODEL_DIR "${install_root}/share/kicad/3dmodels"
set_versioned_path TEMPLATE_DIR "${install_root}/share/kicad/template"

exec "$target" "$@"
