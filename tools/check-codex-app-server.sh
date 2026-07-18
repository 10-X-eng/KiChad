#!/usr/bin/env bash

set -euo pipefail

codex_executable="${KICHAD_CODEX_EXECUTABLE:-codex}"

if [[ "$codex_executable" == */* ]]; then
    if [[ ! -x "$codex_executable" ]]; then
        echo "KICHAD_CODEX_EXECUTABLE is not executable: ${codex_executable}" >&2
        exit 1
    fi
elif ! command -v "$codex_executable" >/dev/null 2>&1; then
    echo "Codex was not found on PATH." >&2
    echo "Install the Codex CLI or set KICHAD_CODEX_EXECUTABLE to its absolute path." >&2
    exit 1
fi

version="$("$codex_executable" --version)"
"$codex_executable" app-server --help >/dev/null

printf 'Codex app-server verified: %s\n' "$version"
