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
./tools/smoke-codex-app-server-protocol.sh  # Verify the embedded JSON-RPC contract.
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
`KICHAD_CODEX_EXECUTABLE` to its absolute path before launching.  The panel's ChatGPT sign-in is
stored by Codex in an isolated KiChad Codex home, not in the project; set `KICHAD_CODEX_HOME` only
when you intentionally want a different state location.  The design-tool boundary and safety model
are documented in [docs/kichad-codex-architecture.md](docs/kichad-codex-architecture.md).  Each
submitted turn first snapshots the project through KiCad's local-history system, and the panel can
restore that complete pre-turn state.  The initial native `project` and `inspect` calls expose
project context and bounded, read-only KiCad 10 design inspection without shell or GUI automation;
the `design` call reads exact source, compiles, previews, atomically saves, and transactionally
applies reusable `.kicad_kds` project sidecars, and the `pcb` call exposes the exact protobuf field
schema and connects directly to the open PCB Editor through KiCad 10's protobuf IPC API for bounded
live reads and snapshot-gated, native undoable transactions. The read-only `verify` call runs the
matching sibling KiCad 10.0.4 ERC or DRC engine (including DRC schematic parity), rejects reports
from any other KiCad version, and returns complete counts plus bounded pageable violations. Its
`sourcing` operation compiles the project's one KDS sidecar and fails physical components whose
cached evidence is incomplete, stale, unavailable, or not active.

The native `fabricate` call plans and exports the fixed `kichad-production-10.0.4-v4` release
profile. It accepts only the current KiCad 10.0.4 board and schematic formats, binds the request to
the exact compiled KDS SHA-256, and requires KDS declarations for ERC, DRC, sourcing, fabrication,
Gerber, drill, IPC-D-356 electrical-test, placement, and BOM intent plus an explicit physical
stackup. Export reruns the gates and the matching sibling `kicad-cli` from a private bounded project
snapshot, so KiCad cannot rewrite live local settings while checking or plotting. The snapshot
includes project-local symbol and footprint libraries referenced by its native tables. A visible
final-action confirmation is
mandatory; ignored checks or exclusions also require explicit waiver approval. The native board's
schematic-footprint reference/library-ID inventory must exactly match KDS, and the completed BOM and
placement reference sets must exactly match the compiled physical and non-DNP component sets.
KiChad validates every native artifact, writes a hash manifest, and atomically replaces only
`fabrication/`; any failure preserves the prior package. Native KiCad plot timestamps are retained,
so the manifest records exact bytes for each run rather than claiming byte-identical Gerbers across
separate runs. Optional STEP, multipage fabrication PDF, inspectable IPC-2581C XML, and ODB++ can be
declared in the same KDS sidecar. IPC-2581 is fixed to millimetres and precision 6, then parsed and
structurally checked against the planned board and KDS references. ODB++ is fixed to an ODB 8.1,
millimetre, precision-4 ZIP whose complete bounded archive and manufacturing structure are checked
without extraction.

KiChad forces the Codex app-server's built-in web search to live, high-context mode for new and
resumed project conversations. The embedded agent instructions require current manufacturer,
datasheet, lifecycle, and distributor evidence before a component can be accepted into a design;
that exact evidence is written into the component's KDS `source` form and checked by the native
sourcing gate. There is no parallel sourcing database or generated context document. GUI browser
automation and inherited MCP connectors remain disabled.

KiChad Design Script is the versioned source language Codex uses to describe a complete design.
A `project.kicad_kds` sidecar lives beside the normal project, schematic, and board files; KiChad
shows it in the project tree and can load, compile, preview, save, or apply it without losing source
text. KDS is the single external design representation: Codex reads and writes the same compact,
self-describing source that is exported with the project, while compiler IR and protobuf messages
remain private implementation details. Physical board statements use explicit units and stable
logical IDs; preview reports their deterministic target identities without changing the board. The
sidecar declares libraries, components, nets, board intent, rules, sourcing, checks, and fabrication
outputs, while ordinary KiCad files remain the compiler artifacts. The format, grammar, safety rules,
and production support criteria are documented in
[docs/kichad-design-script.md](docs/kichad-design-script.md).
The native `design.describe` operation also returns the authoritative AI-readable coverage catalog:
every design, verification, manufacturing, interchange, editor, and auxiliary-application facet is
marked `qualified`, `partial`, or `unrepresented` with explicit remaining gaps. The catalog is
compiler introspection, not a second design representation.

For an opt-in transaction proof, first open a disposable project copy in the installed PCB Editor,
then run `tools/smoke-kichad-live-ipc.sh --allow-mutation PROJECT_DIRECTORY BOARD_FILE`.  The smoke
test creates, field-mask updates, and deletes one temporary trace through the official KiCad 10 IPC
transaction API; it is never run implicitly by the build.

For the self-contained KDS transaction proof, run
`tools/smoke-kichad-kds-apply.sh --allow-mutation`. It launches a disposable build-tree PCB Editor
with an isolated configuration and project copy, applies the committed KDS fixture, and proves that
a repeated apply converges the same thirteen managed identities—twelve authored PCB primitives and
one compiler-created footprint—without duplicates. It also applies and
reapplies a two-file hierarchical schematic, preserves the existing root screen UUID and unmanaged
title-block company field, creates stable sheet/pin/interface UUIDs, proves the second apply is
byte-idempotent, injects a native-validation failure to prove exact rollback, and exports the
resulting hierarchy through the real `kicad-cli` schematic loader. The same proof resolves exact
project-local resistor symbols, places units on root and child sheets with rotation/mirroring,
places a real derived virtual `GND` power symbol with the canonical `(footprint none)` spelling,
flattens its inheritance into the native cache, preserves each symbol's native
BOM/board/position/simulation flags, attaches project-global signal and power
nets to resolved pin coordinates, and checks their exact nodes in the exported netlist. The native schematic proof also
reconciles net-derived local/global labels, a name-owned bus alias, plus stable-ID wires, junctions,
buses, diagonal bus entries, net-targeted directive flags, and polygonal schematic rule areas with
border-attached directives and explicit native fields. It also applies and
reads back the one authored physical stackup—including finish, impedance policy, bevelled edge
connector, edge plating, masks, paste, silkscreen, copper, and locked dielectric properties—through
KiCad's native stackup API. The same live proof applies and reads back the complete global Board
Setup constraint set through a typed native rules endpoint, including physical via consistency and
the semantic legacy copper-edge mode. It also applies and reads back the one canonical KDS net-class
table—including explicit priority order, inherited fields, via and microvia geometry, schematic
styles, colors, tuning profiles, and pattern assignments—then proves an invalid native replacement
is rejected without mutation. It compiles the canonical KDS custom-rule set into one internal native
rule document, loads all rule semantics through KiCad's real DRC engine, reads the exact document
back, and proves malformed replacement input cannot change the active rules. It also generates the
complete native project symbol and footprint tables from the same KDS declarations, validates them
with KiCad's parser, repeats them byte-for-byte, and covers exact rollback. It creates and fills a
deterministic copper zone through KiCad's official zone engine, creates a distinct locked keepout
rule area with exact prohibited-item policy, creates native multiline board text with deterministic
typography, and creates all five native dimension styles with exact geometry and measurement policy.
It also resolves and places an existing schematic-linked footprint on the back side while proving
the footprint UUID, symbol path, pad UUID, and flipped pad layers are preserved. A second absent
footprint is parsed from the declared project-local `.pretty` library, linked to its deterministic
hierarchical symbol path, assigned its pad net, and created with a deterministic KDS-owned instance
UUID in the same native transaction. Repeat apply proves it is not duplicated; removing and
restoring its placement proves exact deletion and recreation with the same identity. The committed
board, schematic, symbol, and footprint fixtures are serialized in the exact formats emitted by
KiCad 10.0.4, and the smoke test rejects stale fixture versions before opening the editor. The
harness also runs the native `verify` tool against the current-format schematic and board, proving
the real 10.0.4 ERC/DRC JSON contracts and schematic-parity category. Unit coverage separately
proves the deterministic KDS sourcing gate, including physical-versus-virtual coverage, freshness,
lifecycle, stock, malformed evidence, and failure paths. It never connects to or stops an existing
KiChad process.

For the real fabrication integration proof, build `qa_common` and `kicad-cli`, then run
`tools/smoke-kichad-fabrication.sh --allow-mutation`. The harness uses isolated configuration and a
disposable copy of the exact current-format fixture, runs native ERC/DRC and every production export,
checks the installed manifest and artifacts, proves live `.kicad_prl` state is untouched, and removes
the temporary project afterward.

For the complete KDS-to-fabrication component proof, also build `pcbnew` and run
`tools/smoke-kichad-fabrication-component.sh --allow-mutation`. It launches only a disposable PCB
Editor, applies and saves a sourced two-resistor KDS design through official IPC, requires clean
native ERC and DRC with zero ignored checks, and proves that the native board inventory, routed
placement CSV, KDS BOM, and production manifest contain the same exact component references.

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
