#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build/release"
install_dir="${repo_root}/build/install"
launcher_template="${repo_root}/tools/kichad-local-launcher.sh"

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

    for launcher_name in kichad kichad-cli; do
        rm -f -- "$install_dir/bin/$launcher_name"
        install -m 0755 "$launcher_template" "$install_dir/bin/$launcher_name"
    done

    "$install_dir/bin/kichad-cli" version
else
    echo "Target build complete; install skipped because explicit targets were requested."
fi
