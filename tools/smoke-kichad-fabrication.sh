#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
fixture_dir="${repo_root}/qa/data/kichad/fabrication_clean"
qa_binary="${repo_root}/build/release/qa/tests/common/qa_common"
cli_binary="${repo_root}/build/release/kicad/kicad-cli"

if [[ "${1:-}" != "--allow-mutation" || $# -ne 1 ]]; then
    echo "Usage: $0 --allow-mutation" >&2
    echo "Exports only from a disposable copy of the current-format fabrication fixture." >&2
    exit 2
fi

if [[ ! -x "$qa_binary" || ! -x "$cli_binary" ]]; then
    echo "Build qa_common and kicad-cli first with ./tools/build-kichad.sh." >&2
    exit 1
fi

require_native_version() {
    local path="$1"
    local version="$2"
    local version_line
    local actual_version

    version_line="$(grep -m1 -E '^[[:space:]]*\(version [0-9]+\)' "$path" || true)"
    actual_version="${version_line#*'(version '}"
    actual_version="${actual_version%%')'*}"

    if [[ "$actual_version" != "$version" ]]; then
        echo "Refusing stale native fabrication fixture: ${path}" >&2
        echo "Expected exact KiCad 10.0.4 format version ${version}; got ${actual_version:-none}." >&2
        exit 1
    fi
}

require_native_version "${fixture_dir}/fabrication_clean.kicad_pcb" 20260206
require_native_version "${fixture_dir}/fabrication_clean.kicad_sch" 20260306

work_dir="$(mktemp -d --tmpdir kichad-fabrication-live-XXXXXX)"
project_dir="${work_dir}/project"
config_dir="${work_dir}/config"
test_binary="$(mktemp "${repo_root}/build/release/kicad/qa-fabrication-smoke.XXXXXX")"

cleanup() {
    rm -f -- "$test_binary"
    rm -rf -- "$work_dir"
}
trap cleanup EXIT INT TERM

mkdir -p -- "$project_dir" "$config_dir"
cp -R -- "${fixture_dir}/." "$project_dir/"
cp -- "$qa_binary" "$test_binary"
chmod 0700 "$test_binary"

KICHAD_QA_FABRICATION_PROJECT="$project_dir" \
KICAD_CONFIG_HOME="$config_dir" \
KICAD_RUN_FROM_BUILD_DIR=1 \
    "$test_binary" \
        --run_test=CodexToolFabricate/ExportsWithSiblingNativeKiCadCliWhenExplicitlyRequested \
        --log_level=message

echo "KiChad native fabrication export smoke test passed."
