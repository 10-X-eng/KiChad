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
./tools/fetch-kicad-libraries.sh
./tools/run-kichad.sh
```

The build and install trees live below the ignored `build/` directory.  Nothing is installed into
`/usr/local`, and the system KiCad installation is not modified.  The install contains `kichad` and
`kichad-cli` launchers that configure the local runtime library path while retaining the upstream
executable names for compatibility.

The library fetch is optional for compiling and testing, but it supplies the current official
symbols, footprints, 3D models, and project templates needed for a complete interactive session.
The repositories are shallow-cloned into `build/libraries/`; rerunning the command updates them with
fast-forward-only pulls.

## Useful checks

```sh
build/install/bin/kichad-cli version
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

KiChad embeds a native Codex app-server client in the KiCad process.  When KiChad starts, the
application directly launches and owns one `codex app-server` child over redirected stdio; closing
the application terminates that exact child.  App-server dynamic-tool calls are dispatched by the
host itself, so there is no wrapper daemon, MCP service, or separate tool server.  PCB mutations use
the supported KiCad 10 IPC API and KiCad transactions.  Schematic and library work uses a lossless
s-expression layer with KiCad validation because KiCad 10's public IPC surface does not cover those
editors.  Network work stays off the UI thread, and credentials are never stored in the repository
or compiled defaults.

## Syncing upstream

```sh
git fetch upstream --prune --tags
git switch -c upgrade/kicad-X.Y.Z X.Y.Z^{}
# Port the focused KiChad commits, build, test, and review before promoting it.
```

Only move `.kichad-base-version` to an official stable release after it has passed the KiChad test
matrix.  Keep downstream infrastructure in focused commits so conflicts remain easy to resolve.
Never modify or force-push an upstream maintenance branch or tag.
