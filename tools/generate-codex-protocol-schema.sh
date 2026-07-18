#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
output_root="${KICHAD_CODEX_SCHEMA_DIR:-${repo_root}/build/codex-app-server-schema}"
codex_executable="${KICHAD_CODEX_EXECUTABLE:-codex}"

"${repo_root}/tools/check-codex-app-server.sh"
mkdir -p -- "${output_root}/json" "${output_root}/ts"

"$codex_executable" app-server generate-json-schema --experimental --out "${output_root}/json"
"$codex_executable" app-server generate-ts --experimental --out "${output_root}/ts"

printf 'Codex app-server protocol schema generated in %s\n' "$output_root"
