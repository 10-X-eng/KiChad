#!/usr/bin/env bash

set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This bootstrap script supports Ubuntu/Debian hosts with apt-get." >&2
    exit 1
fi

as_root=()

if (( EUID != 0 )); then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "sudo is required when the script is not run as root." >&2
        exit 1
    fi

    as_root=( sudo )
fi

"${as_root[@]}" apt-get update
"${as_root[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    --no-install-recommends \
    build-essential \
    ccache \
    cmake \
    gettext \
    git \
    libboost-all-dev \
    libbz2-dev \
    libcairo2-dev \
    libcurl4-openssl-dev \
    libegl-dev \
    libfontconfig1-dev \
    libfreetype-dev \
    libgit2-dev \
    libgl1-mesa-dev \
    libglew-dev \
    libglm-dev \
    libglu1-mesa-dev \
    libgtk-3-dev \
    libharfbuzz-dev \
    libngspice0-dev \
    libnng-dev \
    libocct-data-exchange-dev \
    libocct-foundation-dev \
    libocct-modeling-algorithms-dev \
    libocct-modeling-data-dev \
    libocct-ocaf-dev \
    libocct-visualization-dev \
    libpixman-1-dev \
    libpoppler-dev \
    libpoppler-glib-dev \
    libprotobuf-dev \
    libsecret-1-dev \
    libspnav-dev \
    libssl-dev \
    libwayland-dev \
    libwxgtk3.2-dev \
    libwxgtk-webview3.2-dev \
    libx11-dev \
    libzint-dev \
    libzstd-dev \
    ngspice \
    ninja-build \
    pkg-config \
    protobuf-compiler \
    python3-cairosvg \
    python3-dev \
    python3-numpy \
    python3-pytest \
    shared-mime-info \
    swig \
    unixodbc-dev \
    wayland-protocols \
    zlib1g-dev
