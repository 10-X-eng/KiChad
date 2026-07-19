#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
fixture_dir="${repo_root}/qa/data/kichad/kds_live_apply"
pcbnew_binary="${repo_root}/build/release/pcbnew/pcbnew"
kicad_cli_binary="${repo_root}/build/release/kicad/kicad-cli"
test_binary="${repo_root}/build/release/qa/tests/common/qa_common"

if [[ "${1:-}" != "--allow-mutation" || $# -ne 1 ]]; then
    echo "Usage: $0 --allow-mutation" >&2
    echo "Creates and mutates only a disposable copy of the committed live-apply fixture." >&2
    exit 2
fi

if [[ ! -x "$pcbnew_binary" || ! -x "$kicad_cli_binary" || ! -x "$test_binary" ]]; then
    echo "Build pcbnew and qa_common first with ./tools/build-kichad.sh." >&2
    exit 1
fi

work_dir="$(mktemp -d --tmpdir kichad-kds-live-XXXXXX)"
project_dir="${work_dir}/project"
config_dir="${work_dir}/config"
editor_log="${work_dir}/pcbnew.log"
mkdir -p -- "$project_dir" "$config_dir"
cp -- "${fixture_dir}/live_apply.kicad_pro" "$project_dir/"
cp -- "${fixture_dir}/live_apply.kicad_pcb" "$project_dir/"
cp -- "${fixture_dir}/live_apply.kicad_kds" "$project_dir/"
cp -- "${fixture_dir}/live_apply.kicad_sch" "$project_dir/"
cp -R -- "${fixture_dir}/config/." "$config_dir/"

editor_pid=""
cleanup() {
    if [[ -n "$editor_pid" ]] && kill -0 "$editor_pid" 2>/dev/null; then
        kill "$editor_pid" 2>/dev/null || true
        wait "$editor_pid" 2>/dev/null || true
    fi

    rm -rf -- "$work_dir"
}
trap cleanup EXIT INT TERM

KICAD_CONFIG_HOME="$config_dir" \
KICAD_RUN_FROM_BUILD_DIR=1 \
KICAD_SOFTWARE_RENDERING=1 \
    "$pcbnew_binary" "${project_dir}/live_apply.kicad_pcb" >"$editor_log" 2>&1 &
editor_pid=$!

# Let the disposable editor finish constructing its canvas and register the PCB API handler before
# issuing requests.  The API server itself starts earlier so editor plugins can discover its socket.
sleep 3

export KICHAD_QA_LIVE_PROJECT="$project_dir"
export KICHAD_QA_LIVE_BOARD="${project_dir}/live_apply.kicad_pcb"
export KICHAD_QA_LIVE_KDS="${project_dir}/live_apply.kicad_kds"

for attempt in $(seq 1 30); do
    if ! kill -0 "$editor_pid" 2>/dev/null; then
        echo "The disposable PCB Editor exited before the test connected." >&2
        sed -n '1,160p' "$editor_log" >&2
        exit 1
    fi

    if "$test_binary" \
        --run_test=CodexToolRegistry/AppliesReusableDesignAgainstLivePcbEditorWhenRequested \
        --log_level=message; then
        if ! KICAD_RUN_FROM_BUILD_DIR=1 "$kicad_cli_binary" sch export netlist \
                --output "${project_dir}/live_apply.net" \
                "${project_dir}/live_apply.kicad_sch"; then
            echo "Native validation rejected the applied root schematic:" >&2
            sed -n '1,240p' "${project_dir}/live_apply.kicad_sch" >&2
            echo "Native validation child schematic:" >&2
            sed -n '1,240p' "${project_dir}/power.kicad_sch" >&2
            KICAD_RUN_FROM_BUILD_DIR=1 "$kicad_cli_binary" sch export netlist \
                --output "${project_dir}/power.net" \
                "${project_dir}/power.kicad_sch" >&2 || true
            exit 1
        fi
        test -s "${project_dir}/live_apply.net"
        grep -Fq '(company "KiChad lossless fixture")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(uuid "11111111-2222-4333-8444-555555555555")' \
            "${project_dir}/live_apply.kicad_sch"
        echo "KiChad live KDS apply smoke test passed."
        exit 0
    fi

    sleep 1
done

echo "The live KDS apply test did not pass within 30 attempts." >&2
sed -n '1,200p' "$editor_log" >&2
exit 1
