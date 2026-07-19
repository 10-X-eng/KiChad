# KiChad Design Script

KiChad Design Script (KDS) is the deterministic source language for a complete KiChad project.  A
script is a reusable `project.kicad_kds` sidecar stored beside `project.kicad_pro`,
`project.kicad_sch`, and `project.kicad_pcb`.  The sidecar is source; ordinary KiCad files and
fabrication packages are compiler outputs.

KDS uses s-expression syntax because it is compact for Codex, reviewable in version control, and
structurally compatible with KiCad's native formats.  It is not Scheme and cannot execute host
code, load plugins, call a shell, access the network, or drive the GUI.  Every form is parsed and
bounded; unknown top-level, board, check, and output kinds are rejected before a program can be
saved or dispatched to a KiChad compiler pass.

## File contract

- Extension: `.kicad_kds`
- Root expression: `kichad_design`
- Required version form: `(version 1)`
- Encoding: UTF-8
- Maximum source size: 1 MiB
- Maximum nesting: 256 expressions
- Save behavior: compile first, then snapshot-backed atomic replacement
- Concurrency: replacing an existing sidecar requires its current SHA-256 digest
- Compatibility: unknown top-level forms are errors; newer syntax requires a new language version

The project manager recognizes the extension, displays sidecars in the project tree, and opens them
as text. The native `design` tool supports `describe`, exact `read`, inline or file-backed `compile`,
read-only `preview`, `save`, and snapshot-gated `apply`. Read returns the original bounded UTF-8
source plus its revision metadata, never a generated context projection. Loading a sidecar never
rewrites it. Saving preserves the supplied source byte-for-byte after the compiler accepts it.
Preview reports KDS logical IDs, deterministic target UUIDs, counts, and unsupported-backend
diagnostics without connecting to or changing the PCB Editor. Internal compiler IR and KiCad
protobuf payloads are not exposed as a second design representation.

KDS itself is the AI context and the only external design representation. Its names are explicit,
physical values retain readable engineering units, references resolve locally, and every generated
object has a stable authored logical ID. A model reads, reviews, edits, imports, and exports this
same source; it never needs to reconstruct design intent from KiCad serialization or backend JSON.

## Version 1 source model

```scheme
(kichad_design
  (version 1)
  (project sensor_node
    (title "Production Sensor Node")
    (company "Example Engineering")
    (revision "A"))
  (units mm)

  (library symbol Device
    (uri "${KICAD10_SYMBOL_DIR}/Device.kicad_sym"))
  (library footprint Resistor_SMD)

  (component R1
    (symbol "Device:R")
    (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric")
    (property "Tolerance" "1%"))
  (component LED1
    (symbol "Device:LED")
    (value "GREEN")
    (footprint "LED_SMD:LED_0603_1608Metric"))
  (net LED_A (pin R1 1) (pin LED1 1))

  (board
    (stackup (copper_layers 2) (thickness 1.6mm))
    (outline (rect (id board-edge) (at 0mm 0mm) (size 40mm 30mm)))
    (place R1 (at 10mm 10mm) (rotation 0deg) (side front))
    (route LED_A (id led-a-trace) (from 10mm 10mm) (to 20mm 10mm)
      (width 0.25mm) (layer F.Cu))
    (via LED_A (id led-a-via) (at 20mm 10mm) (diameter 0.8mm) (drill 0.4mm))
    (zone LED_A
      (id led-a-plane)
      (name "LED copper plane")
      (layers F.Cu)
      (outline
        (polygon
          (point 1mm 1mm) (point 39mm 1mm) (point 39mm 29mm) (point 1mm 29mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection thermal (thermal_gap 0.3mm) (thermal_spoke_width 0.35mm))
      (islands remove_below (minimum_area 1mm2))
      (fill solid)
      (border diagonal_edge (pitch 0.5mm))))

  (rule default_clearance (minimum 0.2mm))
  (source R1
    (manufacturer "Yageo")
    (mpn "RC0603FR-0710KL")
    (supplier "DigiKey")
    (quantity 1))
  (check erc)
  (check drc)
  (check sourcing)
  (output gerbers)
  (output drill)
  (output bom))
```

The compiler normalizes source into a validated intermediate representation. Logical component and
net identities remain stable across recompilation. Previewed board items use stable RFC 9562 UUIDv8
identities derived from the language namespace, project name, item kind, and authored logical ID;
formatting and statement order do not affect those identities.

Physical board quantities always carry explicit units (`mm`, `mil`, `um`, `nm`, or `in`), area
thresholds use the corresponding square units (`mm2`, `mil2`, `um2`, `nm2`, or `in2`), and
rotations carry `deg`.  Generated items such as outlines, traces, arcs, vias, and zones require logical
`id` fields.  Those IDs are stable across formatting and statement reordering and are the source
identity used by transactional backends; component placement uses the already-unique reference.

### Copper zone form

The canonical KDS version 1 copper-zone form is:

```scheme
(zone NET
  (id LOGICAL_ID)
  (name "OPTIONAL DISPLAY NAME")
  (layers F.Cu In1.Cu B.Cu)
  (outline
    (polygon
      (point X Y) (point X Y) (point X Y)
      (hole (point X Y) (point X Y) (point X Y)))
    (polygon ...))
  (clearance DISTANCE)
  (min_thickness DISTANCE)
  (connection none|solid)
  ; or: (connection thermal|pth_thermal
  ;       (thermal_gap DISTANCE) (thermal_spoke_width DISTANCE))
  (islands remove_all|keep_all)
  ; or: (islands remove_below (minimum_area AREA))
  (fill solid)
  ; or: (fill hatched
  ;       (thickness DISTANCE) (gap DISTANCE) (orientation ANGLE)
  ;       (smoothing 0..1) (hole_min_area_ratio 0..1) (border minimum|hatch))
  ; optional for hatched fill:
  ; (hatch_offsets (layer F.Cu X Y) (layer B.Cu X Y))
  (priority UNSIGNED_INTEGER)
  (border solid|invisible)
  ; or: (border diagonal_full|diagonal_edge (pitch DISTANCE))
  (locked true|false))
```

`id`, `layers`, `outline`, `clearance`, `min_thickness`, `connection`, `islands`, and `fill`
are required. `name` defaults to the logical ID, `priority` to zero, `border` to solid, and
`locked` to false. Polygon closure is implicit, so the first point must not be repeated. Every
outer line and hole needs at least three unique non-collinear points and may not self-intersect.
Holes must be strictly inside and mutually disjoint; multiple outer polygons must also be disjoint.
A zone is bounded to 32 polygons, 64 holes per polygon, and 8192 total points; layers must be
distinct and present in the declared stackup. KDS validates these constraints before any editor
connection or mutation. Hatched zones may additionally define one bounded offset per zone layer;
solid zones reject hatch offsets instead of retaining ignored intent.

## Compiler pipeline

1. Parse bounded, lossless s-expressions.
2. Type-check every executable form, required field, identifier, and reference.
3. Resolve libraries, symbols, footprints, models, components, pins, and nets.
4. Produce a deterministic IR, source digest, diagnostics, and mutation plan.
5. Establish the whole-project pre-apply snapshot.
6. Compile schematic and library artifacts through lossless edits validated by KiCad 10.
7. Compile live board state through the official KiCad 10 protobuf IPC transaction API.
8. Resolve and cache sourcing evidence with explicit remote-action permissions.
9. Run ERC, DRC, connectivity, sourcing, and manufacturability checks.
10. Generate and validate requested fabrication outputs.

Compilation and planning are read-only. Apply requires the exact previewed source SHA-256, the
pre-turn project snapshot, and the target board open in PCB Editor. Managed PCB ownership is
validated by recomputing every deterministic UUID from project, item kind, and KDS logical ID;
unmanaged collisions abort before a transaction begins. Existing managed items are updated,
missing ones are recreated, and only previously managed obsolete items are deleted in a single
KiCad transaction. A project-confined apply journal makes an interrupted operation safely
reconcilable on the next apply, while the whole turn remains revertible from local history.

The apply backend currently executes rectangular outlines, component placement, straight traces,
arcs, vias, and copper zones. A zone explicitly declares its net, stable ID, one or more copper
layers, bounded polygon/hole geometry, clearance, minimum thickness, connection and thermal policy,
island policy, solid or hatched fill, priority, border display, and lock state. No manufacturing
setting is inherited silently. Zone creation and updates are committed through KiCad 10 IPC, after
which KiCad's official refill operation is polled until every desired zone reports filled. Refill
failure retains the recovery journal and aborts the apply result rather than claiming success.

Placement requires exactly one live footprint with the KDS component reference and
an existing schematic symbol path. It updates only position, rotation, front/back side, and lock
state in place; footprint ownership, UUID, symbol path, fields, pads, and child UUIDs remain KiCad's
existing objects. Missing, duplicate, or board-only footprint references abort before mutation.
The backend still refuses stackup, text, dimensions, keepouts, or any structurally retained
form before mutation until that form has its own typed backend and rollback coverage.

Run `tools/smoke-kichad-kds-apply.sh --allow-mutation` for the opt-in live proof. The harness creates
an isolated temporary project, starts its own build-tree PCB Editor, applies five managed items, and
reapplies the unchanged source to verify updates reuse the same deterministic identities. It also
places a schematic-linked footprint on the back side and proves its footprint/symbol/pad identities
and flipped child layers survive both applies, and proves the fifth managed object is a filled
net-connected copper zone with exact physical settings.

## Production support rule

A form is documented as executable only after it has all of the following coverage:

- parser and type-checker unit tests, including malformed and bounded-input cases;
- deterministic source-to-IR tests;
- backend unit tests with injected failures and rollback assertions;
- round-trip tests proving untouched source and KiCad data remain unchanged;
- live KiCad integration tests against disposable project copies;
- relevant ERC, DRC, sourcing, and fabrication-output assertions.

The front-end currently validates the stable identities and fields for project metadata, libraries,
components, nets, sourcing, board statement kinds, checks, and outputs. It structurally normalizes
nested sheet, board, and rule payloads into IR and a pass plan; those nested payloads become
executable only as their backend-specific type checkers and rollback tests land. Native backend
execution is enabled incrementally, and apply refuses unsupported execution before mutation.
