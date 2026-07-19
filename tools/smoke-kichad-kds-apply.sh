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

require_native_version() {
    local path="$1"
    local version="$2"

    if ! grep -Fq -- "(version ${version})" "$path"; then
        echo "Refusing to open stale KiCad fixture format: ${path}" >&2
        echo "Expected native KiCad 10.0.4 format version ${version}." >&2
        exit 1
    fi
}

# Check before editor launch so a stale fixture can never trigger a migration dialog and then
# masquerade as an IPC/compiler failure.
require_native_version "${fixture_dir}/live_apply.kicad_pcb" 20260206
require_native_version "${fixture_dir}/live_apply.kicad_sch" 20260306
require_native_version "${fixture_dir}/Device.kicad_sym" 20251024
require_native_version \
    "${fixture_dir}/Resistor_SMD.pretty/R_0603_1608Metric.kicad_mod" 20260206

work_dir="$(mktemp -d --tmpdir kichad-kds-live-XXXXXX)"
project_dir="${work_dir}/project"
config_dir="${work_dir}/config"
editor_log="${work_dir}/pcbnew.log"
mkdir -p -- "$project_dir" "$config_dir"
cp -- "${fixture_dir}/live_apply.kicad_pro" "$project_dir/"
cp -- "${fixture_dir}/live_apply.kicad_pcb" "$project_dir/"
cp -- "${fixture_dir}/live_apply.kicad_kds" "$project_dir/"
cp -- "${fixture_dir}/live_apply.kicad_sch" "$project_dir/"
cp -- "${fixture_dir}/Device.kicad_sym" "$project_dir/"
cp -R -- "${fixture_dir}/Resistor_SMD.pretty" "$project_dir/"
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

    if KICAD_CONFIG_HOME="$config_dir" KICAD_RUN_FROM_BUILD_DIR=1 "$test_binary" \
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
        if ! perl -0e '
            my $text = <>;
            exit 1 unless $text =~ /\(comp\s+\(ref "R1"\)/s;
            exit 1 unless $text =~ /\(comp\s+\(ref "R2"\)/s;
            my ($net) = $text =~
                    /(\(net\s+\(code "[^"]+"\)\s+\(name "Net1"\).*?)(?=\n[ \t]*\(net|\n[ \t]*\)\s*\z)/s;
            exit 1 unless defined $net;
            exit 1 unless $net =~ /\(node\s+\(ref "R1"\)\s+\(pin "1"\)/s;
            exit 1 unless $net =~ /\(node\s+\(ref "R2"\)\s+\(pin "1"\)/s;
            my @nodes = $net =~ /\(node\b/g;
            exit 1 unless @nodes == 2;
            my ($ground) = $text =~
                    /(\(net\s+\(code "[^"]+"\)\s+\(name "GND"\).*?)(?=\n[ \t]*\(net|\n[ \t]*\)\s*\z)/s;
            exit 1 unless defined $ground;
            exit 1 unless $ground =~ /\(node\s+\(ref "R1"\)\s+\(pin "2"\)/s;
            my @ground_nodes = $ground =~ /\(node\b/g;
            exit 1 unless @ground_nodes == 1;
        ' "${project_dir}/live_apply.net"; then
            echo "Native netlist is missing exact KDS signal or power connectivity." >&2
            sed -n '1,260p' "${project_dir}/live_apply.net" >&2
            exit 1
        fi
        grep -Fq '(title "KiChad AI-native release")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(company "KiChad QA")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(comment 1 "Controlled by the KDS sidecar")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(comment 9 "Complete indexed title-block proof")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(uuid "11111111-2222-4333-8444-555555555555")' \
            "${project_dir}/live_apply.kicad_sch"
        for expected in '(wire' '(junction' '(bus' '(bus_entry'; do
            if ! grep -Fq "$expected" "${project_dir}/live_apply.kicad_sch"; then
                echo "Applied root schematic is missing native KDS drawing: $expected" >&2
                sed -n '1,320p' "${project_dir}/live_apply.kicad_sch" >&2
                exit 1
            fi
        done
        grep -Fq '(label "Net1"' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(global_label "Net1"' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(netclass_flag ""' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(property "Review Note" "AI-controlled directive"' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(rule_area' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(type hatch)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(color 68 85 102 0.6)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(property "Component Class" "ANALOG"' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(text "AI-readable schematic note\nwith complete typography"' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(at 100 70 15.5)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(justify right top mirror)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(href "https://github.com/10-X-eng/KiChad")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(text_box "AI-readable constraint summary\nwith bounded context"' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(at 125 70 22.5)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(size 30 12)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(margins 0.5 0.75 1 1.25)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(size 1.7 1.3)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(width -0.000001)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(type cross_hatch)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(color 112 128 144 0.8)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(href "https://github.com/10-X-eng/KiChad/issues")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(pts' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(xy 20 90)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(rectangle' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(start 50 82)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(radius 2)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(circle' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(center 90 90)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(arc' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(mid 115 80)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(bezier' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(xy 155 100)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(type reverse_hatch)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(image' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(at 180 90)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(scale 1.25)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(table' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(column_count 2)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(column_widths 18 27)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(row_heights 7 7)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(table_cell "AI pin summary"' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(at 180 170 90)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(size 7 -45)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(span 2 1)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(span 0 0)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(bus_alias "ROOT_SIGNALS"' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(group "AI signal core"' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(group "AI root review bundle"' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(locked yes)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(group "AI power interface"' "${project_dir}/power.kicad_sch"
        grep -Fq '(locked yes)' "${project_dir}/power.kicad_sch"
        grep -Fq '(property "Datasheet" "https://example.com/r1.pdf"' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(property private "Manufacturer Part" "RC0603FR-071KL"' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(at 43 42 12.5)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(show_name yes)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(do_not_autoplace yes)' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(href "https://example.com/value")' \
            "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(lib_id "Device:GND")' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(reference "#PWR01")' "${project_dir}/live_apply.kicad_sch"
        grep -Fq '(property "Description" "Derived GROUND power symbol"' \
            "${project_dir}/live_apply.kicad_sch"
        echo "KiChad live KDS apply smoke test passed."
        exit 0
    fi

    sleep 1
done

echo "The live KDS apply test did not pass within 30 attempts." >&2
sed -n '1,200p' "$editor_log" >&2
exit 1
