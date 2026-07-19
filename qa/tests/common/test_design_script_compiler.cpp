/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <boost/test/unit_test.hpp>

#include <limits>

#include <kicad/codex/design_script_compiler.h>


namespace
{

const std::string VALID_PROGRAM = R"KDS((kichad_design
  (version 1)
  (project sensor_node
    (title "Production Sensor Node")
    (company "KiChad QA")
    (revision "A"))
  (units mm)
  (library symbol Device (uri "${KICAD10_SYMBOL_DIR}/Device.kicad_sym"))
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
  (sheet root (title "Main"))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (outline (rect (id board-edge) (at 0mm 0mm) (size 40mm 30mm)))
    (place R1 (at 10mm 10mm) (rotation 0deg) (side front))
    (route LED_A (id led-a-trace) (from 10mm 10mm) (to 20mm 10mm)
      (width 0.25mm) (layer F.Cu))
    (route LED_A (id led-a-arc) (from 20mm 10mm) (mid 22mm 12mm) (to 24mm 10mm)
      (width 0.25mm) (layer F.Cu))
    (via LED_A (id led-a-via) (at 24mm 10mm) (diameter 0.8mm) (drill 0.4mm))
    (zone LED_A
      (id led-a-pour)
      (name "LED copper pour")
      (layers F.Cu)
      (outline
        (polygon
          (point 1mm 1mm) (point 39mm 1mm) (point 39mm 29mm) (point 1mm 29mm)
          (hole
            (point 10mm 10mm) (point 12mm 10mm) (point 12mm 12mm) (point 10mm 12mm))))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection thermal (thermal_gap 0.3mm) (thermal_spoke_width 0.35mm))
      (islands remove_below (minimum_area 1mm2))
      (fill hatched (thickness 0.3mm) (gap 0.4mm) (orientation 45deg)
        (smoothing 0.25) (hole_min_area_ratio 0.1) (border hatch))
      (hatch_offsets (layer F.Cu 0.1mm -0.2mm))
      (priority 3)
      (border diagonal_edge (pitch 0.5mm))
      (locked true)))
  (rules
    (minimum_clearance 0.2mm)
    (minimum_connection_width 0.2mm)
    (minimum_track_width 0.2mm)
    (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.6mm)
    (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.3mm)
    (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm)
    (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm)
    (minimum_groove_width 0.25mm)
    (minimum_resolved_spokes 2)
    (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance 0.5mm)
    (use_height_for_length_calculations true)
    (maximum_error 0.005mm)
    (allow_fillets_outside_zone_outline false))
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
)KDS";

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptCompiler )


BOOST_AUTO_TEST_CASE( CompilesEveryDesignFacetIntoDeterministicValidatedIr )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT first =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( VALID_PROGRAM );
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT second =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( VALID_PROGRAM );

    BOOST_REQUIRE_MESSAGE( first.ok, first.diagnostics.dump() );
    BOOST_REQUIRE( second.ok );
    BOOST_CHECK_EQUAL( first.sourceSha256, second.sourceSha256 );
    BOOST_CHECK_EQUAL( first.ir.dump(), second.ir.dump() );
    BOOST_CHECK_EQUAL( first.ir["project"]["name"].get<std::string>(), "sensor_node" );
    BOOST_CHECK_EQUAL( first.ir["schematic"]["components"].size(), 2 );
    BOOST_CHECK_EQUAL( first.ir["schematic"]["nets"].size(), 1 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["pinConnections"].get<size_t>(), 2 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["boardStatements"].get<size_t>(), 7 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["rules"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( first.ir["rules"]["minimumClearanceNm"].get<int64_t>(), 200000 );
    BOOST_CHECK_EQUAL( first.ir["rules"]["minimumResolvedSpokes"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( first.ir["rules"]["copperEdgeClearanceMode"].get<std::string>(),
                       "explicit" );
    BOOST_CHECK( first.ir["rules"]["useHeightForLengthCalculations"].get<bool>() );
    BOOST_CHECK_EQUAL( first.ir["rules"]["maximumErrorNm"].get<int64_t>(), 5000 );
    BOOST_CHECK( !first.ir["rules"]["allowFilletsOutsideZoneOutline"].get<bool>() );
    BOOST_CHECK( first.plan["boardFullyTyped"].get<bool>() );
    BOOST_CHECK_EQUAL( first.ir["pcb"][1]["logicalId"].get<std::string>(), "board-edge" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][3]["kind"].get<std::string>(), "trace" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][3]["widthNm"].get<int64_t>(), 250000 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][4]["kind"].get<std::string>(), "arc" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][5]["drillNm"].get<int64_t>(), 400000 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["kind"].get<std::string>(), "zone" );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["polygons"][0]["holes"].size(), 1 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["islands"]["minimumAreaNm2"].get<int64_t>(),
                       1000000000000LL );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["fill"]["orientationDegrees"].get<double>(),
                       45.0 );
    BOOST_REQUIRE_EQUAL( first.ir["pcb"][6]["layerProperties"].size(), 1 );
    BOOST_CHECK_EQUAL( first.ir["pcb"][6]["layerProperties"][0]["layer"].get<std::string>(),
                       "F.Cu" );
    BOOST_CHECK_EQUAL(
            first.ir["pcb"][6]["layerProperties"][0]["offset"]["yNm"].get<int64_t>(),
            -200000 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["sourcingRecords"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["checks"].get<size_t>(), 3 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["outputs"].get<size_t>(), 3 );
    BOOST_CHECK( first.plan["transactional"].get<bool>() );
}


BOOST_AUTO_TEST_CASE( CompilesOneExplicitLosslessStackupRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controlled_stackup)
  (board
    (stackup
      (finish "ENIG")
      (impedance_controlled true)
      (edge_connector bevelled)
      (edge_plating true)
      (layers
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
        (soldermask B.Mask (thickness 10um) (material "LPI")
          (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))
        (solderpaste B.Paste)
        (silkscreen B.SilkS (material "Epoxy ink") (color "White"))))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 1 );
    const nlohmann::json& stackup = compiled.ir["pcb"][0];
    BOOST_CHECK_EQUAL( stackup["kind"].get<std::string>(), "stackup" );
    BOOST_CHECK_EQUAL( stackup["finish"].get<std::string>(), "ENIG" );
    BOOST_CHECK( stackup["impedanceControlled"].get<bool>() );
    BOOST_CHECK_EQUAL( stackup["edgeConnector"].get<std::string>(), "bevelled" );
    BOOST_CHECK( stackup["edgePlating"].get<bool>() );
    BOOST_CHECK_EQUAL( stackup["copperLayers"].get<int>(), 4 );
    BOOST_CHECK_EQUAL( stackup["thicknessNm"].get<int64_t>(), 1615000 );
    BOOST_REQUIRE_EQUAL( stackup["layers"].size(), 13 );
    BOOST_CHECK_EQUAL( stackup["layers"][0]["category"].get<std::string>(), "silkscreen" );
    BOOST_CHECK_EQUAL( stackup["layers"][3]["layer"].get<std::string>(), "F.Cu" );
    BOOST_CHECK_EQUAL( stackup["layers"][4]["typeName"].get<std::string>(), "core" );
    BOOST_CHECK_EQUAL( stackup["layers"][4]["dielectricIndex"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( stackup["layers"][5]["layer"].get<std::string>(), "In1.Cu" );
    BOOST_CHECK_EQUAL( stackup["layers"][8]["dielectricIndex"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( stackup["layers"][9]["layer"].get<std::string>(), "B.Cu" );
    BOOST_CHECK_EQUAL( stackup["layers"][12]["color"].get<std::string>(), "White" );
}


BOOST_AUTO_TEST_CASE( CompilesOneCanonicalCompleteNetclassRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controlled_netclasses)
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
      (microvia_diameter 0.25mm) (microvia_drill inherit)
      (diff_pair_width 0.15mm) (diff_pair_gap 0.18mm) (diff_pair_via_gap inherit)
      (tuning_profile usb_hs) (pcb_color "#112233CC")
      (wire_width inherit) (bus_width inherit)
      (schematic_color "#AABBCC") (line_style dash_dot))
    (class POWER
      (clearance 0.3mm) (track_width 0.5mm)
      (via_diameter 0.8mm) (via_drill 0.4mm)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width 0.2mm) (bus_width 0.4mm)
      (schematic_color inherit) (line_style inherit))
    (assign (pattern "USB_[PN]") (classes USB_HS))
    (assign (pattern "/power/VBUS[0..3]") (classes POWER USB_HS))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE( compiled.ir["netClasses"].is_object() );
    const nlohmann::json& netClasses = compiled.ir["netClasses"];
    BOOST_REQUIRE_EQUAL( netClasses["classes"].size(), 3 );
    BOOST_REQUIRE_EQUAL( netClasses["assignments"].size(), 2 );
    BOOST_CHECK_EQUAL( netClasses["classes"][0]["name"].get<std::string>(), "Default" );
    BOOST_CHECK_EQUAL( netClasses["classes"][0]["priority"].get<int64_t>(),
                       std::numeric_limits<int32_t>::max() );
    BOOST_CHECK_EQUAL( netClasses["classes"][1]["priority"].get<int64_t>(), 0 );
    BOOST_CHECK_EQUAL( netClasses["classes"][2]["priority"].get<int64_t>(), 1 );
    BOOST_CHECK( netClasses["classes"][1]["clearanceNm"].is_null() );
    BOOST_CHECK_EQUAL( netClasses["classes"][1]["trackWidthNm"].get<int64_t>(), 150000 );
    BOOST_CHECK_CLOSE( netClasses["classes"][1]["pcbColor"]["a"].get<double>(),
                       204.0 / 255.0, 0.0001 );
    BOOST_CHECK_EQUAL( netClasses["classes"][1]["lineStyle"].get<std::string>(),
                       "dash_dot" );
    BOOST_CHECK_EQUAL( netClasses["assignments"][1]["classes"].size(), 2 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["netClasses"].get<size_t>(), 3 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["netClassAssignments"].get<size_t>(), 2 );
}


BOOST_AUTO_TEST_CASE( CompilesEveryNativeCustomRuleConstraintFromOneKdsRepresentation )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project custom_rule_design)
  (custom_rules
    (rule production_constraints
      (condition always)
      (layer all)
      (severity error)
      (constraint assertion (test "A.Type != 'Footprint' || A.Orientation != 13deg"))
      (constraint clearance (min -0.01mm))
      (constraint creepage (min 1mm))
      (constraint hole_clearance (min 0.2mm))
      (constraint edge_clearance (min 0.3mm))
      (constraint hole_size (min 0.2mm) (opt 0.3mm) (max 1mm))
      (constraint hole_to_hole (min 0.25mm))
      (constraint courtyard_clearance (min 0.1mm))
      (constraint silk_clearance (min -0.05mm))
      (constraint text_height (min 0.8mm) (max 3mm))
      (constraint text_thickness (min 0.08mm) (max 0.5mm))
      (constraint track_width (min 0.15mm) (opt 0.2mm) (max 2mm))
      (constraint track_angle (min 45deg) (max 135deg))
      (constraint track_segment_length (min 0.1mm) (max 100mm))
      (constraint connection_width (min 0.15mm))
      (constraint annular_width (min 0.1mm) (max 0.5mm))
      (constraint via_diameter (min 0.4mm) (opt 0.6mm) (max 2mm))
      (constraint via_dangling)
      (constraint zone_connection (style thermal_reliefs))
      (constraint thermal_relief_gap (min 0.2mm))
      (constraint thermal_spoke_width (opt 0.3mm))
      (constraint min_resolved_spokes (count 2))
      (constraint solder_mask_expansion (opt 0.05mm))
      (constraint solder_mask_sliver (min 0.08mm))
      (constraint solder_paste_abs_margin (opt -0.03mm))
      (constraint solder_paste_rel_margin (ratio -0.1))
      (constraint disallow (items track through_via blind_via buried_via micro_via pad zone
        text graphic hole footprint))
      (constraint length (min 10ps) (opt 12ps) (max 14ps))
      (constraint skew (domain diff_pairs) (min -5ps) (opt 0ps) (max 5ps))
      (constraint via_count (min 0) (max 20))
      (constraint diff_pair_gap (min 0.15mm) (opt 0.2mm) (max 0.3mm))
      (constraint diff_pair_uncoupled (max 5mm))
      (constraint physical_clearance (min 0.2mm))
      (constraint physical_hole_clearance (min 0.25mm))
      (constraint bridged_mask))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE( compiled.ir["customRules"].is_object() );
    BOOST_REQUIRE_EQUAL( compiled.ir["customRules"]["rules"].size(), 1 );
    const nlohmann::json& rule = compiled.ir["customRules"]["rules"][0];
    BOOST_CHECK_EQUAL( rule["name"].get<std::string>(), "production_constraints" );
    BOOST_CHECK_EQUAL( rule["condition"].get<std::string>(), "always" );
    BOOST_CHECK_EQUAL( rule["layer"].get<std::string>(), "all" );
    BOOST_CHECK_EQUAL( rule["severity"].get<std::string>(), "error" );
    BOOST_CHECK_EQUAL( rule["constraints"].size(), 35 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["customRules"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( rule["constraints"][1]["values"]["min"]["value"].get<int64_t>(),
                       -10000 );
    BOOST_CHECK_EQUAL( rule["constraints"][27]["values"]["min"]["domain"].get<std::string>(),
                       "time" );
    BOOST_CHECK_EQUAL( rule["constraints"][28]["domain"].get<std::string>(), "diff_pairs" );
    BOOST_CHECK_EQUAL( rule["constraints"][25]["valuePermille"].get<int64_t>(), -100 );
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousOrUnsafeCustomRulePrograms )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project invalid_custom_rules)
  (custom_rules
    (rule duplicate
      (condition "${UNRESOLVED}")
      (layer all)
      (severity error)
      (constraint clearance (min 0.3mm) (min 0.2mm))
      (constraint clearance (min 0.1mm))
      (constraint track_angle (min 135deg) (max 45deg))
      (constraint skew (min 1mm) (max 2ps))
      (constraint disallow (items via through_via)))
    (rule duplicate
      (condition always)
      (severity warning)
      (layer F.Cu)
      (constraint solder_paste_rel_margin (ratio inf)))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();

    for( const char* code : { "invalid_custom_rule_condition",
                              "noncanonical_custom_constraint_values",
                              "unordered_custom_constraint_range",
                              "duplicate_custom_constraint",
                              "invalid_custom_skew_domain",
                              "mixed_custom_constraint_domains",
                              "redundant_custom_disallow", "duplicate_custom_rule",
                              "invalid_custom_rule_layer", "invalid_custom_rule_severity",
                              "invalid_custom_paste_ratio" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousUnsafeOrNonNativeNetclasses )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project invalid_netclasses)
  (net_classes
    (class Fast
      (clearance inherit) (track_width inherit)
      (via_diameter 0.2mm) (via_drill 0.3mm)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))
    (class Default
      (clearance inherit) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.1mm) (microvia_drill 0.2mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width 0.00015mm) (bus_width 0.3mm)
      (schematic_color inherit) (line_style inherit))
    (assign (pattern "BUS[0..999]") (classes Missing Fast Fast))
    (class fast
      (clearance inherit) (track_width inherit)
      (via_diameter inherit) (via_drill inherit)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();

    for( const char* code : { "invalid_default_netclass", "duplicate_netclass",
                              "noncanonical_netclass_order",
                              "invalid_default_netclass_inheritance",
                              "invalid_netclass_distance", "invalid_netclass_color",
                              "invalid_netclass_tuning_profile", "invalid_netclass_pattern",
                              "unknown_netclass_assignment",
                              "duplicate_netclass_assignment" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }

    const std::string geometrySource = R"KDS((kichad_design
  (version 1)
  (project invalid_netclass_geometry)
  (net_classes
    (class Default
      (clearance 0.2mm) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile none) (pcb_color default)
      (wire_width 0.15mm) (bus_width 0.3mm)
      (schematic_color default) (line_style solid))
    (class Broken
      (clearance inherit) (track_width inherit)
      (via_diameter 0.2mm) (via_drill 0.3mm)
      (microvia_diameter 0.1mm) (microvia_drill 0.2mm)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color inherit)
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT geometry =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( geometrySource );
    BOOST_CHECK( !geometry.ok );
    BOOST_CHECK_NE( geometry.diagnostics.dump().find( "inconsistent_netclass_via_geometry" ),
                    std::string::npos );
    BOOST_CHECK_NE(
            geometry.diagnostics.dump().find( "inconsistent_netclass_microvia_geometry" ),
            std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsLegacyDuplicateAndPhysicallyInconsistentGlobalRules )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project invalid_rules)
  (rule legacy_clearance (minimum 0.2mm))
  (rules
    (minimum_clearance 0.2mm)
    (minimum_clearance 0.3mm)
    (minimum_connection_width 0.2mm)
    (minimum_track_width 0.2mm)
    (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.4mm)
    (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.2mm)
    (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm)
    (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm)
    (minimum_groove_width 0.25mm)
    (minimum_resolved_spokes 100)
    (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance -0.01mm)
    (use_height_for_length_calculations true)
    (maximum_error 0.0005mm)
    (allow_fillets_outside_zone_outline maybe)
    (mystery_constraint 1mm))))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();

    for( const char* code : { "unknown_top_level_form", "duplicate_rules_field",
                              "unknown_rules_field", "invalid_rules_resolved_spokes",
                              "invalid_rules_edge_clearance", "inconsistent_rules_via_geometry",
                              "inconsistent_rules_microvia_geometry", "invalid_rules_distance",
                              "invalid_rules_zone_fillet_policy" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsLegacyOrStructurallyAmbiguousStackups )
{
    const std::string legacy = R"KDS((kichad_design
  (version 1)
  (project legacy_stackup)
  (board (stackup (copper_layers 4) (thickness 1.6mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT legacyResult =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( legacy );
    BOOST_CHECK( !legacyResult.ok );
    BOOST_CHECK_NE( legacyResult.diagnostics.dump().find( "unknown_board_field" ),
                    std::string::npos );
    BOOST_CHECK_NE( legacyResult.diagnostics.dump().find( "invalid_stackup_layers" ),
                    std::string::npos );

    const std::string ambiguous = R"KDS((kichad_design
  (version 1)
  (project ambiguous_stackup)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector yes) (edge_plating false)
      (layers
        (soldermask F.Mask (thickness 0mm) (material "LPI")
          (epsilon_r 0.5) (loss_tangent 2) (color "Green"))
        (silkscreen F.SilkS (material "bad\nmaterial") (color "White"))
        (copper F.Cu (thickness 35um))
        (copper In2.Cu (thickness 35um))
        (dielectric foam (thickness 11mm) (material "")
          (epsilon_r 101) (loss_tangent -0.1) (locked maybe))
        (copper In1.Cu (thickness 35um))
        (dielectric core (thickness 11mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))
        (silkscreen B.SilkS (material "Ink") (color "White"))
        (soldermask B.Mask (thickness 10um) (material "LPI")
          (epsilon_r 3.5) (loss_tangent 0.02) (color "Green"))))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( ambiguous );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "invalid_stackup_order", "invalid_stackup_layers",
                              "invalid_stackup_dielectric_type",
                              "invalid_stackup_layer_thickness", "invalid_stackup_material",
                              "invalid_stackup_epsilon_r", "invalid_stackup_loss_tangent",
                              "invalid_stackup_dielectric_lock", "invalid_stackup_thickness" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsMalformedPhysicalBoardIntent )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project physical_errors)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net POWER (pin R1 1) (pin R2 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled maybe)
      (edge_connector tapered) (edge_plating maybe)
      (layers
        (copper F.Cu (thickness -1mm))
        (dielectric core (thickness 11mm) (material "")
          (epsilon_r 0.5) (loss_tangent 2) (locked maybe))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 11mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (outline (rect (id same) (at 0 0mm) (size -1mm 2mm)))
    (route POWER (id same) (from 0mm 0mm) (to 0mm 0mm)
      (width 0mm) (layer Edge.Cuts))
    (via POWER (id via1) (at 1mm 1mm) (diameter 0.4mm) (drill 0.5mm)
      (layers F.Cu In1.Cu) (type through))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "invalid_stackup_layers", "invalid_stackup_thickness",
                              "invalid_outline_position", "invalid_outline_size",
                              "duplicate_board_id", "zero_length_route", "invalid_route_width",
                              "invalid_route_layer", "invalid_via_drill",
                              "invalid_through_via_layers" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( EnforcesStackupAndSinglePlacementSemantics )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project stackup_errors)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net POWER (pin R1 1) (pin R2 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (place R1 (at 1mm 1mm))
    (place R1 (at 2mm 2mm))
    (route POWER (id outside-route) (from 0mm 0mm) (to 1mm 1mm)
      (width 0.2mm) (layer In3.Cu))
    (route POWER (id overflow-route) (from 9223372036854775808nm 0mm) (to 1mm 1mm)
      (width 0.2mm) (layer F.Cu))
    (via POWER (id blind) (at 1mm 1mm) (diameter 0.8mm) (drill 0.4mm)
      (layers F.Cu B.Cu) (type blind))
    (via POWER (id buried) (at 2mm 2mm) (diameter 0.8mm) (drill 0.4mm)
      (layers F.Cu In1.Cu) (type buried))
    (via POWER (id micro) (at 3mm 3mm) (diameter 0.4mm) (drill 0.2mm)
      (layers F.Cu In2.Cu) (type micro))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "duplicate_board_placement", "route_layer_outside_stackup",
                              "invalid_route_start", "invalid_blind_via_span",
                              "invalid_buried_via_span", "invalid_microvia_span" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousOrUnsafeCopperZoneIntent )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project bad_zone)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1) (pin R2 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (zone GND
      (id gnd-pour)
      (layers F.Cu F.Cu In3.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 0mm) (point 0mm 0mm)
          (hole (point 1mm 1mm) (point 2mm 1mm))))
      (clearance -0.1mm)
      (min_thickness 0mm)
      (connection solid (thermal_gap 0.2mm))
      (islands keep_all (minimum_area 1mm2))
      (fill solid (gap 0.5mm))
      (hatch_offsets (layer F.Cu 0mm 0mm))
      (priority -1)
      (border solid (pitch 0.5mm))
      (locked maybe))
    (zone GND
      (id self-crossing)
      (layers F.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 10mm) (point 0mm 10mm)
          (point 10mm 0mm) (point 10mm -2mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))
    (zone GND
      (id outside-hole)
      (layers F.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 0mm) (point 10mm 10mm) (point 0mm 10mm)
          (hole (point 20mm 20mm) (point 21mm 20mm) (point 21mm 21mm))))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))
    (zone GND
      (id invalid-hatch-offset)
      (layers F.Cu B.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 0mm) (point 10mm 10mm) (point 0mm 10mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill hatched (thickness 0.3mm) (gap 0.4mm) (orientation 0deg))
      (hatch_offsets
        (layer F.Cu 0mm 0mm)
        (layer F.Cu 0.1mm 0.1mm)
        (layer In1.Cu 0mm 0mm)))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "invalid_zone_layers", "duplicate_zone_point",
                              "invalid_zone_polygon", "invalid_zone_clearance",
                              "invalid_zone_min_thickness", "unexpected_zone_thermal_setting",
                              "unexpected_zone_minimum_island_area",
                              "unexpected_solid_zone_fill_setting", "invalid_zone_priority",
                              "unexpected_zone_hatch_offset",
                              "unexpected_zone_border_pitch", "invalid_zone_locked",
                              "zone_layer_outside_stackup",
                              "self_intersecting_zone_polygon",
                              "zone_hole_outside_polygon", "invalid_zone_hatch_offset" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesExplicitKeepoutPoliciesAndRejectsAmbiguousIntent )
{
    const std::string valid = R"KDS((kichad_design
  (version 1)
  (project keepout_board)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (keepout
      (id antenna-clearance)
      (name "Antenna clearance")
      (layers F.Cu B.Cu)
      (outline
        (polygon
          (point 1mm 1mm) (point 9mm 1mm) (point 9mm 7mm) (point 1mm 7mm)
          (hole (point 3mm 3mm) (point 4mm 3mm) (point 4mm 4mm))))
      (prohibit
        (copper true) (vias true) (tracks true) (pads false) (footprints true))
      (border diagonal_edge (pitch 0.5mm))
      (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( valid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 2 );
    const nlohmann::json& keepout = compiled.ir["pcb"][1];
    BOOST_CHECK_EQUAL( keepout["kind"].get<std::string>(), "keepout" );
    BOOST_CHECK_EQUAL( keepout["logicalId"].get<std::string>(), "antenna-clearance" );
    BOOST_CHECK( keepout["prohibitions"]["copper"].get<bool>() );
    BOOST_CHECK( !keepout["prohibitions"]["pads"].get<bool>() );
    BOOST_CHECK_EQUAL( keepout["polygons"][0]["holes"].size(), 1 );
    BOOST_CHECK_EQUAL( keepout["border"]["pitchNm"].get<int64_t>(), 500000 );
    BOOST_CHECK( compiled.plan["boardFullyTyped"].get<bool>() );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project bad_keepout)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled false)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
    (keepout
      (id ineffective)
      (layers F.Cu F.Cu In1.Cu)
      (outline
        (polygon (point 0mm 0mm) (point 5mm 0mm) (point 5mm 5mm)))
      (prohibit
        (copper false) (vias false) (tracks false) (pads false) (footprints false))
      (border solid (pitch 0.5mm))
      (locked maybe))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "invalid_keepout_layers", "empty_keepout",
                              "unexpected_zone_border_pitch", "invalid_keepout_locked",
                              "keepout_layer_outside_stackup" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesBoundedBoardTextAndRejectsUnsafeTypography )
{
    const std::string valid = R"KDS((kichad_design
  (version 1)
  (project annotated_board)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (text "Assembly\nrevision A"
      (id assembly-note)
      (layer F.SilkS)
      (at 12mm 8mm)
      (size 1.5mm 2mm)
      (stroke 0.2mm)
      (angle 90deg)
      (justify left top)
      (font stroke)
      (line_spacing 1.25)
      (bold true)
      (italic true)
      (underlined true)
      (mirrored true)
      (keep_upright true)
      (hyperlink "https://example.com/revision-a")
      (knockout true)
      (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( valid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 2 );
    const nlohmann::json& text = compiled.ir["pcb"][1];
    BOOST_CHECK_EQUAL( text["kind"].get<std::string>(), "text" );
    BOOST_CHECK_EQUAL( text["logicalId"].get<std::string>(), "assembly-note" );
    BOOST_CHECK_EQUAL( text["value"].get<std::string>(), "Assembly\nrevision A" );
    BOOST_CHECK_EQUAL( text["layer"].get<std::string>(), "F.SilkS" );
    BOOST_CHECK_EQUAL( text["size"]["xNm"].get<int64_t>(), 1500000 );
    BOOST_CHECK_EQUAL( text["strokeNm"].get<int64_t>(), 200000 );
    BOOST_CHECK_EQUAL( text["horizontalJustification"].get<std::string>(), "left" );
    BOOST_CHECK_EQUAL( text["verticalJustification"].get<std::string>(), "top" );
    BOOST_CHECK_CLOSE( text["lineSpacing"].get<double>(), 1.25, 0.0001 );
    BOOST_CHECK( text["multiline"].get<bool>() );
    BOOST_CHECK( text["bold"].get<bool>() );
    BOOST_CHECK( text["knockout"].get<bool>() );
    BOOST_CHECK( compiled.plan["boardFullyTyped"].get<bool>() );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project invalid_text)
  (board
    (stackup
      (finish "ENIG") (impedance_controlled false)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
    (text "Bad\rtext"
      (id unsafe-note)
      (layer In1.Cu)
      (at 3000mm 0mm)
      (size 0.5mm 0.5mm)
      (stroke 0.2mm)
      (justify sideways top)
      (font "")
      (line_spacing 0)
      (bold maybe)
      (hyperlink "line\nbreak"))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "invalid_text_value", "invalid_text_position",
                              "invalid_text_stroke", "invalid_text_justification",
                              "invalid_text_font", "invalid_text_line_spacing",
                              "invalid_text_boolean", "invalid_text_hyperlink",
                              "text_layer_outside_stackup" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( CompilesAllNativeDimensionStylesWithoutIgnoredIntent )
{
    const std::string valid = R"KDS((kichad_design
  (version 1)
  (project dimension_board)
  (board
    (dimension aligned
      (id aligned-width) (layer Dwgs.User)
      (from 0mm 0mm) (to 20mm 0mm) (height 3mm) (extension_height 0.2mm)
      (units mm) (unit_format bare_suffix) (precision fixed_2)
      (suppress_trailing_zeroes true) (prefix "W=") (suffix " nominal")
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0.1mm)
      (arrow_direction inward) (text_position manual) (keep_text_aligned false)
      (text_at 10mm 3mm) (text_size 1mm 1mm) (text_stroke 0.15mm)
      (text_angle 0deg) (text_justify center center) (font stroke) (locked true))
    (dimension orthogonal
      (id orthogonal-height) (layer Dwgs.User)
      (from 0mm 0mm) (to 0mm 10mm) (height -2mm) (extension_height 0mm) (axis y)
      (units automatic) (unit_format no_suffix) (precision scaled_in_4)
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0mm)
      (arrow_direction outward) (text_position inline) (keep_text_aligned true)
      (text_size 1mm 1mm) (text_stroke 0.15mm))
    (dimension radial
      (id radius) (layer Dwgs.User)
      (center 30mm 20mm) (radius_point 35mm 20mm) (leader_length 3mm)
      (units mm) (unit_format paren_suffix) (precision fixed_3)
      (prefix "R ") (line_width 0.15mm) (arrow_length 0.8mm)
      (keep_text_aligned false) (text_at 40mm 22mm)
      (text_size 1mm 1mm) (text_stroke 0.15mm) (text_angle 30deg))
    (dimension leader
      (id callout) (layer Dwgs.User)
      (from 5mm 15mm) (to 10mm 15mm) (border roundrect) (label "Inspect joint")
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0.1mm)
      (text_at 15mm 15mm) (text_size 1mm 1mm) (text_stroke 0.15mm)
      (bold true) (italic true))
    (dimension center
      (id hole-center) (layer Dwgs.User)
      (center 25mm 25mm) (to 28mm 25mm)
      (line_width 0.15mm) (arrow_length 0.8mm) (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( valid );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( compiled.ir["pcb"].size(), 5 );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][0]["dimensionStyle"].get<std::string>(), "aligned" );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][1]["geometry"]["axis"].get<std::string>(), "y" );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][2]["geometry"]["leaderLengthNm"].get<int64_t>(),
                       3000000 );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][3]["overrideText"].get<std::string>(),
                       "Inspect joint" );
    BOOST_CHECK( compiled.ir["pcb"][3]["overrideEnabled"].get<bool>() );
    BOOST_CHECK_EQUAL( compiled.ir["pcb"][4]["dimensionStyle"].get<std::string>(), "center" );
    BOOST_CHECK( compiled.ir["pcb"][4]["text"].empty() );
    BOOST_CHECK( compiled.plan["boardFullyTyped"].get<bool>() );

    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project invalid_dimension)
  (board
    (dimension center
      (id bad-center) (layer Missing.Layer)
      (center 1mm 1mm) (to 1mm 1mm)
      (line_width 0mm)
      (text_size 1mm 1mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT rejected =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );
    BOOST_CHECK( !rejected.ok );
    const std::string diagnostics = rejected.diagnostics.dump();

    for( const char* code : { "unknown_board_field", "invalid_dimension_layer",
                              "invalid_dimension_line_width", "invalid_dimension_arrow_length",
                              "zero_length_dimension" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( BoundsCopperZoneGeometryBeforePlanning )
{
    std::string oversized = R"KDS((kichad_design
  (version 1)
  (project bounded_zone)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1) (pin R2 1))
  (board
    (zone GND
      (id too-many-points)
      (layers F.Cu)
      (outline (polygon )KDS";

    for( int point = 0; point < 8193; ++point )
        oversized += "(point " + std::to_string( point ) + "nm 0nm)";

    oversized += R"KDS())
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( oversized );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "too_many_zone_points" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousAndUnsupportedFacetDeclarations )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project strict (title "First") (title "Second"))
  (library symbol Device (uri "one") (uri "two"))
  (library symbol Device)
  (sheet repeated)
  (sheet repeated)
  (component R1
    (symbol "Device:R") (value "1k") (footprint "R:R")
    (property "Tolerance" "1%") (property "Tolerance" "5%")
    (dnp false) (dnp true))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net N1 (pin R1 1) (pin R2 1))
  (net N2 (pin R1 1) (pin R2 2))
  (board
    (place MISSING (at 1mm 1mm))
    (route NO_NET (width 0.2mm))
    (teleport R1))
  (source R1 (manufacturer "Maker") (mpn "Part"))
  (source R1 (manufacturer "Maker") (mpn "Part"))
  (rules)
  (rules)
  (check erc verbose)
  (output assembly))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    const std::string diagnostics = result.diagnostics.dump();

    for( const char* code : { "duplicate_project_field", "duplicate_library_field",
                              "duplicate_library", "duplicate_sheet",
                              "duplicate_component_property",
                              "duplicate_component_field", "duplicate_pin_connection",
                              "unknown_board_statement", "duplicate_rules", "duplicate_source",
                              "invalid_check",
                              "invalid_output", "unresolved_component", "unresolved_net" } )
    {
        BOOST_CHECK_NE( diagnostics.find( code ), std::string::npos );
    }
}


BOOST_AUTO_TEST_CASE( ReportsAllSemanticErrorsWithoutProducingAnExecutableProgram )
{
    const std::string invalid = R"KDS((kichad_design
  (version 2)
  (project broken)
  (component R1 (symbol "Device:R") (value "1k"))
  (component R1 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net N1 (pin R1 1) (pin MISSING 2))
  (mystery unsafe))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalid );

    BOOST_CHECK( !result.ok );
    BOOST_CHECK_GE( result.diagnostics.size(), 5 );

    std::string diagnostics = result.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "invalid_version" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "missing_component_field" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "duplicate_component" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "unresolved_component" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "unknown_top_level_form" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( BoundsMalformedAndHostilePrograms )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT malformed =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( "(kichad_design (version 1)" );
    BOOST_CHECK( !malformed.ok );
    BOOST_REQUIRE( !malformed.diagnostics.empty() );
    BOOST_CHECK_EQUAL( malformed.diagnostics[0]["code"].get<std::string>(), "parse_failed" );

    std::string deep = "(kichad_design (version 1) (project deep) ";

    for( int i = 0; i < 300; ++i )
        deep += "(x ";

    deep += "value";

    for( int i = 0; i < 300; ++i )
        deep += ')';

    deep += ')';

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT nested =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( deep );
    BOOST_CHECK( !nested.ok );
    BOOST_REQUIRE( !nested.diagnostics.empty() );
    BOOST_CHECK_NE( nested.diagnostics[0]["message"].get<std::string>().find( "256 levels" ),
                    std::string::npos );

    std::string oversized( 1024 * 1024 + 1, 'x' );
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT large =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( oversized );
    BOOST_CHECK( !large.ok );
    BOOST_CHECK_EQUAL( large.diagnostics[0]["code"].get<std::string>(), "invalid_source_size" );

    std::string embeddedNul = "(kichad_design";
    embeddedNul.push_back( '\0' );
    embeddedNul += "(version 1))";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT nul =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( embeddedNul );
    BOOST_CHECK( !nul.ok );
    BOOST_CHECK_EQUAL( nul.diagnostics[0]["code"].get<std::string>(), "invalid_encoding" );

    const std::string invalidUtf8 = "(kichad_design (version 1) (project \"\xC3\x28\"))";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT utf8 =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( invalidUtf8 );
    BOOST_CHECK( !utf8.ok );
    BOOST_CHECK_EQUAL( utf8.diagnostics[0]["code"].get<std::string>(), "invalid_encoding" );
}


BOOST_AUTO_TEST_CASE( MinimalNewProjectSidecarIsAValidReusableProgram )
{
    const std::string source =
            "(kichad_design\n  (version 1)\n  (project \"My Project\"))\n";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_CHECK_EQUAL( result.ir["project"]["name"].get<std::string>(), "My Project" );
    BOOST_CHECK_EQUAL( result.plan["counts"]["components"].get<size_t>(), 0 );
}


BOOST_AUTO_TEST_CASE( DescribesAStableVersionedLanguageWithoutHostExecution )
{
    nlohmann::json description = KICHAD::DESIGN_SCRIPT_COMPILER::Describe();
    BOOST_CHECK_EQUAL( description["language"].get<std::string>(), "kichad-design" );
    BOOST_CHECK_EQUAL( description["version"].get<int>(), 1 );
    BOOST_CHECK( description["deterministic"].get<bool>() );
    BOOST_CHECK( !description["hostCodeExecution"].get<bool>() );
    BOOST_CHECK_GE( description["topLevelForms"].size(), 10 );
    BOOST_CHECK_NE( description.dump().find( "thermal_spoke_width" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "hatch_offsets" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "prohibit" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "edge_connector none|yes|bevelled" ),
                    std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "dielectric core|prepreg" ),
                    std::string::npos );
    BOOST_CHECK_NE( description.dump().find( "font stroke|NAME" ), std::string::npos );
    BOOST_CHECK_NE( description.dump().find(
                            "dimension aligned|orthogonal|radial|leader|center" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
