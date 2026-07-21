#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
base_fixture="${repo_root}/qa/data/kichad/fabrication_clean"
component_fixture="${repo_root}/qa/data/kichad/fabrication_component"
library_fixture="${repo_root}/qa/data/kichad/kds_live_apply"
pcbnew_binary="${repo_root}/build/release/pcbnew/pcbnew"
kicad_cli_binary="${repo_root}/build/release/kicad/kicad-cli"
qa_binary="${repo_root}/build/release/qa/tests/common/qa_common"

if [[ "${1:-}" != "--allow-mutation" || $# -ne 1 ]]; then
    echo "Usage: $0 --allow-mutation" >&2
    echo "Applies KDS and fabricates a sourced component only in a disposable project." >&2
    exit 2
fi

if [[ ! -x "$pcbnew_binary" || ! -x "$kicad_cli_binary" || ! -x "$qa_binary" ]]; then
    echo "Build pcbnew, kicad-cli, and qa_common first with ./tools/build-kichad.sh." >&2
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
        echo "Refusing stale native component-fabrication fixture: ${path}" >&2
        echo "Expected exact KiCad 10.0.4 format version ${version}; got ${actual_version:-none}." >&2
        exit 1
    fi
}

require_native_version "${base_fixture}/fabrication_clean.kicad_pcb" 20260206
require_native_version "${base_fixture}/fabrication_clean.kicad_sch" 20260306
require_native_version "${component_fixture}/fabrication_component.kicad_pcb" 20260206
require_native_version "${library_fixture}/Device.kicad_sym" 20251024
require_native_version \
    "${library_fixture}/Resistor_SMD.pretty/R_0603_1608Metric.kicad_mod" 20260206

work_dir="$(mktemp -d --tmpdir kichad-fabrication-component-XXXXXX)"
project_dir="${work_dir}/project"
config_dir="${work_dir}/config"
editor_log="${work_dir}/pcbnew.log"
test_binary="$(mktemp "${repo_root}/build/release/kicad/qa-fabrication-component.XXXXXX")"
editor_pid=""

cleanup() {
    if [[ -n "$editor_pid" ]] && kill -0 "$editor_pid" 2>/dev/null; then
        kill "$editor_pid" 2>/dev/null || true
        wait "$editor_pid" 2>/dev/null || true
    fi

    rm -f -- "$test_binary"
    rm -rf -- "$work_dir"
}
trap cleanup EXIT INT TERM

mkdir -p -- "$project_dir" "$config_dir"
cp -- "${component_fixture}/fabrication_component.kicad_pcb" \
    "${project_dir}/fabrication_component.kicad_pcb"
cp -- "${base_fixture}/fabrication_clean.kicad_sch" \
    "${project_dir}/fabrication_component.kicad_sch"
cp -- "${component_fixture}/fabrication_component.kicad_pro" "$project_dir/"
cp -- "${base_fixture}/fabrication_clean.kicad_prl" \
    "${project_dir}/fabrication_component.kicad_prl"
cp -- "${component_fixture}/fabrication_component.kicad_kds" "$project_dir/"
cp -- "${library_fixture}/Device.kicad_sym" "$project_dir/"
cp -R -- "${library_fixture}/Resistor_SMD.pretty" "$project_dir/"
cp -R -- "${library_fixture}/config/." "$config_dir/"
cp -- "$qa_binary" "$test_binary"
chmod 0700 "$test_binary"

# Keep the committed synthetic QA record valid and make its freshness check deterministic any day.
sed -i -E \
    "s/\(verified_on [0-9]{4}-[0-9]{2}-[0-9]{2}\)/(verified_on $(date -I))/" \
    "${project_dir}/fabrication_component.kicad_kds"
sed -i 's/fabrication_clean/fabrication_component/g' \
    "${project_dir}/fabrication_component.kicad_prl"

KICAD_CONFIG_HOME="$config_dir" \
KICAD_RUN_FROM_BUILD_DIR=1 \
KICAD_SOFTWARE_RENDERING=1 \
    "$pcbnew_binary" "${project_dir}/fabrication_component.kicad_pcb" \
        >"$editor_log" 2>&1 &
editor_pid=$!

sleep 3

export KICHAD_QA_FABRICATION_COMPONENT_PROJECT="$project_dir"

if ! kill -0 "$editor_pid" 2>/dev/null; then
    echo "The disposable PCB Editor exited before the fabrication test connected." >&2
    sed -n '1,200p' "$editor_log" >&2
    exit 1
fi

KICAD_CONFIG_HOME="$config_dir" KICAD_RUN_FROM_BUILD_DIR=1 "$test_binary" \
    --run_test=CodexToolFabricate/AppliesSavesAndFabricatesSourcedComponentWhenExplicitlyRequested \
    --log_level=message

echo "KiChad KDS component fabrication smoke test passed."
