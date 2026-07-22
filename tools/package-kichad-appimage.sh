#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
base_version_file="${repo_root}/.kichad-base-version"
install_root="${1:-${repo_root}/build/install}"
output_dir="${2:-${repo_root}/build/appimage/artifacts}"
work_root="${repo_root}/build/appimage/package"
appdir="${work_root}/AppDir"
tool_dir="${work_root}/tools"
packaging_patch="${repo_root}/tools/appimage/kicad-appimage-ubuntu-24.04.diff"
packaging_commit="f57d56e4c20fc322241fb53d8e247d68f50051d4"
packaging_url="https://gitlab.com/kicad/packaging/kicad-appimage.git"

if [[ ! -r "$base_version_file" ]]; then
    echo "Missing KiChad stable-base marker: ${base_version_file}" >&2
    exit 1
fi

base_version="$(<"$base_version_file")"

if [[ ! "$base_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Invalid KiChad stable-base version: ${base_version}" >&2
    exit 1
fi

install_root="$(realpath -e -- "$install_root")"
output_dir="$(realpath -m -- "$output_dir")"

for required in bin/kicad bin/kicad-cli bin/_eeschema.kiface bin/_pcbnew.kiface \
        share/kicad/schemas/api.v1.schema.json share/kicad/symbols/Device.kicad_sym \
        share/kicad/footprints/Module.pretty/Arduino_Nano.kicad_mod \
        share/kicad/3dmodels/Resistor_SMD.3dshapes/R_0603_1608Metric.step \
        share/kicad/template/sym-lib-table share/kicad/template/fp-lib-table; do
    if [[ ! -r "${install_root}/${required}" ]]; then
        echo "Installed KiChad tree is incomplete: ${install_root}/${required}" >&2
        exit 1
    fi
done

case "$appdir" in
    "${repo_root}"/build/appimage/package/AppDir)
        ;;
    *)
        echo "Refusing unsafe AppDir path: ${appdir}" >&2
        exit 2
        ;;
esac

mkdir -p "$work_root" "$tool_dir" "$output_dir"
rm -rf -- "$appdir"
mkdir -p "${appdir}/usr"

# The stage and AppDir live on the same build filesystem. Hard-link the immutable install tree so
# the full 3D library does not consume another several gigabytes while packaging. Files that are
# intentionally replaced below are unlinked first; the installed tree is never modified.
cp --archive --link "${install_root}/." "${appdir}/usr/"

# KiChad owns the app-server process.  Bundle the complete pinned Codex standalone package so a
# clean host does not need npm, Node, a system Codex install, or helper binaries on PATH.
codex_root="${appdir}/usr/lib/kichad/codex"
mkdir -p -- "$codex_root"
"${repo_root}/tools/fetch-codex-standalone.sh" "$codex_root"
ln -s ../lib/kichad/codex/bin/codex "${appdir}/usr/bin/codex"

# The normal developer install exposes relocation wrappers as kicad/kicad-cli and retains the
# native ELFs with an underscore. AppImage's AppRun owns relocation, so stage the native programs.
for program in kicad kicad-cli; do
    if [[ -x "${appdir}/usr/bin/_${program}" ]]; then
        rm -f -- "${appdir}/usr/bin/${program}"
        install -m 0755 "${appdir}/usr/bin/_${program}" "${appdir}/usr/bin/${program}"
        rm -f -- "${appdir}/usr/bin/_${program}"
    fi
done

# Exclude stale development launchers that can exist in an incrementally installed local tree.
rm -f -- "${appdir}/usr/bin/kichad" "${appdir}/usr/bin/kichad-cli" \
    "${appdir}/usr/bin/kichad.real" "${appdir}/usr/bin/kichad-cli.real"

for program in kicad kicad-cli; do
    if [[ "$(file -b "${appdir}/usr/bin/${program}")" != ELF* ]]; then
        echo "AppImage staging requires a native ELF at usr/bin/${program}." >&2
        exit 1
    fi
done

raw_library_path=""

while IFS= read -r directory; do
    raw_library_path="${raw_library_path:+${raw_library_path}:}${directory}"
done < <(find "${appdir}/usr/lib" -type d -print | sort)

smoke_config="$(mktemp -d -t kichad-appdir-smoke.XXXXXXXX)"
packaging_checkout="$(mktemp -d -t kichad-appimage-packaging.XXXXXXXX)"

cleanup()
{
    if [[ -n "${smoke_config:-}" && -d "$smoke_config" ]]; then
        rm -rf -- "$smoke_config"
    fi

    if [[ -n "${packaging_checkout:-}" && -d "$packaging_checkout" ]]; then
        rm -rf -- "$packaging_checkout"
    fi
}

trap cleanup EXIT

env LD_LIBRARY_PATH="$raw_library_path" \
    KICAD_CONFIG_HOME="$smoke_config" \
    KICAD_CONFIG_HOME_IS_QA=1 \
    KICAD10_SYMBOL_DIR="${appdir}/usr/share/kicad/symbols" \
    KICAD10_FOOTPRINT_DIR="${appdir}/usr/share/kicad/footprints" \
    KICAD10_TEMPLATE_DIR="${appdir}/usr/share/kicad/template" \
    KICHAD_INSTALL_ROOT="${appdir}/usr" \
    KICHAD_CLI="${appdir}/usr/bin/kicad-cli" \
    "${repo_root}/tools/check-kichad-libraries.sh"

git -C "$packaging_checkout" init --quiet
git -C "$packaging_checkout" remote add origin "$packaging_url"
git -C "$packaging_checkout" fetch --quiet --depth 1 origin "$packaging_commit"
git -C "$packaging_checkout" checkout --quiet --detach FETCH_HEAD

if [[ "$(git -C "$packaging_checkout" rev-parse HEAD)" != "$packaging_commit" ]]; then
    echo "KiCad AppImage packaging checkout did not resolve the pinned commit." >&2
    exit 1
fi

git -C "$packaging_checkout" apply --check "$packaging_patch"
git -C "$packaging_checkout" apply "$packaging_patch"

source_revision="unknown"

if git -C "$repo_root" rev-parse --git-dir >/dev/null 2>&1; then
    source_revision="$(git -C "$repo_root" rev-parse --short=12 HEAD)"

    if ! git -C "$repo_root" diff --quiet \
            || ! git -C "$repo_root" diff --cached --quiet \
            || [[ -n "$(git -C "$repo_root" ls-files --others --exclude-standard)" ]]; then
        source_revision+="-dirty"
    fi
fi

app_version="${base_version}-KiChad.${source_revision}"

rm -f -- "${output_dir}"/KiChad-*.AppImage "${output_dir}"/KiChad-*.AppImage.sha256

env PATH="${tool_dir}:${PATH}" \
    APPDIR="$appdir" \
    OUTPUT_DIR="$output_dir" \
    APPIMAGE_TOOL_DIR="$tool_dir" \
    APP_NAME="KiChad" \
    APP_VERSION="$app_version" \
    APPIMAGE_VARIANT="full" \
    KICAD_BUILD_MAJVERSION="${base_version%%.*}" \
    SHARUN_VERSION="v0.8.1" \
    URUNTIME_VERSION="v0.5.6" \
    DWARFS_VERSION="v0.14.1" \
    SHARUN_SHA256="18d970f56eca2c527ffd3993b161b6bc340055129db14b394a77cb67d8bbfff9" \
    URUNTIME_SHA256="6416a112fac1e9983b1c0738cd140f17dc1205f515b9bdb36b4607ef98ee2a70" \
    DWARFS_SHA256="f3a117fd6d5b7304944b199af7fdb8086a48c509ea2e9832255d8f9a54c98587" \
    GENERATE_ZSYNC=0 \
    EXTRACT_DBGSYM=0 \
    bash "${packaging_checkout}/scripts/build_appimage.sh"

shopt -s nullglob
images=( "${output_dir}"/KiChad-*.AppImage )
shopt -u nullglob

if (( ${#images[@]} != 1 )); then
    echo "Expected exactly one KiChad AppImage, found ${#images[@]}." >&2
    exit 1
fi

image="${images[0]}"
image_version="$(APPIMAGE_EXTRACT_AND_RUN=1 "$image" kicad-cli version)"
codex_version="$(APPIMAGE_EXTRACT_AND_RUN=1 "$image" codex --version)"

if [[ "$image_version" != "$base_version"* ]]; then
    echo "Expected AppImage version ${base_version}, got: ${image_version}" >&2
    exit 1
fi

if [[ "$codex_version" != "codex-cli 0.144.4" ]]; then
    echo "Expected bundled Codex 0.144.4, got: ${codex_version}" >&2
    exit 1
fi

for bundled in share/kicad/schemas/api.v1.schema.json share/kicad/symbols/Device.kicad_sym \
        share/kicad/footprints/Module.pretty/Arduino_Nano.kicad_mod \
        share/kicad/3dmodels/Resistor_SMD.3dshapes/R_0603_1608Metric.step \
        share/kicad/template/sym-lib-table shared/lib/libngspice.so.0 \
        shared/codex/bin/codex shared/codex/bin/codex-code-mode-host \
        shared/codex/codex-path/rg shared/codex/codex-resources/bwrap \
        shared/codex/licenses/LICENSE shared/codex/licenses/NOTICE; do
    if [[ ! -e "${appdir}/${bundled}" ]]; then
        echo "AppImage is missing required bundled content: ${bundled}" >&2
        exit 1
    fi
done


# Exercise the actual bundled executable over the app-server protocol.  This verifies much more
# than `--version`: KiChad can initialize a session, enumerate models, manage a durable goal, and
# register its native dynamic tool surface without using the developer's host Codex install.
KICHAD_CODEX_EXECUTABLE="${appdir}/shared/codex/bin/codex" \
    "${repo_root}/tools/smoke-codex-app-server-protocol.sh"

(
    cd -- "$(dirname -- "$image")"
    sha256sum "$(basename -- "$image")" > "$(basename -- "$image").sha256"
)

printf 'KiChad AppImage verified: %s\n' "$image"
printf 'Version: %s\n' "$image_version"
printf 'Codex: %s\n' "$codex_version"
printf 'SHA-256: %s\n' "$(cut -d' ' -f1 "${image}.sha256")"
