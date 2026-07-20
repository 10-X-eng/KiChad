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
#include <kicad/codex/design_script_symbol_resolver.h>


BOOST_AUTO_TEST_SUITE( DesignScriptSymbolLibraryGenerator )


BOOST_AUTO_TEST_CASE( LowersOneAiNativeRepresentationToCurrentDeterministicLibrarySource )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project authored)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/libraries/Product.kicad_sym") (managed true))
  (sheet root (parent none) (file "authored.kicad_sch") (title "Authored"))
  (symbol Product:SENSOR
    (reference U)
    (value SENSOR)
    (datasheet "https://example.test/sensor.pdf")
    (description "Two-pin production sensor")
    (keywords "sensor precision")
    (property "Manufacturer" "Example Semiconductor")
    (pin_names_offset 0.254mm)
    (unit common
      (rectangle body (from -2.54mm -1.27mm) (to 2.54mm 1.27mm)
        (stroke 0.254mm default) (fill background)))
    (unit 1
      (pin 1 (name IN) (electrical input) (shape line)
        (at -5.08mm 0mm) (orientation right) (length 2.54mm))
      (pin 2 (name OUT) (electrical output) (shape inverted)
        (at 5.08mm 0mm) (orientation left) (length 2.54mm))))
  (component U1 (symbol "Product:SENSOR") (value "SENSOR") (footprint none)
    (unit 1 (sheet root) (at 20mm 20mm) (rotation 0deg) (mirror none))))
)KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    BOOST_REQUIRE_EQUAL( compiled.ir["authoredSymbols"].size(), 1 );
    BOOST_CHECK_EQUAL( compiled.plan["counts"]["authoredSymbols"], 1 );

    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT first =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT second =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.ok, first.diagnostics.dump( 2 ) );
    BOOST_CHECK_EQUAL( first.sources, second.sources );
    BOOST_REQUIRE( first.sources.contains( "Product" ) );
    const std::string source = first.sources["Product"].get<std::string>();
    BOOST_CHECK_NE( source.find( "(version 20251024)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(generator \"kichad_kds\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(symbol \"SENSOR_0_1\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(symbol \"SENSOR_1_1\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(pin input line" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(pin output inverted" ), std::string::npos );
    BOOST_CHECK_EQUAL( first.counts["libraries"], 1 );
    BOOST_CHECK_EQUAL( first.counts["symbols"], 1 );
    BOOST_CHECK_EQUAL( first.counts["pins"], 2 );
    BOOST_CHECK_EQUAL( first.counts["graphics"], 1 );

    KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT resolved =
            KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve( compiled.ir, first.sources );
    BOOST_REQUIRE_MESSAGE( resolved.ok, resolved.diagnostics.dump( 2 ) );
    BOOST_REQUIRE( resolved.symbols.contains( "Product:SENSOR" ) );
    BOOST_REQUIRE_EQUAL( resolved.symbols["Product:SENSOR"]["units"]["1"].size(), 2 );
    BOOST_CHECK_EQUAL( resolved.symbols["Product:SENSOR"]["units"]["1"][0]["number"], "1" );
    BOOST_CHECK_EQUAL( resolved.symbols["Product:SENSOR"]["properties"]["Manufacturer"],
                       "Example Semiconductor" );
}


BOOST_AUTO_TEST_CASE( RejectsImplicitOwnershipAmbiguousPinsAndUnknownNativePassthrough )
{
    const std::string unmanaged = R"KDS((kichad_design
  (version 1) (project unsafe)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (symbol Local:X (unit 1 (pin 1 (at 0mm 0mm))))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( unmanaged );
    BOOST_CHECK( !compiled.ok );
    BOOST_CHECK_NE( compiled.diagnostics.dump().find( "unmanaged_authored_symbol_library" ),
                    std::string::npos );

    const std::string duplicatePins = R"KDS((kichad_design
  (version 1) (project unsafe)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym") (managed true))
  (symbol Local:X
    (native_sexpr "(pin passive line)")
    (unit 1
      (pin 1 (at 0mm 0mm))
      (pin 1 (at 2.54mm 0mm))))
))KDS";
    compiled = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( duplicatePins );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "unknown_authored_symbol_field" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "duplicate_authored_symbol_pin_number" ),
                    std::string::npos );

    const std::string emptyManaged = R"KDS((kichad_design
  (version 1) (project unsafe)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym") (managed true))
))KDS";
    compiled = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( emptyManaged );
    BOOST_CHECK( !compiled.ok );
    BOOST_CHECK_NE( compiled.diagnostics.dump().find( "empty_managed_symbol_library" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
