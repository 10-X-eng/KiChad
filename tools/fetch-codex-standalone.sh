#!/usr/bin/env bash

set -euo pipefail

# Keep KiChad's inference/session runtime reproducible.  This is the complete official Codex
# standalone package rather than only the main ELF: Codex also uses its packaged ripgrep,
# sandbox helper, code-mode host, and shell resources at runtime.
codex_version="0.144.4"
codex_target="x86_64-unknown-linux-musl"
archive_name="codex-package-${codex_target}.tar.zst"
archive_sha256="7bb170b4d8a9ed2a944b3edf12d68d7cb5079fd796d0f7bf04138ee764f8d5fd"
license_sha256="d17f227e4df5da1600391338865ce0f3055211760a36688f816941d58232d8dc"
notice_sha256="9d71575ecfd9a843fc1677b0efb08053c6ba9fd686a0de1a6f5382fd3c220915"
release_base="https://github.com/openai/codex/releases/download/rust-v${codex_version}"
source_base="https://raw.githubusercontent.com/openai/codex/rust-v${codex_version}"
destination="${1:-}"

if [[ -z "$destination" ]]; then
    echo "Usage: $0 DESTINATION" >&2
    exit 2
fi

destination="$(realpath -m -- "$destination")"
mkdir -p -- "$destination"

if find "$destination" -mindepth 1 -print -quit | grep -q .; then
    echo "Codex destination must be empty: ${destination}" >&2
    exit 2
fi

download_root="$(mktemp -d -t kichad-codex-download.XXXXXXXX)"

cleanup()
{
    rm -rf -- "$download_root"
}

trap cleanup EXIT

archive="${download_root}/${archive_name}"
license="${download_root}/LICENSE"
notice="${download_root}/NOTICE"

wget --quiet --tries=3 "${release_base}/${archive_name}" -O "$archive"
wget --quiet --tries=3 "${source_base}/LICENSE" -O "$license"
wget --quiet --tries=3 "${source_base}/NOTICE" -O "$notice"

printf '%s  %s\n' "$archive_sha256" "$archive" | sha256sum --check --status
printf '%s  %s\n' "$license_sha256" "$license" | sha256sum --check --status
printf '%s  %s\n' "$notice_sha256" "$notice" | sha256sum --check --status

# Refuse absolute paths and traversal before unpacking an upstream archive into the package tree.
if tar --zstd -tf "$archive" | awk '
    /^\// || /(^|\/)\.\.($|\/)/ { bad = 1 }
    END { exit bad ? 0 : 1 }
'; then
    echo "Codex archive contains an unsafe path." >&2
    exit 1
fi

tar --zstd -xf "$archive" -C "$destination"

for required in bin/codex bin/codex-code-mode-host codex-package.json \
        codex-path/rg codex-resources/bwrap codex-resources/zsh/bin/zsh; do
    if [[ ! -f "${destination}/${required}" ]]; then
        echo "Codex package is incomplete: ${required}" >&2
        exit 1
    fi
done

for executable in bin/codex bin/codex-code-mode-host codex-path/rg \
        codex-resources/bwrap codex-resources/zsh/bin/zsh; do
    chmod 0755 "${destination}/${executable}"
done

if [[ "$("${destination}/bin/codex" --version)" != "codex-cli ${codex_version}" ]]; then
    echo "Downloaded Codex package did not report the pinned version ${codex_version}." >&2
    exit 1
fi

install -d -m 0755 "${destination}/licenses"
install -m 0644 "$license" "${destination}/licenses/LICENSE"
install -m 0644 "$notice" "${destination}/licenses/NOTICE"

printf 'Codex standalone package verified: %s (%s)\n' "$codex_version" "$codex_target"
