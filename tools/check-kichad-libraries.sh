#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${KICHAD_INSTALL_ROOT:-${repo_root}/build/install}"
kicad_cli="${install_root}/bin/kichad-cli"

if [[ ! -x "$kicad_cli" ]]; then
    echo "KiChad CLI is missing: ${kicad_cli}" >&2
    exit 1
fi

version="$($kicad_cli version)"
major="${version%%.*}"

if [[ ! "$major" =~ ^[0-9]+$ ]]; then
    echo "Could not determine the KiCad major version from: ${version}" >&2
    exit 1
fi

data_root="${install_root}/share/kicad"
symbol_root="${data_root}/symbols"
footprint_root="${data_root}/footprints"
template_root="${data_root}/template"
symbol_table="${template_root}/sym-lib-table"
footprint_table="${template_root}/fp-lib-table"

required_paths=(
    "$symbol_table"
    "$footprint_table"
    "${symbol_root}/Device.kicad_sym"
    "${footprint_root}/Module.pretty/Arduino_Nano.kicad_mod"
    "${footprint_root}/Module.pretty/Pololu_Breakout-16_15.2x20.3mm.kicad_mod"
)

for path in "${required_paths[@]}"; do
    if [[ ! -r "$path" ]]; then
        echo "Required KiCad standard-library resource is missing: ${path}" >&2
        exit 1
    fi
done

if ! grep -Fq "\${KICAD${major}_SYMBOL_DIR}/Device.kicad_sym" "$symbol_table"; then
    echo "Stock symbol table does not target KICAD${major}_SYMBOL_DIR." >&2
    exit 1
fi

if ! grep -Fq "\${KICAD${major}_FOOTPRINT_DIR}/Module.pretty" "$footprint_table"; then
    echo "Stock footprint table does not target KICAD${major}_FOOTPRINT_DIR." >&2
    exit 1
fi

smoke_dir="$(mktemp -d -t kichad-library-smoke.XXXXXXXX)"

cleanup() {
    if [[ -n "${smoke_dir:-}" && -d "$smoke_dir" ]]; then
        rm -rf -- "$smoke_dir"
    fi
}

trap cleanup EXIT

mkdir -p "${smoke_dir}/footprint" "${smoke_dir}/symbol" \
    "${smoke_dir}/input.pretty" "${smoke_dir}/native-config"

install -m 0644 \
    "${footprint_root}/Module.pretty/Pololu_Breakout-16_15.2x20.3mm.kicad_mod" \
    "${smoke_dir}/input.pretty/Pololu_Breakout-16_15.2x20.3mm.kicad_mod"

env -u KICAD_RUN_FROM_BUILD_DIR \
    KICAD_CONFIG_HOME="${smoke_dir}/native-config" \
    KICAD_CONFIG_HOME_IS_QA=1 \
    "$kicad_cli" fp upgrade --force \
    --output "${smoke_dir}/validated.pretty" \
    "${smoke_dir}/input.pretty" >/dev/null

"$kicad_cli" fp export svg \
    --output "${smoke_dir}/footprint" \
    --footprint Pololu_Breakout-16_15.2x20.3mm \
    "${footprint_root}/Module.pretty" >/dev/null

"$kicad_cli" sym export svg \
    --output "${smoke_dir}/symbol" \
    --symbol R \
    "${symbol_root}/Device.kicad_sym" >/dev/null

if ! find "${smoke_dir}/footprint" -type f -name '*.svg' -print -quit | grep -q .; then
    echo "KiCad did not render the standard Pololu footprint." >&2
    exit 1
fi

if [[ ! -r "${smoke_dir}/validated.pretty/Pololu_Breakout-16_15.2x20.3mm.kicad_mod" ]]; then
    echo "The installed KiCad CLI could not natively validate a managed footprint library." >&2
    exit 1
fi

if ! find "${smoke_dir}/symbol" -type f -name '*.svg' -print -quit | grep -q .; then
    echo "KiCad did not render the standard Device:R symbol." >&2
    exit 1
fi

printf 'KiCad %s standard libraries verified: Device:R, Module:Arduino_Nano, Module:Pololu_Breakout-16_15.2x20.3mm\n' \
    "$version"
