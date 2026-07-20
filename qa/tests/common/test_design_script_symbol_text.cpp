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


BOOST_AUTO_TEST_SUITE( DesignScriptSymbolText )


BOOST_AUTO_TEST_CASE( TypechecksAndLowersCompleteTextAndTextBoxFormatting )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project symbol_text)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:ANNOTATED
    (unit common
      (text title "READY" (at 0mm 2mm) (rotation 90deg) (private true)
        (size 1mm 2mm) (font "DejaVu Sans") (thickness 0.15mm)
        (bold true) (italic true) (line_spacing 1.25)
        (color 10 20 30 0.75) (justify left top)
        (hyperlink "https://example.test/ready"))
      (text_box note "Line one\nLine two" (at -4mm -3mm) (box_size 8mm 3mm)
        (rotation 12.5deg) (margins 0.1mm 0.2mm 0.3mm 0.4mm)
        (stroke 0.2mm dash_dot (color 40 50 60 0.5))
        (fill color (color 70 80 90 0.25))
        (size 1.2mm 1.4mm) (font stroke) (thickness auto)
        (line_spacing 0.9) (justify right bottom) (hyperlink "#2")))
    (unit 1 (pin 1 (at 0mm 0mm))))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    const nlohmann::json& items = compiled.ir["authoredSymbols"][0]["units"][0]["items"];
    BOOST_REQUIRE_EQUAL( items.size(), 2 );
    BOOST_CHECK_EQUAL( items[0]["kind"], "text" );
    BOOST_CHECK_EQUAL( items[0]["rotationTenths"], 900 );
    BOOST_CHECK_EQUAL( items[0]["effects"]["widthNm"], 1'000'000 );
    BOOST_CHECK_EQUAL( items[0]["effects"]["heightNm"], 2'000'000 );
    BOOST_CHECK_EQUAL( items[0]["effects"]["lineSpacingPpm"], 1'250'000 );
    BOOST_CHECK_EQUAL( items[1]["rotationTenths"], 125 );
    BOOST_CHECK_EQUAL( items[1]["marginsNm"][3], 400'000 );

    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT generated =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( generated.ok, generated.diagnostics.dump( 2 ) );
    BOOST_CHECK_EQUAL( generated.counts["graphics"], 2 );
    const std::string source = generated.sources["Product"].get<std::string>();
    BOOST_CHECK_NE( source.find( "(text private \"READY\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(at 0 2 900)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(face \"DejaVu Sans\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(size 2 1)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(line_spacing 1.25)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(bold yes)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(italic yes)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(justify left top)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(href \"https://example.test/ready\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(text_box \"Line one\\nLine two\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(at -4 -3 12.5)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(size 8 3)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(margins 0.1 0.2 0.3 0.4)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(href \"#2\")" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsMissingGeometryUnsafeLinksAndInvalidTypography )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project bad_symbol_text)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:BAD
    (unit common
      (text missing "x" (rotation 0.01deg))
      (text link "x" (at 0mm 0mm) (hyperlink "relative path"))
      (text size "x" (at 0mm 0mm) (size 0mm 1mm))
      (text_box box "x" (at 0mm 0mm) (box_size -1mm 2mm)
        (margins 0mm 0mm 0mm -1mm)))
    (unit 1 (pin 1 (at 0mm 0mm))))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "missing_authored_symbol_text_position" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_text_rotation" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_text_hyperlink" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_text_size" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_text_box_size" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_text_box_margins" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
