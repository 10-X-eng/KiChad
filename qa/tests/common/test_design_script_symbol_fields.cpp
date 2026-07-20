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


BOOST_AUTO_TEST_SUITE( DesignScriptSymbolFields )


BOOST_AUTO_TEST_CASE( TypechecksAndLowersCompleteMandatoryAndCustomFieldLayouts )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project symbol_fields)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:FIELDS
    (reference U (at -1mm -2mm) (rotation 12.5deg) (visible true)
      (show_name true) (autoplace false) (private true) (size 1mm 2mm)
      (font "DejaVu Sans") (line_spacing 1.25) (thickness 0.15mm)
      (color 10 20 30 0.75) (justify left top) (bold true) (italic true)
      (hyperlink "https://example.test/reference"))
    (value FIELDS (at 1mm 2mm) (rotation -90deg) (visible false))
    (property "Manufacturer" "Example Semiconductor" (at 3mm 4mm)
      (visible true) (show_name true) (size 1.1mm 1.2mm)
      (font stroke) (thickness auto) (color default) (justify right bottom)
      (hyperlink "#2"))
    (unit 1 (pin 1 (at 0mm 0mm))))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    const nlohmann::json& symbol = compiled.ir["authoredSymbols"][0];
    BOOST_CHECK_EQUAL( symbol["fieldLayouts"]["reference"]["rotationTenths"], 125 );
    BOOST_CHECK_EQUAL( symbol["fieldLayouts"]["value"]["rotationTenths"], 2700 );
    BOOST_CHECK( symbol["fieldLayouts"]["reference"]["private"].get<bool>() );
    BOOST_CHECK_EQUAL( symbol["fieldLayouts"]["reference"]["color"]["alphaPpm"], 750'000 );
    BOOST_REQUIRE_EQUAL( symbol["properties"].size(), 1 );
    BOOST_CHECK_EQUAL( symbol["properties"][0]["layout"]["position"]["xNm"], 3'000'000 );

    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT generated =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( generated.ok, generated.diagnostics.dump( 2 ) );
    const std::string source = generated.sources["Product"].get<std::string>();
    BOOST_CHECK_NE( source.find( "(property private \"Reference\" \"U\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(at -1 -2 12.5)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(show_name yes)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(do_not_autoplace yes)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(face \"DejaVu Sans\")" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(size 2 1)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(line_spacing 1.25)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(color 10 20 30 0.75)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(href \"https://example.test/reference\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(property \"Value\" \"FIELDS\"" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(at 1 2 270)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(property \"Manufacturer\" \"Example Semiconductor\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( source.find( "(justify right bottom)" ), std::string::npos );
    BOOST_CHECK_NE( source.find( "(href \"#2\")" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsUnsafeIncompleteAndOutOfRangeFieldLayouts )
{
    const std::string program = R"KDS((kichad_design
  (version 1) (project bad_symbol_fields)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/Product.kicad_sym") (managed true))
  (symbol Product:BAD
    (reference U (rotation 0.01deg) (size 0mm 1mm) (visible maybe))
    (value BAD (color 256 0 0 1) (hyperlink "relative path"))
    (property "X" "Y" (at 3000mm 0mm) (justify middle center))
    (unit 1 (pin 1 (at 0mm 0mm))))
))KDS";

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_CHECK( !compiled.ok );
    const std::string diagnostics = compiled.diagnostics.dump();
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_field_rotation" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_field_size" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_field_boolean" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_field_color" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_field_hyperlink" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_field_at" ),
                    std::string::npos );
    BOOST_CHECK_NE( diagnostics.find( "invalid_authored_symbol_field_justify" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
