#!/usr/bin/env bash

set -euo pipefail

launcher_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
install_root="$(cd -- "${launcher_dir}/.." && pwd)"
launcher_name="$(basename -- "$0")"

case "$launcher_name" in
    kichad)
        target="kicad"
        ;;
    kichad-cli)
        target="kicad-cli"
        ;;
    *)
        echo "Unsupported KiChad launcher name: ${launcher_name}" >&2
        exit 2
        ;;
esac

if [[ ! -x "${launcher_dir}/${target}" ]]; then
    echo "KiChad target is missing: ${launcher_dir}/${target}" >&2
    exit 1
fi

export LD_LIBRARY_PATH="${install_root}/lib:${install_root}/lib/kicad${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

exec "${launcher_dir}/${target}" "$@"
