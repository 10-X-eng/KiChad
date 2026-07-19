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
#include <kicad/codex/design_script_schematic_planner.h>
#include <kicad/codex/lossless_sexpr_document.h>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

const std::string HIERARCHY_PROGRAM = R"KDS((kichad_design
  (version 1)
  (project hierarchy)
  (sheet root
    (parent none)
    (file "hierarchy.kicad_sch")
    (title "Main"))
  (sheet power
    (parent root)
    (file "sheets/power.kicad_sch")
    (title "Power")
    (at 20mm 30mm)
    (size 40mm 20mm)
    (pin VIN input (at 20mm 35mm) (side left))
    (pin VOUT output (at 60mm 35mm) (side right)))
  (sheet monitor
    (parent power)
    (file "sheets/monitor.kicad_sch")
    (title "Monitor")
    (at 80mm 30mm)
    (size 30mm 20mm)
    (pin SENSE input (at 80mm 35mm) (side left)))
))KDS";

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptSchematicPlanner )


BOOST_AUTO_TEST_CASE( LowersHierarchyIntoStableNativeExpressionsAndPaths )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_REQUIRE_EQUAL( first.operations.size(), 1 );
    BOOST_CHECK_EQUAL( first.counts["files"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( first.counts["sheets"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( first.counts["pins"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( first.counts["managedItems"].get<int>(), 5 );

    const nlohmann::json& operation = first.operations[0];
    BOOST_CHECK_EQUAL( operation["action"].get<std::string>(),
                       "reconcile_schematic_hierarchy" );
    BOOST_CHECK_EQUAL( operation["rootFile"].get<std::string>(), "hierarchy.kicad_sch" );
    BOOST_REQUIRE_EQUAL( operation["files"].size(), 3 );
    BOOST_CHECK( operation["files"][0]["root"].get<bool>() );
    BOOST_CHECK_EQUAL( operation["files"][1]["path"].get<std::string>(),
                       "sheets/power.kicad_sch" );
    BOOST_CHECK_EQUAL( operation["files"][2]["page"].get<int>(), 3 );

    const std::string rootSource = operation["files"][0]["newDocumentSource"];
    const std::string powerSource = operation["files"][1]["newDocumentSource"];
    BOOST_CHECK_NE( rootSource.find( "(version 20260306)" ), std::string::npos );
    BOOST_CHECK_NE( rootSource.find( "(title \"Main\")" ), std::string::npos );
    BOOST_CHECK_NE( rootSource.find( "(property \"Sheetfile\" \"sheets/power.kicad_sch\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( powerSource.find( "(hierarchical_label \"VIN\"" ), std::string::npos );
    BOOST_CHECK_NE( powerSource.find( "(hierarchical_label \"VOUT\"" ), std::string::npos );
    const std::string expectedNestedParentPath =
            "(path \"/" + operation["files"][0]["screenUuid"].get<std::string>() + "/"
            + operation["managedItems"][0]["uuid"].get<std::string>() + "\"";
    BOOST_CHECK_NE( powerSource.find( expectedNestedParentPath ), std::string::npos );

    for( const nlohmann::json& file : operation["files"] )
    {
        std::string parseError;
        BOOST_CHECK_MESSAGE(
                KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse(
                        file["newDocumentSource"].get<std::string>(), &parseError ),
                parseError );
    }
}


BOOST_AUTO_TEST_CASE( RejectsMalformedOrAliasedScreenIrWithoutPartialOperations )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE( compiled.ok );
    compiled.ir["schematic"]["sheets"][2]["file"] = "sheets/power.kicad_sch";

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_CHECK( !result.fullyLowered );
    BOOST_CHECK( result.operations.empty() );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "shared_sheet_file_not_supported" ),
                    std::string::npos );

    compiled.ir["schematic"]["sheets"][2].erase( "position" );
    result = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_CHECK( !result.fullyLowered );
    BOOST_CHECK( result.operations.empty() );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_schematic_ir" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( PreservesExistingScreenIdentityInNestedInstancePaths )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE( compiled.ok );
    const nlohmann::json existing = {
        { "hierarchy.kicad_sch", "11111111-2222-4333-8444-555555555555" },
        { "sheets/power.kicad_sch", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee" }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir, existing );
    BOOST_REQUIRE_MESSAGE( result.fullyLowered, result.diagnostics.dump() );
    const nlohmann::json& files = result.operations[0]["files"];
    BOOST_CHECK_EQUAL( files[0]["screenUuid"].get<std::string>(),
                       "11111111-2222-4333-8444-555555555555" );
    BOOST_CHECK_EQUAL( files[1]["screenUuid"].get<std::string>(),
                       "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee" );
    BOOST_CHECK_NE( files[1]["newDocumentSource"].get<std::string>().find(
                            "(uuid \"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\")" ),
                    std::string::npos );
    BOOST_CHECK_NE( files[1]["newDocumentSource"].get<std::string>().find(
                            "(path \"/11111111-2222-4333-8444-555555555555/" ),
                    std::string::npos );

    result = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
            compiled.ir, { { "hierarchy.kicad_sch", "not-a-uuid" } } );
    BOOST_CHECK( !result.fullyLowered );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "invalid_screen_identity" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( LowersResolvedComponentsGlobalNetsAndNoConnectsWithoutPlaceholders )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project connected)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "connected.kicad_sch") (title "Connected"))
  (component R1 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))
  (component R2 (symbol "Local:R") (value "20k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 60mm 40mm) (rotation 90deg) (mirror x)))
  (component R3 (symbol "Local:R") (value "30k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 80mm 40mm) (rotation 90deg) (mirror xy)))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
  (no_connect R1 1 2)
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const std::string cache = R"SYM((symbol "Local:R"
  (property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "R" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (symbol "R_1_1"
    (pin passive line (at 0 3.81 270) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 0 -3.81 90) (length 1.27)
      (name "" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27)))))))
)SYM";
    const nlohmann::json resolved = {
        { "Local:R",
          { { "libraryId", "Local:R" },
            { "cacheSource", cache },
            { "properties", { { "Description", "Resistor" } } },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", 0 }, { "yNm", 3810000 } },
                                 { { "number", "2" }, { "xNm", 0 }, { "yNm", -3810000 } } } ) } } } } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["components"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( plan.counts["netEndpoints"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( plan.counts["noConnects"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["librarySymbols"].get<int>(), 1 );
    const std::string native = plan.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(symbol \"Local:R\"" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(lib_id \"Local:R\")" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(global_label \"SIGNAL\"" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(no_connect" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(mirror x)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 40 36.19 0)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 56.19 40 0)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 40 43.81)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(at 80 40 270)" ), std::string::npos );
    BOOST_CHECK_EQUAL( native.find( "(mirror x y)" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( TreatsAnUndeclaredHierarchyAsACompleteNoOp )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile(
                    "(kichad_design (version 1) (project board_only))" );
    BOOST_REQUIRE( compiled.ok );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_CHECK( result.fullyLowered );
    BOOST_CHECK( result.operations.empty() );
    BOOST_CHECK_EQUAL( result.counts["files"].get<int>(), 0 );
}


BOOST_AUTO_TEST_CASE( LowersCanonicalWiresBusesEntriesAndJunctionsToNativeItems )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project schematic_drawings)
  (sheet root (parent none) (file "schematic_drawings.kicad_sch") (title "Drawings"))
  (wire signal-leg (sheet root) (from 20mm 20mm) (to 30mm 20mm)
    (stroke default default))
  (bus control-bus (sheet root) (from 20mm 30mm) (to 40mm 30mm)
    (stroke 0.5mm dash_dot))
  (bus_entry control-entry (sheet root) (from 25mm 28.73mm) (to 26.27mm 30mm)
    (stroke default solid))
  (junction signal-junction (sheet root) (at 30mm 20mm)
    (diameter 0.8mm) (color #11223380))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["drawings"].get<int>(), 4 );
    const std::string native = plan.operations[0]["files"][0]["newDocumentSource"];
    BOOST_CHECK_NE( native.find( "(wire\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(bus\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(bus_entry\n" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(size 1.27 1.27)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(width 0.5)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(type dash_dot)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(diameter 0.8)" ), std::string::npos );
    BOOST_CHECK_NE( native.find( "(color 17 34 51 0.50196078)" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( ExportsGeneratedHierarchyForOptInNativeValidation )
{
    wxString exportDirectory;

    if( !wxGetEnv( wxS( "KICHAD_QA_EXPORT_SCHEMATIC_DIR" ), &exportDirectory ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in native schematic fixture export" );
        return;
    }

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( HIERARCHY_PROGRAM );
    BOOST_REQUIRE( compiled.ok );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE( wxFileName::Mkdir( exportDirectory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL )
                   || wxDirExists( exportDirectory ) );

    for( const nlohmann::json& file : plan.operations[0]["files"] )
    {
        wxFileName target( wxString::FromUTF8( file["path"].get<std::string>() ) );
        target.MakeAbsolute( exportDirectory );
        BOOST_REQUIRE( wxFileName::Mkdir( target.GetPath(), wxS_DIR_DEFAULT,
                                         wxPATH_MKDIR_FULL )
                       || wxDirExists( target.GetPath() ) );
        const std::string source = file["newDocumentSource"].get<std::string>();
        wxFile output;
        BOOST_REQUIRE( output.Create( target.GetFullPath(), true ) );
        BOOST_REQUIRE_EQUAL( output.Write( source.data(), source.size() ), source.size() );
        BOOST_REQUIRE( output.Flush() );
    }
}


BOOST_AUTO_TEST_SUITE_END()
