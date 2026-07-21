#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${KICHAD_INSTALL_ROOT:-${repo_root}/build/install}"
kicad_binary="${install_root}/bin/kichad"
kicad_cli="${install_root}/bin/kichad-cli"

if [[ ! -x "$kicad_binary" || ! -x "$kicad_cli" ]]; then
    echo "KiChad is not installed in ${install_root}." >&2
    echo "Run: ./tools/build-kichad.sh" >&2
    exit 1
fi

exec "$kicad_binary" "$@"
