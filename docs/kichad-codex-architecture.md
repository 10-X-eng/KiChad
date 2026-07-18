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
rejected with a structured JSON-RPC error.

Set `KICHAD_CODEX_EXECUTABLE` when `codex` is not on `PATH`.  The executable is an installation
prerequisite, not a linked build dependency, and it stores authentication using Codex's own account
storage.  KiChad never stores access tokens in a project or its settings.

`tools/generate-codex-protocol-schema.sh` regenerates the installed app-server's experimental JSON
Schema and TypeScript contract under the ignored `build/` tree.  This is the review/update path for
protocol changes; the runtime client accepts unknown notifications and relies only on the fields it
negotiates during `initialize`.

## Native tool surface

The complete tool surface is capped at nine host functions.  Each function accepts typed,
schema-validated requests and returns structured results; functionality is added to these tools
instead of creating narrowly named one-off tools.

1. `project` — create/open projects, read context, set stackup/rules/net classes, and manage whole
   turn snapshots.
2. `source_parts` — verify MPNs/datasheets, query live distributor availability, and maintain the
   sourcing cache.
3. `library` — inspect or create verified KLC-compliant symbols, footprints, and model mappings.
4. `schematic` — losslessly inspect and mutate schematics, connectivity, hierarchy, annotations,
   and netlists.
5. `board` — inspect and mutate the live board through the KiCad 10 IPC API and transactions,
   including placement, copper, routing, zones, vias, constraints, and dimensions.
6. `inspect` — return compact structured design context plus rendered schematic/PCB/3D images.
7. `verify` — run structured ERC, DRC, connectivity, sourcing, and manufacturability checks.
8. `fabricate` — generate and verify Gerber, drill, position, BOM, and other fabrication outputs
   behind an explicit final-action permission gate.
9. `ask_user` — request a structured decision when an engineering choice cannot safely be inferred.

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
