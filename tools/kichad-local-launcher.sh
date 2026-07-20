#!/usr/bin/env bash

set -euo pipefail

launcher_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
install_root="$(cd -- "${launcher_dir}/.." && pwd)"
launcher_name="$(basename -- "$0")"

case "$launcher_name" in
    kichad)
        target_name="kicad.real"
        ;;
    kichad-cli)
        target_name="kicad-cli.real"
        ;;
    *)
        echo "Unsupported KiChad launcher name: ${launcher_name}" >&2
        exit 2
        ;;
esac

target="${launcher_dir}/${target_name}"

if [[ ! -x "$target" ]]; then
    echo "KiChad target is missing: ${target}" >&2
    exit 1
fi

export LD_LIBRARY_PATH="${install_root}/lib:${install_root}/lib/kicad${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

exec "$target" "$@"
