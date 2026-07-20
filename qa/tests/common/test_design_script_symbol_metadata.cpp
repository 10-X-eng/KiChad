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
#include <kicad/codex/design_script_symbol_library_generator.h>


BOOST_AUTO_TEST_SUITE( DesignScriptSymbolMetadata )


BOOST_AUTO_TEST_CASE( LowersNamedAndDemorganBodyStylesUnitNamesFiltersAndLocking )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project symbol_metadata)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:DUAL
    (reference U) (value DUAL)
    (body_styles "Normal" "Alternate")
    (units_locked true)
    (footprint_filter "Package_SO:*")
    (footprint_filter "Package_DFN_QFN:*")
    (unit common (body_style 1)
      (rectangle normal_body (from -2mm -1mm) (to 2mm 1mm)))
    (unit common (body_style 2)
      (rectangle alternate_body (from -1mm -2mm) (to 1mm 2mm)))
    (unit 1 (body_style 1) (display_name "Channel A")
      (pin 1 (name A) (at -2.54mm 0mm)))
    (unit 1 (body_style 2) (display_name "Channel A")
      (pin 1 (name A_ALT) (at -2.54mm 0mm)))
    (unit 2 (body_style 1) (display_name "Channel B")
      (pin 2 (name B) (at 2.54mm 0mm)))
    (unit 2 (body_style 2) (display_name "Channel B")
      (pin 2 (name B_ALT) (at 2.54mm 0mm))))
  (symbol Product:LOGIC
    (reference U) (value LOGIC) (body_styles demorgan)
    (unit 1 (body_style 1) (pin 1 (name A) (at -2.54mm 0mm)))
    (unit 1 (body_style 2) (pin 1 (name A) (at -2.54mm 0mm))))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    BOOST_REQUIRE_EQUAL( compiled.ir["authoredSymbols"].size(), 2 );

    const auto& dual = compiled.ir["authoredSymbols"][0];
    BOOST_CHECK_EQUAL( dual["bodyStyles"]["mode"], "named" );
    BOOST_REQUIRE_EQUAL( dual["bodyStyles"]["names"].size(), 2 );
    BOOST_CHECK_EQUAL( dual["bodyStyles"]["names"][0], "Normal" );
    BOOST_CHECK_EQUAL( dual["bodyStyles"]["names"][1], "Alternate" );
    BOOST_CHECK( dual["unitsLocked"].get<bool>() );
    BOOST_REQUIRE_EQUAL( dual["footprintFilters"].size(), 2 );
    BOOST_CHECK_EQUAL( dual["units"][2]["displayName"], "Channel A" );
    BOOST_CHECK_EQUAL( dual["units"][4]["displayName"], "Channel B" );

    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT generated =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( generated.ok, generated.diagnostics.dump( 2 ) );
    const std::string source = generated.sources["Product"].get<std::string>();
    BOOST_CHECK_NE( source.find( "(body_styles \"Normal\" \"Alternate\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(body_styles demorgan)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(property \"ki_locked\" \"\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find(
            "(property \"ki_fp_filters\" \"Package_SO:* Package_DFN_QFN:*\"" ),
            std::string::npos );
    BOOST_CHECK_NE( source.find( "(symbol \"DUAL_1_1\"\n\t\t\t(unit_name \"Channel A\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(symbol \"DUAL_2_2\"\n\t\t\t(unit_name \"Channel B\")" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( generated.counts["pins"], 6 );

    auto malformed = compiled.ir;
    malformed["authoredSymbols"][0]["bodyStyles"]["names"][1] = "Normal";
    generated = KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( malformed );
    BOOST_CHECK( !generated.ok );
    BOOST_CHECK_NE( generated.diagnostics.dump().find( "invalid_authored_symbol_ir" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsAmbiguousOrUnrepresentableLibraryMetadata )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project invalid_symbol_metadata)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:UNDECLARED_STYLE
    (unit 1 (body_style 2) (pin 1 (at 0mm 0mm))))
  (symbol Product:CONFLICTING_NAMES
    (body_styles "Normal" "Alternate")
    (unit 1 (body_style 1) (display_name "Channel A") (pin 1 (at 0mm 0mm)))
    (unit 1 (body_style 2) (display_name "Different") (pin 1 (at 0mm 0mm))))
  (symbol Product:BAD_FILTERS
    (footprint_filter "Package_SO:*")
    (footprint_filter "Package_SO:*")
    (footprint_filter "Has Space")
    (unit 1 (pin 1 (at 0mm 0mm))))
  (symbol Product:BAD_STYLE_NAMES
    (body_styles "Same" "Same")
    (unit 1 (pin 1 (at 0mm 0mm))))
  (symbol Product:BAD_COMMON_NAME
    (unit common (display_name "Common") (rectangle box (from 0mm 0mm) (to 1mm 1mm)))
    (unit 1 (display_name "A") (display_name "B") (pin 1 (at 0mm 0mm))))
  (symbol Product:BASE (unit 1 (pin 1 (at 0mm 0mm))))
  (symbol Product:DERIVED (extends BASE)
    (body_styles demorgan) (units_locked false) (footprint_filter "Package_DIP:*")
    (property "ki_locked" "forbidden"))
))KDS";

    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "undeclared_authored_symbol_body_style" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "conflicting_authored_symbol_unit_display_name" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "duplicate_authored_symbol_footprint_filter" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_footprint_filter" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "duplicate_authored_symbol_body_style_name" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_unit_display_name" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "derived_authored_symbol_library_metadata" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "reserved_authored_symbol_property" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( EnforcesLibraryMetadataCardinalityAndTextBounds )
{
    std::string program = R"KDS((kichad_design
  (version 1) (project bounded_symbol_metadata)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:BOUNDED
    (body_styles)KDS";

    for( int style = 0; style < 65; ++style )
        program += " \"Style" + std::to_string( style ) + "\"";

    program += ")\n";

    for( int filter = 0; filter < 257; ++filter )
        program += "    (footprint_filter \"Library:Filter" + std::to_string( filter ) + "\")\n";

    program += "    (unit 1 (display_name \"" + std::string( 257, 'x' )
               + "\") (pin 1 (at 0mm 0mm))))\n)";

    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();
    BOOST_CHECK_MESSAGE( diagnostics.find( "invalid_authored_symbol_body_styles" )
                                 != std::string::npos,
                         diagnostics );
    BOOST_CHECK_MESSAGE( diagnostics.find( "invalid_authored_symbol_footprint_filter" )
                                 != std::string::npos,
                         diagnostics );
    BOOST_CHECK_MESSAGE( diagnostics.find( "invalid_authored_symbol_unit_display_name" )
                                 != std::string::npos,
                         diagnostics );
}


BOOST_AUTO_TEST_SUITE_END()
