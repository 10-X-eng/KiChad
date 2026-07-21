#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
reference_fixture="${repo_root}/qa/data/kichad/reference_stepper_controller"
config_fixture="${repo_root}/qa/data/kichad/kds_live_apply/config"
pcbnew_binary="${repo_root}/build/release/pcbnew/pcbnew"
qa_binary="${repo_root}/build/release/qa/tests/common/qa_common"

if [[ "${1:-}" != "--allow-mutation" || $# -ne 1 ]]; then
    echo "Usage: $0 --allow-mutation" >&2
    echo "Materializes and releases the complete stepper-controller KDS only in a disposable project." >&2
    exit 2
fi

if [[ ! -x "$pcbnew_binary" || ! -x "$qa_binary" ]]; then
    echo "Build pcbnew and qa_common first with ./tools/build-kichad.sh." >&2
    exit 1
fi

work_dir="$(mktemp -d --tmpdir kichad-stepper-reference-XXXXXX)"
project_dir="${work_dir}/project"
config_dir="${work_dir}/config"
editor_log="${work_dir}/pcbnew.log"
mkdir -p -- "$project_dir" "$config_dir"

for extension in kicad_pcb kicad_sch kicad_pro; do
    cp -- "${reference_fixture}/blank.${extension}" \
        "${project_dir}/reference_stepper_controller.${extension}"
done

cp -- "${reference_fixture}/reference_stepper_controller.kicad_kds" "$project_dir/"
cp -R -- "${config_fixture}/." "$config_dir/"

editor_pid=""
ipc_marker="${work_dir}/pcbnew-ipc-start"
cleanup() {
    if [[ -n "$editor_pid" ]] && kill -0 "$editor_pid" 2>/dev/null; then
        kill "$editor_pid" 2>/dev/null || true
        wait "$editor_pid" 2>/dev/null || true
    fi

    if [[ "${KICHAD_KEEP_REFERENCE_OUTPUT:-0}" == "1" ]]; then
        echo "Retained disposable stepper release at ${work_dir}"
    else
        rm -rf -- "$work_dir"
    fi
}
trap cleanup EXIT INT TERM

touch "$ipc_marker"

KICAD_CONFIG_HOME="$config_dir" \
KICAD_RUN_FROM_BUILD_DIR=1 \
KICAD_SOFTWARE_RENDERING=1 \
    "$pcbnew_binary" "${project_dir}/reference_stepper_controller.kicad_pcb" \
        >"$editor_log" 2>&1 &
editor_pid=$!

ipc_socket=""
ipc_socket_dir="${TMPDIR:-/tmp}/kicad"

for _ in {1..80}; do
    if ! kill -0 "$editor_pid" 2>/dev/null; then
        echo "The disposable PCB Editor exited before the reference test connected." >&2
        sed -n '1,200p' "$editor_log" >&2
        exit 1
    fi

    ipc_socket="$(find "$ipc_socket_dir" -maxdepth 1 -type s -name 'api*.sock' \
        -newer "$ipc_marker" -print -quit 2>/dev/null || true)"

    if [[ -n "$ipc_socket" ]]; then
        break
    fi

    sleep 0.25
done

if [[ -z "$ipc_socket" ]]; then
    echo "PCB Editor did not publish an IPC socket within 20 seconds." >&2
    sed -n '1,200p' "$editor_log" >&2
    exit 1
fi

# The socket file is created immediately before the protobuf service begins
# accepting requests. Give that final initialization a short bounded window.
sleep 1

export KICHAD_QA_STEPPER_REFERENCE_PROJECT="$project_dir"

if ! KICAD_CONFIG_HOME="$config_dir" KICAD_RUN_FROM_BUILD_DIR=1 \
        "$qa_binary" \
        --run_test=CodexToolRegistry/MaterializesAndReleasesStepperReferenceWhenRequested \
        --log_level=message; then
    echo "The production stepper reference failed; disposable PCB Editor output follows." >&2
    sed -n '1,240p' "$editor_log" >&2
    exit 1
fi

test -s "${project_dir}/fabrication/manifest.json"
test -s "${project_dir}/fabrication/design/reference_stepper_controller.kicad_kds"
test -s "${project_dir}/fabrication/production/firmware/controller.hex"
echo "Production stepper KDS materialized with clean ERC/DRC and a validated running-board release."
