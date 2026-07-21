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
#include <kicad/codex/design_script_symbol_resolver.h>


BOOST_AUTO_TEST_SUITE( DesignScriptSymbolResolver )


BOOST_AUTO_TEST_CASE( ResolvesExactProjectSymbolCacheAndPinsDeterministically )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project symbols)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "symbols.kicad_sch") (title "Symbols"))
  (component R1 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
))KDS";
    const std::string library = R"SYM((kicad_symbol_lib
  (version 20241209)
  (generator "kicad_symbol_editor")
  (symbol "R"
    (exclude_from_sim yes)
    (in_bom no)
    (on_board no)
    (in_pos_files no)
    (property "Reference" "R" (at -5 2 0) (do_not_autoplace yes)
      (effects (font (size 1.27 1.27))))
    (property "Value" "R" (at 5 -2 0) (hide yes) (show_name yes)
      (effects (font (size 1.27 1.27))))
    (symbol "R_1_1"
      (pin passive line (at 0 3.81 270) (length 1.27)
        (name "" (effects (font (size 1.27 1.27))))
        (number "1" (effects (font (size 1.27 1.27)))))
      (pin passive line (at 0 -3.81 90) (length 1.27)
        (name "" (effects (font (size 1.27 1.27))))
        (number "2" (effects (font (size 1.27 1.27))))))))
)SYM";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const nlohmann::json sources = { { "Local", library } };
    KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve( compiled.ir, sources );
    KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve( compiled.ir, sources );
    BOOST_REQUIRE_MESSAGE( first.ok, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.symbols.dump(), second.symbols.dump() );
    BOOST_REQUIRE( first.symbols.contains( "Local:R" ) );
    BOOST_CHECK_NE( first.symbols["Local:R"]["cacheSource"].get<std::string>().find(
                            "(symbol \"Local:R\"" ), std::string::npos );
    BOOST_REQUIRE_EQUAL( first.symbols["Local:R"]["units"]["1"].size(), 2 );
    BOOST_CHECK_EQUAL( first.symbols["Local:R"]["units"]["1"][0]["number"], "1" );
    BOOST_CHECK_EQUAL( first.symbols["Local:R"]["units"]["1"][0]["yNm"], 3810000 );
    BOOST_CHECK_EQUAL(
            first.symbols["Local:R"]["units"]["1"][0]["rotationDegrees"], 270 );
    BOOST_CHECK_EQUAL( first.symbols["Local:R"]["flags"]["excludeFromSim"], true );
    BOOST_CHECK_EQUAL( first.symbols["Local:R"]["flags"]["inBom"], false );
    BOOST_CHECK_EQUAL( first.symbols["Local:R"]["flags"]["onBoard"], false );
    BOOST_CHECK_EQUAL( first.symbols["Local:R"]["flags"]["inPosFiles"], false );
    const nlohmann::json& referenceLayout =
            first.symbols["Local:R"]["propertyLayouts"]["Reference"];
    BOOST_CHECK_EQUAL( referenceLayout["position"]["xNm"], -5000000 );
    BOOST_CHECK_EQUAL( referenceLayout["position"]["yNm"], 2000000 );
    BOOST_CHECK( referenceLayout["visible"].get<bool>() );
    BOOST_CHECK( !referenceLayout["autoplace"].get<bool>() );
    const nlohmann::json& valueLayout =
            first.symbols["Local:R"]["propertyLayouts"]["Value"];
    BOOST_CHECK( !valueLayout["visible"].get<bool>() );
    BOOST_CHECK( valueLayout["showName"].get<bool>() );

    std::string malformedFlags = library;
    const size_t inBom = malformedFlags.find( "(in_bom no)" );
    BOOST_REQUIRE_NE( inBom, std::string::npos );
    malformedFlags.replace( inBom, std::string( "(in_bom no)" ).size(), "(in_bom maybe)" );
    KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT malformed =
            KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                    compiled.ir, { { "Local", malformedFlags } } );
    BOOST_CHECK( !malformed.ok );
    BOOST_CHECK_NE( malformed.diagnostics.dump().find( "invalid_symbol_flags" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( ResolvesDerivedSymbolsAndRejectsUnsafeInheritance )
{
    const std::string globalProgram = R"KDS((kichad_design
  (version 1) (project global)
  (library symbol Device (table global))
  (library footprint Fp (table global))
  (sheet root (parent none) (file "global.kicad_sch") (title "Global"))
  (component R1 (symbol "Device:R") (value "R") (footprint "Fp:R")
    (unit 1 (sheet root) (at 1mm 1mm) (rotation 0deg) (mirror none))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( globalProgram );
    BOOST_REQUIRE( compiled.ok );
    KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                    compiled.ir, nlohmann::json::object() );
    BOOST_CHECK_MESSAGE( !result.ok, result.diagnostics.dump() );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "global_symbol_resolution_not_supported" ),
                    std::string::npos );

    const std::string projectProgram = R"KDS((kichad_design
  (version 1) (project project_symbols)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "project_symbols.kicad_sch") (title "Project"))
  (component U1 (symbol "Local:Derived") (value "D") (footprint "LocalFp:D")
    (unit 1 (sheet root) (at 1mm 1mm) (rotation 0deg) (mirror none))))
)KDS";
    compiled = KICHAD::DESIGN_SCRIPT_COMPILER::Compile( projectProgram );
    BOOST_REQUIRE( compiled.ok );
    result = KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
            compiled.ir, nlohmann::json::object() );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "missing_symbol_library_source" ),
                    std::string::npos );

    const std::string missingParentLibrary = R"SYM((kicad_symbol_lib
  (version 20241209)
  (generator "kicad_symbol_editor")
  (symbol "Derived" (extends "Base")))
)SYM";
    result = KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
            compiled.ir, { { "Local", missingParentLibrary } } );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "unresolved_symbol_parent" ),
                    std::string::npos );

    const std::string derivedLibrary = R"SYM((kicad_symbol_lib
  (version 20241209)
  (generator "kicad_symbol_editor")
  (symbol "Base"
    (exclude_from_sim yes)
    (in_bom no)
    (on_board no)
    (in_pos_files no)
    (property "Reference" "U" (at 0 0 0) (effects (font (size 1.27 1.27))))
    (property "Value" "Base" (at 0 0 0) (effects (font (size 1.27 1.27))))
    (property "Description" "Base description"
      (at 0 0 0) (effects (font (size 1.27 1.27))))
    (symbol "Base_1_1"
      (pin passive line (at 0 2.54 270) (length 1.27)
        (name "P" (effects (font (size 1.27 1.27))))
        (number "1" (effects (font (size 1.27 1.27)))))))
  (symbol "Intermediate"
    (extends "Base")
    (property "Description" "Intermediate description"
      (at 0 0 0) (effects (font (size 1.27 1.27)))))
  (symbol "Derived"
    (extends "Intermediate")
    (property "Value" "Derived" (at 0 0 0) (effects (font (size 1.27 1.27))))
    (property "Description" "" (at 0 0 0) (effects (font (size 1.27 1.27))))))
)SYM";
    result = KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
            compiled.ir, { { "Local", derivedLibrary } } );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE( result.symbols.contains( "Local:Derived" ) );
    BOOST_CHECK_EQUAL( result.symbols["Local:Derived"]["inheritanceDepth"], 2 );
    BOOST_CHECK_EQUAL( result.symbols["Local:Derived"]["properties"]["Value"], "Derived" );
    BOOST_CHECK_EQUAL( result.symbols["Local:Derived"]["properties"]["Description"],
                       "Intermediate description" );
    BOOST_CHECK_EQUAL( result.symbols["Local:Derived"]["flags"]["excludeFromSim"], false );
    BOOST_CHECK_EQUAL( result.symbols["Local:Derived"]["flags"]["inBom"], true );
    BOOST_CHECK_NE( result.symbols["Local:Derived"]["cacheSource"].get<std::string>().find(
                            "(symbol \"Local:Derived\"" ), std::string::npos );
    BOOST_CHECK_EQUAL( result.symbols["Local:Derived"]["cacheSource"].get<std::string>().find(
                               "(extends" ), std::string::npos );
    BOOST_CHECK_NE( result.symbols["Local:Derived"]["cacheSource"].get<std::string>().find(
                            "(symbol \"Derived_1_1\"" ), std::string::npos );
    BOOST_REQUIRE_EQUAL( result.symbols["Local:Derived"]["units"]["1"].size(), 1 );

    const std::string recursiveLibrary = R"SYM((kicad_symbol_lib
  (version 20241209)
  (generator "kicad_symbol_editor")
  (symbol "Derived" (extends "Intermediate"))
  (symbol "Intermediate" (extends "Derived")))
)SYM";
    result = KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
            compiled.ir, { { "Local", recursiveLibrary } } );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "recursive_symbol_inheritance" ),
                    std::string::npos );

    std::string futureLibrary = missingParentLibrary;
    const size_t version = futureLibrary.find( "20241209" );
    BOOST_REQUIRE_NE( version, std::string::npos );
    futureLibrary.replace( version, 8, "20990101" );
    result = KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
            compiled.ir, { { "Local", futureLibrary } } );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "unsupported_symbol_library_version" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
