#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
library_root="${KICHAD_LIBRARY_ROOT:-${repo_root}/build/libraries}"
base_version_file="${repo_root}/.kichad-base-version"

if [[ ! -r "$base_version_file" ]]; then
    echo "Missing KiChad stable-base marker: ${base_version_file}" >&2
    exit 1
fi

base_version="$(<"$base_version_file")"

if [[ ! "$base_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Invalid KiChad stable-base version: ${base_version}" >&2
    exit 1
fi

mkdir -p "$library_root"

sync_library() {
    local name="$1"
    local url="$2"
    local destination="${library_root}/${name}"
    local tagged_commit

    if [[ -d "${destination}/.git" ]]; then
        if [[ -n "$(git -C "$destination" status --porcelain)" ]]; then
            echo "Refusing to replace local changes in ${destination}." >&2
            return 1
        fi

        if ! git -C "$destination" rev-parse --verify --quiet \
                "refs/tags/${base_version}^{commit}" >/dev/null; then
            echo "Fetching ${name} ${base_version}..."
            git -C "$destination" fetch --depth 1 origin \
                "refs/tags/${base_version}:refs/tags/${base_version}"
        fi
    elif [[ -e "$destination" ]]; then
        echo "Refusing to replace non-Git path: ${destination}" >&2
        return 1
    else
        echo "Fetching ${name} ${base_version}..."
        git clone --depth 1 --branch "$base_version" --single-branch "$url" "$destination"
    fi

    tagged_commit="$(git -C "$destination" rev-parse "refs/tags/${base_version}^{commit}")"

    if [[ "$(git -C "$destination" rev-parse HEAD)" != "$tagged_commit" ]]; then
        git -C "$destination" checkout --detach "$tagged_commit"
    fi

    printf '%s: %s (%s)\n' "$name" "$base_version" "$tagged_commit"
}

sync_library kicad-symbols https://gitlab.com/kicad/libraries/kicad-symbols.git
sync_library kicad-footprints https://gitlab.com/kicad/libraries/kicad-footprints.git
sync_library kicad-packages3D https://gitlab.com/kicad/libraries/kicad-packages3D.git
sync_library kicad-templates https://gitlab.com/kicad/libraries/kicad-templates.git

echo "KiCad ${base_version} libraries are available in ${library_root}"
