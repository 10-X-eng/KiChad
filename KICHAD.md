# KiChad development baseline

KiChad is a downstream of the canonical
[KiCad source repository](https://gitlab.com/kicad/code/kicad).  The `upstream` remote tracks
GitLab, while `origin` tracks the KiChad GitHub repository.  The application executable names and
internal APIs intentionally remain compatible with KiCad at this stage; downstream builds carry a
`KiChad` version suffix and use `~/.config/kichad` so they do not overwrite normal KiCad settings.
The active downstream line is based on the stable `10.0.4` tag.  It does not track KiCad's moving
development branch or release candidates.

## Ubuntu 24.04 / KDE neon quick start

```sh
./tools/bootstrap-kichad-ubuntu.sh
./tools/check-codex-app-server.sh
./tools/build-kichad.sh
./tools/run-kichad.sh
```

## Ubuntu 24.04 AppImage

Build the distributable image locally before changing or diagnosing CI:

```sh
./tools/bootstrap-kichad-appimage-ubuntu.sh
./tools/build-kichad-appimage.sh
build/appimage/artifacts/KiChad-*.AppImage
```

This invokes the same build script as `.github/workflows/linux-appimage.yml`, pins the official
KiCad AppImage packager and every downloaded packaging tool by commit or SHA-256, downloads the
complete official Codex standalone package at its pinned version and checksum, and then verifies
both KiCad CLI and the Codex app-server protocol from the assembled package. It always installs the
complete official symbols, footprints, templates, and 3D-model libraries; there is one artifact
with one capability boundary.

No credentials are copied into the image. At runtime KiChad stores its isolated Codex state below
the user's KiChad configuration directory and launches the bundled app-server as its own child.

The build and install trees live below the ignored `build/` directory.  Nothing is installed into
`/usr/local`, and the system KiCad installation is not modified.  The install contains `kicad` and
`kicad-cli` launchers that configure the local runtime library path while retaining the upstream
executable names for compatibility. They execute the installed native `_kicad` and `_kicad-cli`
files, so the wrappers cannot recurse into themselves.

A full build fetches the official symbol, footprint, 3D-model, and template repositories at the
exact stable version in `.kichad-base-version`, then installs them beside KiChad under
`build/install/share/kicad`. This keeps the application and its standard libraries on one supported
version and makes the installed launchers self-contained. `./tools/fetch-kicad-libraries.sh` and
`./tools/install-kichad-libraries.sh` remain available when only the library runtime needs to be
refreshed. Neither command tracks the libraries' moving development branch.

## Useful checks

```sh
build/install/bin/kicad-cli version
./tools/check-kichad-libraries.sh
./tools/smoke-codex-app-server-protocol.sh
ctest --preset kichad-release
./tools/run-kichad.sh  # GUI smoke check; close the window after startup.
```

`tools/build-kichad.sh` always uses the tracked `kichad-release` preset, installs only after a
successful compile, and finishes with a headless version check.  It also verifies that `HEAD`
descends from the stable tag named by `.kichad-base-version` and that the installed binary reports
that version.  Set `KICHAD_BUILD_JOBS` to override its default of one job per detected CPU, or pass
target names to build a smaller target set.

For a narrow change, prefer the closest QA target or test label instead of running the entire suite.
CMake always exports `build/release/compile_commands.json` for language tooling.

## Codex integration seam

KiChad embeds a native Codex app-server client in the KiCad process. Distribution packages carry
the complete pinned Codex standalone runtime; developer builds may use a Codex on `PATH`. When
KiChad starts, the application directly launches and owns one `codex app-server` child over
redirected stdio; closing the application terminates that exact child. App-server dynamic-tool
calls are dispatched by the
host itself, so there is no wrapper daemon, MCP service, or separate tool server.  PCB mutations use
the supported KiCad 10 IPC API and KiCad transactions.  Schematic and library work uses a lossless
s-expression layer with KiCad validation because KiCad 10's public IPC surface does not cover those
editors.  Network work stays off the UI thread, and credentials are never stored in the repository
or compiled defaults.

The read-only `inspect.render` operation plots current schematic and 2D board views through the
matching `kicad-cli`, renders a native 3D board view when requested, crops blank plot margins, and
attaches the resulting PNG directly to the Codex tool response. Preview files live only under the
project's derived `.kichad/previews/` directory. A committed `design.apply` saves the live board and
returns `verification.status = not_run`; the embedded agent must inspect the rendered result and run
ERC/DRC before it can describe a design as correct.

## Syncing upstream

```sh
git fetch upstream --prune --tags
git switch -c upgrade/kicad-X.Y.Z X.Y.Z^{}
# Port the focused KiChad commits, build, test, and review before promoting it.
```

Only move `.kichad-base-version` to an official stable release after it has passed the KiChad test
matrix.  Keep downstream infrastructure in focused commits so conflicts remain easy to resolve.
Never modify or force-push an upstream maintenance branch or tag.
