#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build/release"
install_dir="${repo_root}/build/install"
launcher_template="${repo_root}/tools/kichad-local-launcher.sh"
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

if command -v git >/dev/null 2>&1 && git -C "$repo_root" rev-parse --git-dir >/dev/null 2>&1; then
    base_commit="$(git -C "$repo_root" rev-parse "${base_version}^{}" 2>/dev/null || true)"

    if [[ -z "$base_commit" ]]; then
        echo "Required upstream stable tag ${base_version} is not present." >&2
        echo "Run: git fetch upstream --tags" >&2
        exit 1
    fi

    if ! git -C "$repo_root" merge-base --is-ancestor "$base_commit" HEAD; then
        echo "HEAD is not based on stable KiCad ${base_version} (${base_commit})." >&2
        exit 1
    fi
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

cd "$repo_root"

cmake --preset kichad-release

build_command=( cmake --build --preset kichad-release --parallel "$jobs" )

if (( $# > 0 )); then
    build_command+=( --target "$@" )
fi

"${build_command[@]}"

if (( $# == 0 )); then
    cmake --install "$build_dir"

    for target_name in kicad kicad-cli; do
        installed_binary="$install_dir/bin/$target_name"

        if [[ ! -x "$installed_binary" ]]; then
            echo "Installed KiChad target is missing: ${installed_binary}" >&2
            exit 1
        fi
    done

    install -m 0755 "$launcher_template" "$install_dir/bin/kichad"
    install -m 0755 "$launcher_template" "$install_dir/bin/kichad-cli"

    built_version="$("$install_dir/bin/kichad-cli" version)"

    if [[ "$built_version" != "$base_version"* ]]; then
        echo "Expected a ${base_version} build, got: ${built_version}" >&2
        exit 1
    fi

    printf 'KiChad stable build verified: %s\n' "$built_version"
else
    echo "Target build complete; install skipped because explicit targets were requested."
fi
