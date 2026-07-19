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
    (outline (rect (at 0mm 0mm) (size 40mm 30mm)))
    (place R1 (at 10mm 10mm) (rotation 0deg) (side front))
    (route LED_A (width 0.25mm) (layer F.Cu)))
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
    BOOST_CHECK_EQUAL( first.plan["counts"]["boardStatements"].get<size_t>(), 4 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["sourcingRecords"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["checks"].get<size_t>(), 3 );
    BOOST_CHECK_EQUAL( first.plan["counts"]["outputs"].get<size_t>(), 3 );
    BOOST_CHECK( first.plan["transactional"].get<bool>() );
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
}


BOOST_AUTO_TEST_SUITE_END()
