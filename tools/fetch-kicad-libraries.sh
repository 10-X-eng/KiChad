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

default_projects=(
    kicad-symbols
    kicad-footprints
    kicad-packages3D
    kicad-templates
)

if [[ -n "${KICHAD_LIBRARY_PROJECTS:-}" ]]; then
    read -r -a library_projects <<<"$KICHAD_LIBRARY_PROJECTS"
else
    library_projects=( "${default_projects[@]}" )
fi

if (( ${#library_projects[@]} == 0 )); then
    echo "KICHAD_LIBRARY_PROJECTS must select at least one official library project." >&2
    exit 2
fi

declare -A seen_projects=()

for project in "${library_projects[@]}"; do
    case "$project" in
        kicad-symbols|kicad-footprints|kicad-packages3D|kicad-templates)
            ;;
        *)
            echo "Unsupported KiCad library project: ${project}" >&2
            exit 2
            ;;
    esac

    if [[ -n "${seen_projects[$project]:-}" ]]; then
        echo "Duplicate KiCad library project: ${project}" >&2
        exit 2
    fi

    seen_projects[$project]=1
done

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

for project in "${library_projects[@]}"; do
    sync_library "$project" "https://gitlab.com/kicad/libraries/${project}.git"
done

echo "KiCad ${base_version} libraries are available in ${library_root}"
