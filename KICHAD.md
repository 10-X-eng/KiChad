# KiChad development baseline

KiChad is a downstream of the canonical
[KiCad source repository](https://gitlab.com/kicad/code/kicad).  The `upstream` remote tracks
GitLab, while `origin` tracks the KiChad GitHub repository.  The application executable names and
internal APIs intentionally remain compatible with KiCad at this stage; downstream builds carry a
`KiChad` version suffix and use `~/.config/kichad` so they do not overwrite normal KiCad settings.

## Ubuntu 24.04 / KDE neon quick start

```sh
./tools/bootstrap-kichad-ubuntu.sh
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
ctest --preset kichad-release
./tools/run-kichad.sh  # GUI smoke check; close the window after startup.
```

`tools/build-kichad.sh` always uses the tracked `kichad-release` preset, installs only after a
successful compile, and finishes with a headless version check.  Set `KICHAD_BUILD_JOBS` to override
its default of one job per detected CPU, or pass target names to build a smaller target set.

For a narrow change, prefer the closest QA target or test label instead of running the entire suite.
CMake always exports `build/release/compile_commands.json` for language tooling.

## Codex integration seam

The default build includes KiCad's protobuf API, NNG-based IPC server, Python manager, and QA tools.
Those are the preferred seams for a Codex bridge: keep model/network work outside the UI thread,
send typed commands through the API boundary, and make every board or schematic mutation undoable.
Do not place API credentials in the repository or compiled defaults.

## Syncing upstream

```sh
git fetch upstream --prune --tags
git switch master
git rebase upstream/master
git push origin master
```

Keep downstream infrastructure in focused commits so conflicts remain easy to resolve.  Never force
push an upstream maintenance branch or tag.
