#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="${repo_root}/build/release/qa/tests/common/qa_common"

usage() {
    echo "Usage: $0 --allow-mutation PROJECT_DIRECTORY BOARD_FILE [SOCKET_DIRECTORY]" >&2
    echo "Run this only against a disposable copy already open in KiChad PCB Editor." >&2
}

if (( $# < 3 || $# > 4 )) || [[ "$1" != "--allow-mutation" ]]; then
    usage
    exit 2
fi

project_path="$(realpath -- "$2")"
board_path="$(realpath -- "$3")"
socket_directory="${4:-}"

if [[ ! -d "$project_path" || ! -f "$board_path" ]]; then
    usage
    exit 2
fi

case "$board_path" in
    "$project_path"/*.kicad_pcb)
        ;;
    *)
        echo "BOARD_FILE must be a .kicad_pcb inside PROJECT_DIRECTORY." >&2
        exit 2
        ;;
esac

if [[ ! -x "$test_binary" ]]; then
    echo "The KiChad QA binary is missing. Run: ./tools/build-kichad.sh qa_common" >&2
    exit 1
fi

export KICHAD_QA_LIVE_PROJECT="$project_path"
export KICHAD_QA_LIVE_BOARD="$board_path"

if [[ -n "$socket_directory" ]]; then
    export KICHAD_QA_LIVE_SOCKET_DIR="$(realpath -- "$socket_directory")"
fi

exec "$test_binary" \
    --run_test=KiChadIpcClient/LiveCreateUpdateDeleteWhenRequested \
    --log_level=message
