#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
library_root="${KICHAD_LIBRARY_ROOT:-${repo_root}/build/libraries}"
install_root="${KICHAD_INSTALL_ROOT:-${repo_root}/build/install}"
library_build_root="${KICHAD_LIBRARY_BUILD_ROOT:-${repo_root}/build/library-release}"
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

if [[ -n "${KICHAD_BUILD_JOBS:-}" ]]; then
    jobs="$KICHAD_BUILD_JOBS"
elif command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
else
    jobs=1
fi

if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "KICHAD_BUILD_JOBS must be a positive integer; got: ${jobs}" >&2
    exit 2
fi

library_projects=(
    kicad-symbols
    kicad-footprints
    kicad-packages3D
    kicad-templates
)

for project in "${library_projects[@]}"; do
    source_dir="${library_root}/${project}"
    build_dir="${library_build_root}/${project}"

    if [[ ! -f "${source_dir}/CMakeLists.txt" || ! -d "${source_dir}/.git" ]]; then
        echo "Missing official ${project} source in ${source_dir}." >&2
        echo "Run: ./tools/fetch-kicad-libraries.sh" >&2
        exit 1
    fi

    tagged_commit="$(git -C "$source_dir" rev-parse \
        "refs/tags/${base_version}^{commit}" 2>/dev/null || true)"

    if [[ -z "$tagged_commit" || "$(git -C "$source_dir" rev-parse HEAD)" != "$tagged_commit" ]]; then
        echo "${project} is not pinned to KiChad ${base_version}." >&2
        echo "Run: ./tools/fetch-kicad-libraries.sh" >&2
        exit 1
    fi

    cmake -S "$source_dir" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_MESSAGE=NEVER \
        -DCMAKE_INSTALL_PREFIX="$install_root"
    echo "Installing ${project} ${base_version}..."
    cmake --build "$build_dir" --parallel "$jobs"
    cmake --install "$build_dir" --component resources
done

echo "KiCad ${base_version} standard libraries installed in ${install_root}/share/kicad"
