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
cannot inherit broader tools or MCP connectors from the user's global Codex configuration. Codex
web search is forced to `live` with high context for both new and resumed conversations. The agent
instructions require it to verify the manufacturer, exact MPN, primary datasheet, lifecycle, and
current distributor availability before accepting each component. Built-in web search is separate
from GUI browser automation, remains enabled while `browser_use` is disabled, and cannot mutate the
project.

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
describes the versioned language, reads the exact bounded source as model context, compiles either
inline source or a project-confined sidecar into a private deterministic validated IR and pass plan,
previews typed physical board statements and schematic hierarchy files through their KDS logical
and stable UUIDv8 identities,
atomically saves only valid programs, and applies the currently supported physical subset behind the
pre-turn snapshot gate. Read and preview are bounded and read-only. Compiler IR and protobuf
payloads are deliberately not exposed as a second design representation. Existing sidecars require
a matching SHA-256 revision before replacement. KDS has
no host-language or shell escape; it declares project metadata, libraries, schematic hierarchy,
components, connectivity, board intent, design rules, sourcing, verification, and outputs.  See
`kichad-design-script.md` for the file contract and support policy.
`pcb` discovers the matching open board and instance token through KiCad 10's supported protobuf IPC
API.  Its bounded `describe` operation exposes exact protobuf JSON fields and nested enum values, so
the model does not infer message shapes.  It can read typed live items and create, field-mask update,
or delete footprints, traces, vias, arcs, copper zones, rule areas, graphics, text, and all five
native dimension styles. Each mutation requires the pre-turn snapshot and is enclosed by KiCad
`BeginCommit`/`EndCommit`; any validation, item-status,
transport, or commit failure drops the pending commit.  IPC requests have bounded timeouts and
execute off the wxWidgets UI thread; socket paths and instance tokens are not returned to the model.
KiChad persists the required API-enabled setting in its isolated configuration before it launches
editor children.  If a previous transaction response was lost, the next mutation first drops that
client's orphaned commit and retries `BeginCommit` once.

`tools/smoke-kichad-live-ipc.sh` provides an explicit, opt-in integration proof against an already
open disposable board copy.  It creates, commits, field-mask updates, commits, deletes, and commits a
temporary trace while checking per-item statuses and preservation of unmasked geometry.  Normal
builds and test runs never invoke this mutating smoke test.

`tools/smoke-kichad-kds-apply.sh --allow-mutation` is the self-contained compiler integration proof.
It starts only its own build-tree PCB Editor, uses isolated settings and a temporary copy of the
committed fixture, applies the KDS sidecar, and repeats the apply to prove stable object identity and
duplicate-free convergence. It applies the single complete KDS net-class table through the typed
native project API, reads every class and assignment back, rejects an invalid atomic replacement,
and verifies the prior table did not change. It also lowers the single KDS custom-rule
representation to one internal native document, installs it through a bounded board endpoint,
reloads KiCad's DRC engine, verifies exact readback, and proves invalid input cannot mutate the
active rules. It generates and repeats exact KiCad-parsed project symbol and footprint tables from
the canonical KDS library declarations. It verifies a deterministic managed copper zone is filled
by KiCad's official zone engine after each transaction and a distinct managed keepout remains an unfilled,
locked rule area with exact prohibited-item settings. It also creates deterministic native board
text and all five native dimension styles, then reapplies each distinct oneof field mask and verifies
reference-resolved placement through a narrow native footprint transform: the parent and pad UUIDs,
schematic symbol path, and child geometry survive a front-to-back flip. Its cleanup targets only the
process and directory it created. The fixture also reconciles an existing root schematic and a new
child screen, preserves the root screen UUID and unmanaged title-block data, proves byte-idempotent
reapply, injects native validation failure and verifies exact rollback/recovery, and loads the final
hierarchy through `kicad-cli` to export a non-empty netlist.

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

KDS is the only external representation of design intent. Internal JSON IR, transaction actions,
managed-state records, and protobuf messages are compiler implementation details. The hidden
`*.kicad_kds_state` file records only deterministic ownership identities needed for idempotent
reconciliation; a short-lived `*.kicad_kds_journal` safely carries ownership across an interrupted
apply. The generated `.kicad_dru` file is likewise an internal compiler artifact; conditional-rule
intent is authored only in the exported `.kicad_kds` sidecar. Its exact prior presence and bytes are
journaled so a failed apply restores both the file and live DRC engine. Existing schematic-linked footprints are resolved uniquely by reference and transformed in
place; they are never added to KDS ownership state. Managed copper zones are committed unfilled,
then KiCad's official refill command is polled until every desired zone is authoritatively present
and filled. A rejected or timed-out refill retains the recovery journal, and the next apply safely
reconciles and retries. Keepout rule areas use their own ownership namespace and exact protobuf
field mask; they never participate in copper-fill completion polling. Both state files are project-confined and
included in whole-turn local history snapshots.

Hierarchy declarations compile into stable native sheet, sheet-pin, and hierarchical-label UUIDs.
The planner first inventories existing screen UUIDs and uses those exact identities in nested KiCad
instance paths; only missing screens receive deterministic compiler UUIDs. A lossless reconciler
replaces or inserts direct managed expressions and removes only identities proven in the previous
state. It updates the root title while retaining other title-block fields. Exact prior schematic
presence and bytes are stored in the apply journal, files install atomically, and a bounded sibling
`kicad-cli` process loads the complete hierarchy before any PCB item commit. Timeout or native parse
failure restores schematics, library tables, and board settings in reverse order.

Component declarations use one explicit multi-unit form. Before planning, KiChad inventories only
the referenced project-local symbol libraries, confines canonical paths to the project, rejects
symlinks, and passes their exact bytes to the lossless symbol resolver. The resolver extracts the
named native symbol and unit pin geometry; the planner then emits stable placed-symbol/pin UUIDs and
transforms each KDS net or no-connect endpoint onto a real pin coordinate. Project-global KDS nets
lower to native global labels, so connectivity spans hierarchy screens without a second net
representation. Cached symbols are reconciled by library ID inside each screen's `lib_symbols`
container while unrelated cache entries retain their bytes. Any unresolved, derived, missing, or
unmanaged-colliding symbol aborts before installation. The same atomic file journal and native
hierarchy netlist validation cover sheets, cached symbols, placed units, connectivity, and rollback.

Native schematic wires, junctions, buses, and bus entries flow through the same compiler/planner
boundary. KDS authors endpoints rather than KiCad's signed bus-entry size, carries explicit
sheet ownership and stable human-readable IDs, and lowers styling into bounded native stroke,
junction-diameter, and RGBA fields. The reconciler proves kind/UUID agreement before touching an
existing direct item, so an unmanaged collision or stale kind aborts instead of claiming user data.

Project-local library declarations compile into complete native `sym-lib-table` and
`fp-lib-table` artifacts. KiCad's library-table parser validates the generated type, version, and
rows before project-confined atomic replacement. The apply journal retains the exact prior presence
and bytes of both files and restores them in reverse order with board settings after a pre-commit
failure. Installed/global library declarations remain nickname dependencies and are never copied
into the project tables; model declarations remain KDS dependencies because KiCad has no model
table.

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
