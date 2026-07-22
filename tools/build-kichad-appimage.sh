#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${repo_root}/build/appimage"
cmake_build="${build_root}/release"
stage_root="${build_root}/stage"
install_root="${stage_root}/usr"
library_root="${build_root}/libraries"
library_build_root="${build_root}/library-release"
artifact_root="${build_root}/artifacts"
base_version_file="${repo_root}/.kichad-base-version"

if [[ ! -r "$base_version_file" ]]; then
    echo "Missing KiChad stable-base marker: ${base_version_file}" >&2
    exit 1
fi

base_version="$(<"$base_version_file")"
base_commit="$(git -C "$repo_root" rev-parse "${base_version}^{}" 2>/dev/null || true)"

if [[ -z "$base_commit" ]]; then
    echo "Required upstream stable tag ${base_version} is not present." >&2
    exit 1
fi

if ! git -C "$repo_root" merge-base --is-ancestor "$base_commit" HEAD; then
    echo "HEAD is not based on stable KiCad ${base_version} (${base_commit})." >&2
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

mkdir -p "$build_root" "$artifact_root"
exec > >(tee "${artifact_root}/build.log") 2>&1

export CCACHE_BASEDIR="${repo_root}"
export CCACHE_DIR="${CCACHE_DIR:-${build_root}/ccache}"
ccache --max-size="${KICHAD_CCACHE_SIZE:-4G}"
ccache --zero-stats

library_projects="kicad-symbols kicad-footprints kicad-packages3D kicad-templates"

env KICHAD_LIBRARY_ROOT="$library_root" \
    KICHAD_LIBRARY_PROJECTS="$library_projects" \
    "${repo_root}/tools/fetch-kicad-libraries.sh"

cmake -S "$repo_root" -B "$cmake_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DKICAD_BUILD_I18N=ON \
    -DKICAD_BUILD_QA_TESTS=OFF \
    -DKICAD_CONFIG_DIR=kichad \
    -DKICAD_IPC_API=ON \
    -DKICAD_SCRIPTING_WXPYTHON=ON \
    -DKICAD_VERSION_EXTRA=KiChad

cmake --build "$cmake_build" --parallel "$jobs"

case "$stage_root" in
    "${repo_root}"/build/appimage/stage)
        rm -rf -- "$stage_root"
        ;;
    *)
        echo "Refusing unsafe AppImage stage path: ${stage_root}" >&2
        exit 2
        ;;
esac

DESTDIR="$stage_root" cmake --install "$cmake_build"

env KICHAD_LIBRARY_ROOT="$library_root" \
    KICHAD_INSTALL_ROOT="$install_root" \
    KICHAD_LIBRARY_BUILD_ROOT="$library_build_root" \
    KICHAD_LIBRARY_PROJECTS="$library_projects" \
    KICHAD_BUILD_JOBS="$jobs" \
    "${repo_root}/tools/install-kichad-libraries.sh"

"${repo_root}/tools/package-kichad-appimage.sh" "$install_root" "$artifact_root"

ccache --show-stats
