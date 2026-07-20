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


BOOST_AUTO_TEST_SUITE( DesignScriptSymbolGraphics )


BOOST_AUTO_TEST_CASE( TypechecksAndLowersEveryKiCad10VectorPrimitiveDeterministically )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project graphics)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:GRAPHICS
    (unit common
      (rectangle body (from -5mm -4mm) (to 5mm 4mm) (radius 0.5mm)
        (stroke 0.2mm solid (color 10 20 30 0.75)) (fill background))
      (circle lens (center 0mm 0mm) (radius 2mm) (private true)
        (stroke 0.1mm dash) (fill color (color 40 50 60 0.5)))
      (arc sweep (start -2mm 0mm) (mid 0mm 2mm) (end 2mm 0mm)
        (stroke 0.15mm dot) (fill none))
      (bezier curve (start -3mm -2mm) (control1 -1mm 2mm)
        (control2 1mm -2mm) (end 3mm 2mm)
        (stroke 0.12mm dash_dot) (fill outline))
      (polyline marker (point -1mm 0mm) (point 0mm 1mm) (point 1mm 0mm)
        (stroke 0mm default) (fill cross_hatch (color 70 80 90 1))))
    (unit 1
      (pin 1 (at -7.54mm 0mm) (orientation right) (length 2.54mm))))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    const nlohmann::json& items = compiled.ir["authoredSymbols"][0]["units"][0]["items"];
    BOOST_REQUIRE_EQUAL( items.size(), 5 );
    BOOST_CHECK_EQUAL( items[0]["kind"], "rectangle" );
    BOOST_CHECK_EQUAL( items[0]["radiusNm"], 500'000 );
    BOOST_CHECK_EQUAL( items[0]["stroke"]["color"]["alphaPpm"], 750'000 );
    BOOST_CHECK_EQUAL( items[1]["kind"], "circle" );
    BOOST_CHECK( items[1]["private"].get<bool>() );
    BOOST_CHECK_EQUAL( items[4]["points"].size(), 3 );

    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT first =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT second =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.ok, first.diagnostics.dump( 2 ) );
    BOOST_CHECK_EQUAL( first.sources, second.sources );
    BOOST_CHECK_EQUAL( first.counts["graphics"], 5 );
    const std::string source = first.sources["Product"].get<std::string>();
    BOOST_CHECK_NE( source.find( "(rectangle\n" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(radius 0.5)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(circle private\n" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(arc\n" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(bezier\n" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(polyline\n" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(color 10 20 30 0.75)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(type cross_hatch)" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsDegenerateUnboundedAndAmbiguousGraphics )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project bad_graphics)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:BAD
    (unit common
      (rectangle flat (from 0mm 0mm) (to 0mm 2mm))
      (circle empty (center 0mm 0mm) (radius 0mm))
      (arc line (start 0mm 0mm) (mid 1mm 1mm) (end 2mm 2mm))
      (polyline short (point 0mm 0mm))
      (bezier same (start 1mm 1mm) (control1 1mm 1mm)
        (control2 1mm 1mm) (end 1mm 1mm))
      (circle color_bad (center 0mm 0mm) (radius 1mm)
        (stroke 0mm solid (color 256 0 0 1))))
    (unit 1 (pin 1 (at 0mm 0mm))))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "degenerate_authored_symbol_rectangle" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "missing_authored_symbol_circle_geometry" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "degenerate_authored_symbol_arc" ), std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "missing_authored_symbol_polyline_geometry" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "degenerate_authored_symbol_bezier" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_graphic_stroke" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
