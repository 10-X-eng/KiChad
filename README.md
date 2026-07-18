# KiChad

KiChad is a Codex-oriented downstream of KiCad.  It preserves the complete upstream history and
keeps the application architecture compatible with KiCad while providing a reproducible local
development environment for AI-assisted work.

Development is pinned to the latest stable KiCad release: **10.0.4**.  Preview releases and the
upstream development branch are intentionally excluded.  The pinned version is recorded in
`.kichad-base-version`, and the build script verifies both the Git ancestry and resulting binary.

See [KICHAD.md](KICHAD.md) for the Linux quick start, repository layout, runtime libraries, and
upstream-sync workflow.  KiChad is an independent project and is not an official KiCad build.

On Ubuntu 24.04 or KDE neon, the repeatable local build is:

```sh
./tools/bootstrap-kichad-ubuntu.sh  # Run once to install native dependencies.
./tools/check-codex-app-server.sh   # Verify the Codex app-server prerequisite.
./tools/build-kichad.sh             # Configure, compile, install, and smoke-check.
```

Build products stay in `build/`, and the runnable installation is written to `build/install/`.
Rerun `tools/build-kichad.sh` after source changes; it uses the tracked CMake preset, defaults to
one build job per CPU, and accepts `KICHAD_BUILD_JOBS` when you need to cap parallelism.
After a successful build, run `./tools/run-kichad.sh` (or `build/install/bin/kichad`) and use
`build/install/bin/kichad-cli version` for a headless check; both launchers set the local runtime
library path without changing the system installation.

The project manager includes a native docked Codex panel with ChatGPT sign-in, a dynamic account
model catalog, reasoning selection, streaming conversations, and visible app-server status.
Launching KiChad directly starts and owns one `codex app-server` child process and communicates
with it over redirected stdio; no wrapper daemon, MCP server, or separate tool server is involved.
Closing KiChad terminates that owned child.  If Codex is not on `PATH`, set
`KICHAD_CODEX_EXECUTABLE` to its absolute path before launching.  The design-tool boundary and
safety model are documented in
[docs/kichad-codex-architecture.md](docs/kichad-codex-architecture.md).
Developers can run `tools/generate-codex-protocol-schema.sh` to inspect the exact protocol exposed
by their installed Codex app-server without committing generated schemas.

---

# KiCad upstream README

For specific documentation about [building KiCad](https://dev-docs.kicad.org/en/build/), policies
and guidelines, and source code documentation see the
[Developer Documentation](https://dev-docs.kicad.org) website.

You may also take a look into the [Wiki](https://gitlab.com/kicad/code/kicad/-/wikis/home),
the [contribution guide](https://dev-docs.kicad.org/en/contribute/).

For general information about KiCad and information about contributing to the documentation and
libraries, see our [Website](https://kicad.org/) and our [Forum](https://forum.kicad.info/).

## Build state

KiCad uses a host of CI resources.

GitLab CI pipeline status can be viewed for Linux and Windows builds of the latest commits.

## Release status
[![latest released version(s)](https://repology.org/badge/latest-versions/kicad.svg)](https://repology.org/project/kicad/versions)
[![Release status](https://repology.org/badge/tiny-repos/kicad.svg)](https://repology.org/metapackage/kicad/versions)

## Files
* [AUTHORS.txt](AUTHORS.txt) - The authors, contributors, document writers and translators list
* [CMakeLists.txt](CMakeLists.txt) - Main CMAKE build tool script
* [copyright.h](copyright.h) - A very short copy of the GNU General Public License to be included in new source files
* [Doxyfile](Doxyfile) - Doxygen config file for KiCad
* [INSTALL.txt](INSTALL.txt) - The release (binary) installation instructions
* [uncrustify.cfg](uncrustify.cfg) - Uncrustify config file for uncrustify sources formatting tool
* [_clang-format](_clang-format) - clang config file for clang-format sources formatting tool

## Subdirectories

* [3d-viewer](3d-viewer)         - Sourcecode of the 3D viewer
* [bitmap2component](bitmap2component)  - Sourcecode of the bitmap to PCB artwork converter
* [cmake](cmake)      - Modules for the CMAKE build tool
* [common](common)            - Sourcecode of the common library
* [cvpcb](cvpcb)             - Sourcecode of the CvPCB tool
* [demos](demos)             - Some demo examples
* [doxygen](doxygen)     - Configuration for generating pretty doxygen manual of the codebase
* [eeschema](eeschema)          - Sourcecode of the schematic editor
* [gerbview](gerbview)          - Sourcecode of the gerber viewer
* [include](include)           - Interfaces to the common library
* [kicad](kicad)             - Sourcecode of the project manager
* [libs](libs)           - Sourcecode of KiCad utilities (geometry and others)
* [pagelayout_editor](pagelayout_editor) - Sourcecode of the pagelayout editor
* [patches](patches)           - Collection of patches for external dependencies
* [pcbnew](pcbnew)           - Sourcecode of the printed circuit board editor
* [plugins](plugins)           - Sourcecode for the 3D viewer plugins
* [qa](qa)                - Unit testing framework for KiCad
* [resources](resources)         - Packaging resources such as bitmaps and operating system specific files
    - [bitmaps_png](resources/bitmaps_png)       - Menu and program icons
    - [project_template](resources/project_template)          - Project template
* [scripting](scripting)         - Python integration for KiCad
* [thirdparty](thirdparty)           - Sourcecode of external libraries used in KiCad but not written by the KiCad team
* [tools](tools)             - Helpers for developing, testing and building
* [translation](translation) - Translation data files (managed through [Weblate](https://hosted.weblate.org/projects/kicad/master-source/) for most languages)
* [utils](utils)             - Small utils for KiCad, e.g. IDF, STEP, and OGL tools and converters
