#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

"${repo_root}/tools/bootstrap-kichad-ubuntu.sh"

as_root=()

if (( EUID != 0 )); then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "sudo is required when the script is not run as root." >&2
        exit 1
    fi

    as_root=( sudo )
fi

"${as_root[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    --no-install-recommends \
    binutils \
    ca-certificates \
    file \
    libgl1-mesa-dri \
    libsqliteodbc \
    odbc-mariadb \
    odbc-postgresql \
    patchelf \
    wget \
    zstd

echo "KiChad AppImage prerequisites are installed."
