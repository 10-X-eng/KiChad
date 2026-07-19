# KiChad Codex architecture

KiChad targets the exact stable KiCad version named in `.kichad-base-version`; today that is
KiCad 10.0.4.  There is one supported code path.  Development heads, release candidates, KiCad 9,
legacy SWIG automation, GUI automation, and multi-version compatibility layers are out of scope.

## Process boundary

The KiChad project-manager process owns the docked chat panel and starts `codex app-server` as its
Codex inference and conversation process.  It speaks newline-delimited JSON-RPC over redirected
stdio.  ChatGPT authentication, the account's live model catalog, reasoning options, persistent
threads, streaming events, and server-initiated dynamic-tool calls all use the app-server protocol.

Dynamic tool requests are handled by the KiChad host.  KiChad does not run an MCP server,
middleware daemon, tool-server process, or web UI.  The app-server process is never given an
arbitrary KiChad tool that executes shell commands or drives the GUI.  Unrecognized tool calls are
rejected with a structured JSON-RPC error.  The owned process starts with MCP, shell, unified exec,
apps, browser/computer use, image generation, plugins, and multi-agent features disabled, so it
cannot inherit broader tools or MCP connectors from the user's global Codex configuration.  Live
Codex web search remains enabled solely for component, datasheet, availability, and design-evidence
research; it is not GUI browser automation and cannot mutate the project.

Set `KICHAD_CODEX_EXECUTABLE` when `codex` is not on `PATH`.  The executable is an installation
prerequisite, not a linked build dependency.  The owned child uses an isolated Codex home at
`~/.config/kichad/codex` by default, preventing global Codex configuration, MCP connectors, hooks,
plugins, or sessions from leaking into the design agent.  Codex stores the panel's native ChatGPT
authentication there; KiChad never stores access tokens in a project or its settings.  Set
`KICHAD_CODEX_HOME` to override that state location.

Before each submitted Codex turn, the project manager asks KiCad's registered schematic, board,
and project savers for a coherent incremental-history snapshot and records its commit identifier.
The panel's **Revert turn** action closes open editors through the normal KiCad flow and restores
that exact pre-turn state.  If a snapshot cannot be established, mutating native tools stay locked;
read-only conversation and inspection can continue.

The first four advertised native tools are `project`, `inspect`, `design`, and `pcb`.  `project`
reports the active design context and snapshot gate.  `inspect` parses KiCad 10 schematic, board, symbol, and
footprint s-expressions in-process and returns structural summaries or bounded matching expressions.
It accepts only existing project-relative paths, resolves symlinks before enforcing the project
root, checks the file extension against the document root, caps input/output sizes, and never writes.
`design` is the compiler front end for reusable `.kicad_kds` KiChad Design Script sidecars.  It
describes the versioned language, compiles either inline source or a project-confined sidecar into a
deterministic validated IR and pass plan, previews typed physical board statements as exact KiCad 10
protobuf JSON operations with stable UUIDv8 identities, and atomically saves only valid programs
behind the pre-turn snapshot gate. Preview is bounded and read-only. Existing sidecars require a
matching SHA-256 revision before replacement. KDS has
no host-language or shell escape; it declares project metadata, libraries, schematic hierarchy,
components, connectivity, board intent, design rules, sourcing, verification, and outputs.  See
`kichad-design-script.md` for the file contract and support policy.
`pcb` discovers the matching open board and instance token through KiCad 10's supported protobuf IPC
API.  Its bounded `describe` operation exposes exact protobuf JSON fields and nested enum values, so
the model does not infer message shapes.  It can read typed live items and create, field-mask update,
or delete footprints, traces, vias, arcs, zones, graphics, and text.  Each mutation requires the
pre-turn snapshot and is enclosed by KiCad `BeginCommit`/`EndCommit`; any validation, item-status,
transport, or commit failure drops the pending commit.  IPC requests have bounded timeouts and
execute off the wxWidgets UI thread; socket paths and instance tokens are not returned to the model.
KiChad persists the required API-enabled setting in its isolated configuration before it launches
editor children.  If a previous transaction response was lost, the next mutation first drops that
client's orphaned commit and retries `BeginCommit` once.

`tools/smoke-kichad-live-ipc.sh` provides an explicit, opt-in integration proof against an already
open disposable board copy.  It creates, commits, field-mask updates, commits, deletes, and commits a
temporary trace while checking per-item statuses and preservation of unmasked geometry.  Normal
builds and test runs never invoke this mutating smoke test.

`tools/generate-codex-protocol-schema.sh` regenerates the installed app-server's experimental JSON
Schema and TypeScript contract under the ignored `build/` tree.  This is the review/update path for
protocol changes; the runtime client accepts unknown notifications and relies only on the fields it
negotiates during `initialize`.

## Compiler and native tool surface

Codex primarily authors a KDS program.  The compiler parses and type-checks that program, resolves
logical design identities, produces a reviewable plan, establishes a snapshot, and invokes the
native schematic/library/PCB/sourcing/check/output backends.  The lower-level native calls remain
available for bounded inspection, diagnostics, and compiler implementation; they are not a second,
untracked source of design truth.  The sidecar is reusable source and the ordinary KiCad project
files are its compiled artifacts.

The complete tool surface is capped at nine host functions. Each function accepts schema-validated
requests and returns structured results; functionality is added to these tools
instead of creating narrowly named one-off tools.

1. `design` — load, save, compile, preview, apply, and verify a versioned `.kicad_kds` sidecar.
2. `project` — create/open projects, read context, set stackup/rules/net classes, and manage whole
   turn snapshots.
3. `source_parts` — verify MPNs/datasheets, query live distributor availability, and maintain the
   sourcing cache.
4. `library` — inspect or create verified KLC-compliant symbols, footprints, and model mappings.
5. `schematic` — losslessly inspect and mutate schematics, connectivity, hierarchy, annotations,
   and netlists.
6. `board` — inspect and mutate the live board through the KiCad 10 IPC API and transactions,
   including placement, copper, routing, zones, vias, constraints, and dimensions.
7. `inspect` — return compact structured design context plus rendered schematic/PCB/3D images.
8. `verify` — run structured ERC, DRC, connectivity, sourcing, and manufacturability checks.
9. `fabricate` — generate and verify Gerber, drill, position, BOM, and other fabrication outputs
   behind an explicit final-action permission gate.

The host advertises only implemented tools.  A tool is not exposed with a stub implementation.

## Mutation safety

Live board changes enter KiCad through the supported KiCad 10 IPC API and existing transaction
machinery so they participate in undo/redo, dirty-state handling, and editor refresh.  Each agent
turn also receives an atomic whole-project snapshot.  A failed or interrupted turn rolls back to
that snapshot, while a completed turn is one user-visible undo unit.

KiCad 10's public IPC surface does not cover the complete schematic and library editors.  Those
formats therefore use a lossless s-expression document layer that preserves trivia, unknown
expressions, ordering, quoting, and UUIDs.  Every mutation is written atomically and validated by
the corresponding KiCad 10 parser/CLI before it becomes the active project state.  Validation
failure restores the previous files and becomes structured tool output.

Destructive operations, snapshot rollback, and fabrication export require visible confirmation.
Tool calls, arguments, affected objects, previews, progress, validation results, and errors remain
visible in the conversation.

## Definition of done

The integration is not production-ready until unattended natural-language runs create all three
reference boards described in the project specification: a two-layer 555 design, an MCU board with
USB-C/regulation/custom footprint, and a four-layer fine-pitch board with planes and a real stackup.
Each fixture must finish routed, sourced, clean under ERC/DRC, and produce validated fabrication
outputs.  Injected auth, sourcing, parser, IPC, validation, cancellation, and export failures must
leave the project recoverable.
