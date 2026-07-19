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
    (stackup (copper_layers 2) (thickness 1.6mm))
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


BOOST_AUTO_TEST_CASE( RejectsMalformedPhysicalBoardIntent )
{
    const std::string invalid = R"KDS((kichad_design
  (version 1)
  (project physical_errors)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net POWER (pin R1 1) (pin R2 1))
  (board
    (stackup (copper_layers 3) (thickness -1mm))
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
    (stackup (copper_layers 4) (thickness 1.6mm))
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
    (stackup (copper_layers 4) (thickness 1.6mm))
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
  (rule repeated (minimum 0.2mm))
  (rule repeated (minimum 0.3mm))
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
                              "unknown_board_statement", "duplicate_rule", "duplicate_source",
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
}


BOOST_AUTO_TEST_SUITE_END()
