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
    (stackup
      (finish "ENIG")
      (impedance_controlled false)
      (edge_connector none)
      (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
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

  (rules
    (minimum_clearance 0.2mm)
    (minimum_connection_width 0.15mm)
    (minimum_track_width 0.18mm)
    (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.6mm)
    (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.3mm)
    (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm)
    (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm)
    (minimum_groove_width 0.3mm)
    (minimum_resolved_spokes 3)
    (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance 0.5mm)
    (use_height_for_length_calculations true)
    (maximum_error 0.005mm)
    (allow_fillets_outside_zone_outline false))
  (net_classes
    (class Default
      (clearance 0.2mm) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile none) (pcb_color default)
      (wire_width 0.15mm) (bus_width 0.3mm)
      (schematic_color default) (line_style solid))
    (class LED_SIGNAL
      (clearance inherit) (track_width 0.25mm)
      (via_diameter inherit) (via_drill inherit)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color "#22AA44")
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))
    (assign (pattern LED_A) (classes LED_SIGNAL)))
  (custom_rules
    (rule led_clearance
      (condition "A.NetName == 'LED_A'")
      (layer F.Cu)
      (severity error)
      (constraint clearance (min 0.25mm))
      (constraint track_width (min 0.2mm) (opt 0.25mm) (max 0.5mm)))
    (rule dangling_vias
      (condition always)
      (layer all)
      (severity warning)
      (constraint via_dangling)))
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
rotations carry `deg`. Generated items such as outlines, traces, arcs, vias, zones, keepouts, and
board text require logical `id` fields. Those IDs are stable across formatting and statement
reordering and are the source identity used by transactional backends; component placement uses the
already-unique reference.

### Stackup form

KDS authors one explicit top-to-bottom physical stack. Copper-layer count and total board thickness
are derived from it; they are deliberately not accepted as separate fields that could disagree with
the authored layers.

```scheme
(stackup
  (finish "ENIG")
  (impedance_controlled true|false)
  (edge_connector none|yes|bevelled)
  (edge_plating true|false)
  (layers
    ; optional top technical layers, only in this order
    (silkscreen F.SilkS (material "Epoxy ink") (color "White"))
    (solderpaste F.Paste)
    (soldermask F.Mask (thickness 10um) (material "LPI")
      (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))

    (copper F.Cu (thickness 35um))
    (dielectric core (thickness 0.486mm) (material "FR408HR")
      (epsilon_r 3.68) (loss_tangent 0.0092) (locked true))
    (copper In1.Cu (thickness 18um))
    (dielectric prepreg (thickness 0.486mm) (material "FR408HR 2116")
      (epsilon_r 3.66) (loss_tangent 0.0092) (locked false))
    (copper In2.Cu (thickness 18um))
    (dielectric core (thickness 0.517mm) (material "FR408HR")
      (epsilon_r 3.68) (loss_tangent 0.0092) (locked true))
    (copper B.Cu (thickness 35um))

    ; optional bottom technical layers, only in this order
    (soldermask B.Mask (thickness 10um) (material "LPI")
      (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))
    (solderpaste B.Paste)
    (silkscreen B.SilkS (material "Epoxy ink") (color "White"))))
```

All four global fabrication policies and the `layers` declaration are required. A stack has 2
through 32 even copper layers ordered `F.Cu`, sequential `In1.Cu` through `In30.Cu`, then `B.Cu`,
with exactly one `core` or `prepreg` dielectric between adjacent copper layers. Physical board
layers cannot repeat. Copper and dielectric thickness, dielectric material, relative permittivity,
loss tangent, and thickness lock are explicit. Solder-mask physical properties and silkscreen
material/color are explicit when those optional layers are present. Total thickness is the sum of
copper, dielectric, and solder-mask thicknesses and must not exceed 20 mm. The former
`copper_layers`/`thickness` shorthand is invalid KDS: there is one stackup representation.

### Global board rules form

KDS authors KiCad's complete global Board Setup constraint set once, as a single top-level
declaration:

```scheme
(rules
  (minimum_clearance 0.2mm)
  (minimum_connection_width 0.15mm)
  (minimum_track_width 0.18mm)
  (minimum_via_annular_width 0.1mm)
  (minimum_via_diameter 0.6mm)
  (minimum_through_hole_diameter 0.3mm)
  (minimum_microvia_diameter 0.3mm)
  (minimum_microvia_drill 0.1mm)
  (minimum_hole_to_hole 0.25mm)
  (minimum_copper_to_hole_clearance 0.25mm)
  (minimum_silkscreen_clearance -0.01mm)
  (minimum_groove_width 0.3mm)
  (minimum_resolved_spokes 3)
  (minimum_silkscreen_text_height 0.8mm)
  (minimum_silkscreen_text_thickness 0.08mm)
  (minimum_copper_to_edge_clearance legacy)
  (use_height_for_length_calculations true)
  (maximum_error 0.005mm)
  (allow_fillets_outside_zone_outline false))
```

Every field is required when `rules` is present, so a design never inherits an accidental local
constraint. `minimum_copper_to_edge_clearance` accepts a non-negative physical distance or the
semantic value `legacy`; scripts never encode KiCad's internal negative sentinel. Via and microvia
diameters must be large enough for the declared drill plus twice the minimum annular width. Native
KiCad ranges are enforced before planning. `maximum_error` is the curve-to-segment approximation
tolerance, while `allow_fillets_outside_zone_outline` controls KiCad's external zone smoothing.
The former generic top-level `(rule NAME ...)` form is invalid; global constraints have this one
representation. Conditional custom rules are a separate KiCad concept and use the non-overlapping
`custom_rules` form below.

### Net classes form

KDS has one complete net-class table. The `Default` class is declared first; every other class
follows in descending precedence, so declaration order is the priority and there is no second
numeric-priority spelling. All classes precede assignments.

```scheme
(net_classes
  (class Default
    (clearance 0.2mm) (track_width 0.2mm)
    (via_diameter 0.6mm) (via_drill 0.3mm)
    (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
    (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
    (tuning_profile none) (pcb_color default)
    (wire_width 0.15mm) (bus_width 0.3mm)
    (schematic_color default) (line_style solid))
  (class USB_HS
    (clearance inherit) (track_width 0.15mm)
    (via_diameter inherit) (via_drill inherit)
    (microvia_diameter inherit) (microvia_drill inherit)
    (diff_pair_width 0.15mm) (diff_pair_gap 0.18mm) (diff_pair_via_gap inherit)
    (tuning_profile usb_hs) (pcb_color "#1A66CCDD")
    (wire_width inherit) (bus_width inherit)
    (schematic_color "#22AA44") (line_style dash_dot))
  (assign (pattern "USB_[PN]") (classes USB_HS))
  (assign (pattern "/usb/D[PN]") (classes USB_HS)))
```

All fifteen class fields are required so omissions cannot silently acquire machine-local defaults.
`Default` requires concrete distances, `default` colors, `none` or a named tuning profile, and an
explicit line style. Other classes use either a concrete value or `inherit`; colors use `inherit`
or `#RRGGBB`/`#RRGGBBAA`; line styles are `solid`, `dash`, `dot`, `dash_dot`, or `dash_dot_dot`.
Wire and bus widths must be exactly representable in KiCad schematic units (100 nm). Via and
microvia diameters cannot be smaller than their effective drills after inheritance.

Each `assign` contains one bounded KiCad net-name pattern and one or more declared non-Default
classes. Pattern/class pairs are unique. Bus ranges may contain at most 256 members per range and
the complete table may expand to at most 4096 native assignments. Class names are unique without
regard to case; exact spelling is used by assignments. The compiler limits the table to 256 classes
and 1024 authored pattern/class pairs.

Apply lowers this form to one typed `NetClassSettings` message. KiChad validates the complete table,
priorities, native ranges, colors, padstacks, assignments, and physical cross-constraints before
replacing any project setting. It journals the previous table and restores it on lost
acknowledgements or any later pre-commit failure. The older partial merge API is not used by KDS;
there is one source representation and one atomic replacement operation.

### Custom rules form

KDS authors the complete conditional custom-rule set once. There is no second KDS spelling and no
authored `.kicad_dru` companion. The compiler deterministically lowers this form to KiCad's native
rule document as an internal board artifact:

```scheme
(custom_rules
  (rule usb_diff_pair
    (condition "A.hasNetclass('USB_HS')")
    (layer outer)
    (severity error)
    (constraint diff_pair_gap (min 0.15mm) (opt 0.2mm) (max 0.25mm))
    (constraint skew (domain diff_pairs) (max 5ps)))
  (rule assembly_policy
    (condition always)
    (layer all)
    (severity warning)
    (constraint disallow (items track through_via pad footprint))
    (constraint assertion
      (test "A.Type != 'Footprint' || A.Orientation != 13deg"))))
```

Every rule uses exactly this clause order: name, `condition`, `layer`, `severity`, then one or more
constraints. Conditions are either `always` or one quoted native KiCad rule expression. Layers are
`all`, `outer`, `inner`, or one bounded native layer/pattern such as `F.Cu` or `In*.Cu`.
Severities are `ignore`, `warning`, `error`, or `exclusion`. Rule names are unique, a rule contains
each constraint type at most once, and source containing an unresolved `${...}` expression is
rejected. The native KiCad parser and expression compiler validate layer patterns and conditions
again before the active board can change.

KDS exposes all 35 KiCad 10 custom constraint types through structured values:

- Bare flags: `(constraint via_dangling)` and `(constraint bridged_mask)`.
- Assertion: `(constraint assertion (test "EXPRESSION"))`.
- Zone connection: `(constraint zone_connection (style solid|thermal_reliefs|none))`.
- Prohibition: `(constraint disallow (items ...))`, with unique items in canonical order:
  `track`, `via`, `through_via`, `blind_via`, `buried_via`, `micro_via`, `pad`, `zone`, `text`,
  `graphic`, `hole`, `footprint`. The aggregate `via` cannot be combined with individual via types.
- Counts and ratios: `(constraint min_resolved_spokes (count 0..4))` and
  `(constraint solder_paste_rel_margin (ratio -10..10))`; paste ratios have exact 0.001
  resolution, so `-0.1` means a ten-percent reduction.
- Distance ranges: `annular_width`, `clearance`, `creepage`, `connection_width`,
  `courtyard_clearance`, `diff_pair_gap`, `diff_pair_uncoupled`, `edge_clearance`,
  `hole_clearance`, `hole_size`, `hole_to_hole`, `physical_clearance`,
  `physical_hole_clearance`, `silk_clearance`, `solder_mask_expansion`,
  `solder_mask_sliver`, `solder_paste_abs_margin`, `text_height`, `text_thickness`,
  `thermal_relief_gap`, `thermal_spoke_width`, `track_width`, `track_segment_length`, and
  `via_diameter`.
- Typed ranges: `track_angle` uses `deg`; `via_count` uses non-negative integers; `length` and
  `skew` use either distances or `fs`/`ps` time values. A `skew` also requires
  `(domain nets|diff_pairs)`.

Range values use the supported `(min VALUE)`, `(opt VALUE)`, and `(max VALUE)` fields for that
native constraint, occurring once in that order, with `min <= opt <= max`. Required native fields
are enforced—for example, `clearance` requires `min`, `diff_pair_uncoupled` requires `max`, and
paste/mask optimal-margin constraints require `opt`. A single range cannot mix distance and time.
Values are bounded to KiCad's exact numeric domains before planning.

The compiler emits one deterministic native document, normalizing physical distances losslessly to
decimal millimetres, time to femtoseconds, and angles to the native unitless degree field. The PCB
Editor endpoint limits the artifact to 1 MiB, 512 rules, 64 constraints per rule, and 4096 total
constraints. It validates UTF-8, parses and compiles the complete document, installs it atomically,
reloads the live DRC engine, and reads it back byte-for-byte. Failure restores both the previous
file and previous engine rules. KDS journals that exact prior state and restores it on a lost
acknowledgement or any later pre-commit failure.

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

### Keepout form

The canonical KDS version 1 keepout form is:

```scheme
(keepout
  (id LOGICAL_ID)
  (name "OPTIONAL DISPLAY NAME")
  (layers F.Cu In1.Cu B.Cu)
  (outline
    (polygon
      (point X Y) (point X Y) (point X Y)
      (hole (point X Y) (point X Y) (point X Y))))
  (prohibit
    (copper true|false)
    (vias true|false)
    (tracks true|false)
    (pads true|false)
    (footprints true|false))
  (border solid|invisible)
  ; or: (border diagonal_full|diagonal_edge (pitch DISTANCE))
  (locked true|false))
```

`id`, `layers`, `outline`, and `prohibit` are required. Every prohibited category is explicit and
at least one must be true, so an ineffective rule area is rejected. `name` defaults to the logical
ID, `border` to solid, and `locked` to false. Keepouts use distinct copper layers present in the
declared stackup and the same bounded, exact polygon topology contract as copper zones. They lower
to KiCad's native `ZT_RULE_AREA` with placement-area behavior explicitly disabled; they are not
copper zones and are never awaited as filled objects.

### Board text form

The canonical KDS version 1 board text form is:

```scheme
(text "CONTENT"
  (id LOGICAL_ID)
  (layer F.SilkS)
  (at X Y)
  (size WIDTH HEIGHT)
  (stroke WIDTH)
  (angle ANGLE)
  (justify left|center|right top|center|bottom)
  (font stroke|"INSTALLED FONT NAME")
  (line_spacing RATIO)
  (bold true|false)
  (italic true|false)
  (underlined true|false)
  (mirrored true|false)
  (keep_upright true|false)
  (hyperlink "OPTIONAL URI")
  (knockout true|false)
  (locked true|false))
```

`id`, `layer`, `at`, `size`, and `stroke` are required. Text is bounded to 64 KiB and uses LF line
endings; whether it is multiline is derived from the content instead of represented twice. Size is
bounded to KiCad's native 1 um through 250 mm range, stroke is capped at one quarter of the smaller
text dimension, and coordinates must fit KiCad's internal board coordinate range. Angle defaults to
0 degrees, justification to centered, font to KiCad's portable stroke font, line spacing to 1, and
all style, knockout, and lock booleans to false. Named fonts are preserved exactly and must be
installed anywhere the design is rendered. All normal KiCad copper, technical, fabrication, and
user board layers are accepted; copper layers are checked against the declared stackup and the
target board must have the authored layer enabled.

### Dimension form

KDS has one canonical dimension form whose style selects one of KiCad's five native dimension
geometries:

```scheme
(dimension aligned|orthogonal|radial|leader|center
  (id LOGICAL_ID)
  (layer Dwgs.User)
  ; aligned/orthogonal: (from X Y) (to X Y) (height SIGNED_DISTANCE)
  ;                     (extension_height DISTANCE), plus (axis x|y) for orthogonal
  ; radial:             (center X Y) (radius_point X Y) (leader_length DISTANCE)
  ; leader:             (from X Y) (to X Y)
  ;                     (border none|rectangle|circle|roundrect) (label "TEXT")
  ; center:             (center X Y) (to X Y)
  ; aligned, orthogonal, and radial measurement policy:
  (units mm|mil|in|automatic)
  (unit_format no_suffix|bare_suffix|paren_suffix)
  (precision fixed_0|fixed_1|fixed_2|fixed_3|fixed_4|fixed_5|
             scaled_in_2|scaled_in_3|scaled_in_4|scaled_in_5)
  (suppress_trailing_zeroes true|false)
  (prefix "TEXT") (suffix "TEXT") (override "TEXT")
  ; aligned and orthogonal layout policy:
  (arrow_direction inward|outward)
  (text_position outside|inline|manual)
  ; also accepted by radial dimensions:
  (keep_text_aligned true|false)
  ; non-center text appearance:
  (text_at X Y)
  (text_size WIDTH HEIGHT)
  (text_stroke WIDTH)
  (text_angle ANGLE)
  (text_justify left|center|right top|center|bottom)
  (font stroke|"INSTALLED FONT NAME")
  (bold true|false) (italic true|false)
  (underlined true|false) (mirrored true|false)
  ; common physical policy:
  (line_width DISTANCE)
  (arrow_length DISTANCE)
  ; aligned, orthogonal, and leader only:
  (extension_offset DISTANCE)
  (locked true|false))
```

`id`, `layer`, `line_width`, and `arrow_length` are required for every style. Geometry fields are
required exactly as shown and endpoints must differ. Measurement fields are required for aligned,
orthogonal, and radial dimensions; radial dimensions default to the conventional `R ` prefix.
`text_size` and `text_stroke` are required for every style except center marks. `text_at` is required
for radial and leader dimensions and whenever aligned or orthogonal text is manual; it is rejected
when automatic positioning would ignore it. An explicit text angle is likewise rejected while
`keep_text_aligned` is true. Leader labels compile to KiCad's native override text, while center
marks reject all text and measurement fields. Fields that do not apply to the selected style are
errors instead of silently retaining intent. The compiler lowers every style into the same native
KiCad `Dimension` protobuf type and uses the authored style as its single geometry oneof.

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

The apply backend currently executes physical board stackups, the complete global Board Setup
constraint set, complete net-class tables, all 35 conditional custom-rule types, rectangular
outlines, component placement, straight traces, arcs, vias, copper zones, keepout rule areas,
native board text, and all five native dimension styles. Stackup apply
uses KiCad's native protobuf stackup message and editor
endpoint; it validates the entire ordered structure before mutation, derives enabled physical
layers and board thickness, and preserves rather than deletes objects when technical layers are
disabled. Removing a non-empty copper layer is rejected. The pre-apply stack is retained in the
apply journal and restored if a later settings or transactional item mutation fails. Global rules
use a typed native protobuf endpoint, validate every field and physical cross-constraint before
mutation, and are journaled and restored together with the stackup on any pre-commit failure. A
complete net-class table uses a typed native project endpoint, is persisted with the project, and
is journaled and restored after the global rules. Custom rules use a bounded native board endpoint,
are compiled by KiCad's real DRC parser and engine, and are journaled and restored after net classes
and before transactional PCB items. A zone explicitly declares
its net, stable ID, one or more copper layers, bounded
polygon/hole geometry, clearance, minimum thickness, connection and thermal policy, island policy,
solid or hatched fill, priority, border display, and lock state. No manufacturing setting is
inherited silently. Zone creation and updates are committed through KiCad 10 IPC, after which
KiCad's official refill operation is polled until every desired zone reports filled. Refill failure
retains the recovery journal and aborts the apply result rather than claiming success.
Keepouts use a separate deterministic ownership type and exact `rule_area_settings` update mask, so
their unfilled state is never confused with a failed copper refill.

Placement requires exactly one live footprint with the KDS component reference and
an existing schematic symbol path. It updates only position, rotation, front/back side, and lock
state in place; footprint ownership, UUID, symbol path, fields, pads, and child UUIDs remain KiCad's
existing objects. Missing, duplicate, or board-only footprint references abort before mutation.
Any other structurally retained form is refused before mutation until it has its own typed backend
and rollback coverage.

Run `tools/smoke-kichad-kds-apply.sh --allow-mutation` for the opt-in live proof. The harness creates
an isolated temporary project, starts its own build-tree PCB Editor, applies one lossless physical
stackup plus twelve managed items, and reapplies the unchanged source to verify updates reuse the
same deterministic identities. It reads the stackup back from the live editor and verifies finish,
impedance, bevel, plating, all nine ordered physical layers, thicknesses, material, color, loss,
permittivity, and dielectric lock. It also reads back all global constraint fields, including the
semantic legacy edge-clearance mode, and verifies the second apply updates the same settings. It
reads back the complete net-class table and assignments, including inherited fields, native
schematic units, colors, line styles, via/microvia padstacks, and priorities; an invalid replacement
must leave that table unchanged. It also reads back the exact generated custom-rule document,
reapplies it without duplication, and proves malformed native input leaves the active document
unchanged. It then
places a schematic-linked footprint on the back side and proves its footprint/symbol/pad identities
and flipped child layers survive both applies. It proves the fifth managed object is a filled
net-connected copper zone with exact physical settings and the sixth is a distinct unfilled, locked
keepout with exact prohibited-item policy. The seventh is multiline native board text with exact
position, layer, typography, hyperlink, and lock state. The remaining five are aligned, orthogonal,
radial, leader, and center dimensions with exact native geometry, units, precision, layout policy,
labels, and text placement. Reapply groups updates by exact field mask so each dimension oneof is
updated independently.

## Production support rule

A form is documented as executable only after it has all of the following coverage:

- parser and type-checker unit tests, including malformed and bounded-input cases;
- deterministic source-to-IR tests;
- backend unit tests with injected failures and rollback assertions;
- round-trip tests proving untouched source and KiCad data remain unchanged;
- live KiCad integration tests against disposable project copies;
- relevant ERC, DRC, sourcing, and fabrication-output assertions.

The front-end currently validates the stable identities and fields for project metadata, libraries,
components, nets, sourcing, board statement kinds, global rules, net classes, custom rules, checks,
and outputs. Global rules, net classes, custom rules, and executable board forms have backend-specific type checking and
rollback coverage. Nested sheet
and library payloads remain non-executable until their own typed backends and rollback tests land.
Native backend
execution is enabled incrementally, and apply refuses unsupported execution before mutation.
