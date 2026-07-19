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
#include <kicad/codex/design_script_schematic_reconciler.h>


namespace
{

const std::string PROGRAM = R"KDS((kichad_design
  (version 1)
  (project hierarchy)
  (sheet root (parent none) (file "hierarchy.kicad_sch") (title "Managed title"))
  (sheet power (parent root) (file "power.kicad_sch") (title "Power")
    (at 20mm 30mm) (size 40mm 20mm)
    (pin VIN input (at 20mm 35mm) (side left)))
))KDS";


nlohmann::json operationFor( const nlohmann::json& aExisting = nlohmann::json::object() )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( PROGRAM );
    BOOST_REQUIRE( compiled.ok );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( compiled.ir, aExisting );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    return plan.operations.at( 0 );
}


nlohmann::json componentOperation()
{
    const std::string source = R"KDS((kichad_design
  (version 1) (project component_cache)
  (library symbol Local (table project) (uri "${KIPRJMOD}/Local.kicad_sym"))
  (library footprint LocalFp (table project) (uri "${KIPRJMOD}/Local.pretty"))
  (sheet root (parent none) (file "component_cache.kicad_sch") (title "Cache"))
  (component R1 (symbol "Local:R") (value "10k") (footprint "LocalFp:R")
    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const nlohmann::json resolved = {
        { "Local:R",
          { { "libraryId", "Local:R" },
            { "cacheSource",
              "(symbol \"Local:R\"\n"
              "  (property \"Reference\" \"R\" (at 0 0 0) "
              "(effects (font (size 1.27 1.27))))\n"
              "  (property \"Value\" \"R\" (at 0 0 0) "
              "(effects (font (size 1.27 1.27))))\n"
              "  (symbol \"R_1_1\"\n"
              "    (pin passive line (at 0 3.81 270) (length 1.27) "
              "(name \"\" (effects (font (size 1.27 1.27)))) "
              "(number \"1\" (effects (font (size 1.27 1.27)))))))" },
            { "flags",
              { { "excludeFromSim", false }, { "inBom", true }, { "onBoard", true },
                { "inPosFiles", true } } },
            { "properties", nlohmann::json::object() },
            { "units",
              { { "1", nlohmann::json::array(
                               { { { "number", "1" }, { "xNm", 0 },
                                   { "yNm", 3810000 } } } ) } } } } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, nlohmann::json::object(), resolved );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    return plan.operations.at( 0 );
}

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptSchematicReconciler )


BOOST_AUTO_TEST_CASE( CreatesMissingNativeHierarchyFilesExactlyOnce )
{
    const nlohmann::json operation = operationFor();
    const nlohmann::json live = {
        { { "path", "hierarchy.kicad_sch" }, { "present", false } },
        { { "path", "power.kicad_sch" }, { "present", false } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    operation, nlohmann::json::array(), live );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.fileActions.size(), 2 );
    BOOST_CHECK_EQUAL( result.counts["filesCreated"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( result.counts["itemsUpserted"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( result.managedItems, operation["managedItems"] );

    nlohmann::json installed = nlohmann::json::array();

    for( const nlohmann::json& action : result.fileActions )
    {
        installed.push_back( { { "path", action["path"] },
                               { "present", true },
                               { "source", action["source"] } } );
    }

    result = KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
            operation, operation["managedItems"], installed );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_CHECK( result.fileActions.empty() );
    BOOST_CHECK_EQUAL( result.counts["filesUnchanged"].get<int>(), 2 );
}


BOOST_AUTO_TEST_CASE( PreservesUnknownBytesAndExistingScreenUuidDuringManagedUpdates )
{
    const std::string existingUuid = "11111111-2222-4333-8444-555555555555";
    const nlohmann::json operation =
            operationFor( { { "hierarchy.kicad_sch", existingUuid } } );
    const std::string root =
            "; exact leading comment\n"
            "(kicad_sch\n"
            "  (version 20260306)\n"
            "  (generator \"eeschema\")\n"
            "  (generator_version \"10.0\")\n"
            "  (uuid \"" + existingUuid + "\")\n"
            "  (paper \"A4\")\n"
            "  (title_block (title \"Old\") (company \"KEEP COMPANY\"))\n"
            "  (lib_symbols)\n"
            "  (future_extension \"KEEP EXACTLY\")\n"
            "  (sheet_instances (path \"/\" (page \"9\")))\n"
            "  (embedded_fonts no)\n"
            ")\n";
    const nlohmann::json live = {
        { { "path", "hierarchy.kicad_sch" }, { "present", true }, { "source", root } },
        { { "path", "power.kicad_sch" }, { "present", false } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    operation, nlohmann::json::array(), live );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.fileActions.size(), 2 );
    const std::string updated = result.fileActions[0]["source"];
    BOOST_CHECK_NE( updated.find( "; exact leading comment\n" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(company \"KEEP COMPANY\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(future_extension \"KEEP EXACTLY\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(uuid \"" + existingUuid + "\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(title \"Managed title\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(page \"1\")" ), std::string::npos );
    BOOST_CHECK_LT( updated.find( "(sheet\n" ), updated.find( "(sheet_instances" ) );
}


BOOST_AUTO_TEST_CASE( ReplacesAnExplicitlyOwnedCompleteRootTitleBlock )
{
    const std::string program = R"KDS((kichad_design
  (version 1)
  (project hierarchy
    (title "Owned title") (date "2026-07-19") (revision "D")
    (company "Owned company") (comment 2 "Owned comment"))
  (sheet root (parent none) (file "hierarchy.kicad_sch") (title "Root"))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const std::string existingUuid = "11111111-2222-4333-8444-555555555555";
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, { { "hierarchy.kicad_sch", existingUuid } } );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    const std::string existing =
            "(kicad_sch\n"
            "  (version 20260306)\n"
            "  (generator \"eeschema\")\n"
            "  (generator_version \"10.0\")\n"
            "  (uuid \"" + existingUuid + "\")\n"
            "  (paper \"A4\")\n"
            "  (title_block (title \"Stale\") (company \"Stale\") "
            "(comment 1 \"Remove me\"))\n"
            "  (lib_symbols)\n"
            "  (sheet_instances (path \"/\" (page \"1\")))\n"
            "  (embedded_fonts no)\n"
            ")\n";
    const nlohmann::json live = {
        { { "path", "hierarchy.kicad_sch" }, { "present", true }, { "source", existing } }
    };
    const KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    plan.operations[0], nlohmann::json::array(), live );
    BOOST_REQUIRE_MESSAGE( result.ok, result.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( result.fileActions.size(), 1 );
    const std::string updated = result.fileActions[0]["source"];
    BOOST_CHECK_NE( updated.find( "(title \"Owned title\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(date \"2026-07-19\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(rev \"D\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(company \"Owned company\")" ), std::string::npos );
    BOOST_CHECK_NE( updated.find( "(comment 2 \"Owned comment\")" ), std::string::npos );
    BOOST_CHECK_EQUAL( updated.find( "Remove me" ), std::string::npos );
    BOOST_CHECK_EQUAL( updated.find( "company \"Stale\"" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( RemovesOnlyItemsProvenToBePreviouslyManaged )
{
    const nlohmann::json operation = operationFor();
    const nlohmann::json createLive = {
        { { "path", "hierarchy.kicad_sch" }, { "present", false } },
        { { "path", "power.kicad_sch" }, { "present", false } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT created =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    operation, nlohmann::json::array(), createLive );
    BOOST_REQUIRE( created.ok );
    std::string root = created.fileActions[0]["source"];
    const std::string unmanaged =
            "  (hierarchical_label \"UNMANAGED\" (shape passive) (at 1 1 0)"
            " (uuid \"99999999-9999-4999-8999-999999999999\"))\n";
    root.insert( root.rfind( ')' ), unmanaged );
    const nlohmann::json live = {
        { { "path", "hierarchy.kicad_sch" }, { "present", true }, { "source", root } },
        { { "path", "power.kicad_sch" },
          { "present", true },
          { "source", created.fileActions[1]["source"] } }
    };
    nlohmann::json emptyOperation = operation;
    emptyOperation["files"] = nlohmann::json::array();
    emptyOperation["managedItems"] = nlohmann::json::array();
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT removed =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    emptyOperation, operation["managedItems"], live );
    BOOST_REQUIRE_MESSAGE( removed.ok, removed.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( removed.fileActions.size(), 2 );
    BOOST_CHECK_EQUAL( removed.counts["itemsRemoved"].get<int>(), 2 );
    BOOST_CHECK_NE( removed.fileActions[0]["source"].get<std::string>().find(
                            "99999999-9999-4999-8999-999999999999" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( removed.fileActions[0]["source"].get<std::string>().find(
                               operation["managedItems"][0]["uuid"].get<std::string>() ),
                       std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsIdentityCollisionsAndStaleScreenInventories )
{
    nlohmann::json operation = operationFor();
    std::string collision = operation["files"][0]["newDocumentSource"];
    const std::string sheetUuid = operation["managedItems"][0]["uuid"];
    const size_t sheetStart = collision.find( "  (sheet\n" );
    const size_t sheetEnd = collision.find( "\n  (sheet_instances", sheetStart );
    BOOST_REQUIRE_NE( sheetStart, std::string::npos );
    BOOST_REQUIRE_NE( sheetEnd, std::string::npos );
    collision.replace( sheetStart, sheetEnd - sheetStart,
                       "  (wire (pts (xy 0 0) (xy 1 1)) (uuid \"" + sheetUuid + "\"))" );
    const nlohmann::json live = {
        { { "path", "hierarchy.kicad_sch" },
          { "present", true },
          { "source", collision } },
        { { "path", "power.kicad_sch" }, { "present", false } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    operation, nlohmann::json::array(), live );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "schematic_identity_collision" ),
                    std::string::npos );

    nlohmann::json stale = live;
    stale[0]["source"] = operation["files"][0]["newDocumentSource"];
    stale[0]["source"] = stale[0]["source"].get<std::string>();
    const std::string plannedUuid = operation["files"][0]["screenUuid"];
    std::string staleSource = stale[0]["source"];
    staleSource.replace( staleSource.find( plannedUuid ), plannedUuid.size(),
                         "11111111-2222-4333-8444-555555555555" );
    stale[0]["source"] = staleSource;
    result = KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
            operation, nlohmann::json::array(), stale );
    BOOST_CHECK( !result.ok );
    BOOST_CHECK_NE( result.diagnostics.dump().find( "stale_schematic_inventory" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( ReconcilesOnlyOwnedCachedSymbolsAndPlacedUnits )
{
    const nlohmann::json operation = componentOperation();
    const nlohmann::json absent = {
        { { "path", "component_cache.kicad_sch" }, { "present", false } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT created =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    operation, nlohmann::json::array(), absent );
    BOOST_REQUIRE_MESSAGE( created.ok, created.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( created.fileActions.size(), 1 );
    BOOST_CHECK_EQUAL( created.counts["itemsUpserted"].get<int>(), 2 );
    const nlohmann::json installed = {
        { { "path", "component_cache.kicad_sch" },
          { "present", true },
          { "source", created.fileActions[0]["source"] } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT repeated =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    operation, operation["managedItems"], installed );
    BOOST_REQUIRE_MESSAGE( repeated.ok, repeated.diagnostics.dump() );
    BOOST_CHECK( repeated.fileActions.empty() );

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT unmanaged =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    operation, nlohmann::json::array(), installed );
    BOOST_CHECK( !unmanaged.ok );
    BOOST_CHECK_NE( unmanaged.diagnostics.dump().find( "is unmanaged" ), std::string::npos );

    nlohmann::json empty = operation;
    empty["files"] = nlohmann::json::array();
    empty["managedItems"] = nlohmann::json::array();
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT removed =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    empty, operation["managedItems"], installed );
    BOOST_REQUIRE_MESSAGE( removed.ok, removed.diagnostics.dump() );
    BOOST_CHECK_EQUAL( removed.counts["itemsRemoved"].get<int>(), 2 );
    BOOST_REQUIRE_EQUAL( removed.fileActions.size(), 1 );
    BOOST_CHECK_EQUAL( removed.fileActions[0]["source"].get<std::string>().find(
                               "(symbol \"Local:R\"" ),
                       std::string::npos );
}


BOOST_AUTO_TEST_CASE( ReconcilesBusAliasesByOwnedNameWithoutClaimingUnmanagedAliases )
{
    const nlohmann::json baseOperation = operationFor();
    const nlohmann::json absent = {
        { { "path", "hierarchy.kicad_sch" }, { "present", false } },
        { { "path", "power.kicad_sch" }, { "present", false } }
    };
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT baseCreated =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    baseOperation, nlohmann::json::array(), absent );
    BOOST_REQUIRE_MESSAGE( baseCreated.ok, baseCreated.diagnostics.dump() );
    nlohmann::json installed = nlohmann::json::array();

    for( const nlohmann::json& action : baseCreated.fileActions )
    {
        installed.push_back( { { "path", action["path"] },
                               { "present", true },
                               { "source", action["source"] } } );
    }

    nlohmann::json aliasOperation = baseOperation;
    const nlohmann::json alias = {
        { "file", "hierarchy.kicad_sch" },
        { "kind", "bus_alias" },
        { "logicalId", "root/SIGNALS" },
        { "name", "SIGNALS" },
        { "uuid", "11111111-2222-8333-8444-555555555555" },
        { "source", "(bus_alias \"SIGNALS\" (members \"SIGNAL\"))" }
    };
    aliasOperation["files"][0]["busAliases"].push_back( alias );
    nlohmann::json aliasOwnership = alias;
    aliasOwnership.erase( "source" );
    aliasOperation["managedItems"].push_back( aliasOwnership );
    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT added =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    aliasOperation, nlohmann::json::array(), installed );
    BOOST_REQUIRE_MESSAGE( added.ok, added.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( added.fileActions.size(), 1 );
    BOOST_CHECK_NE( added.fileActions[0]["source"].get<std::string>().find(
                            "(bus_alias \"SIGNALS\"" ),
                    std::string::npos );

    for( nlohmann::json& live : installed )
    {
        if( live["path"] == "hierarchy.kicad_sch" )
            live["source"] = added.fileActions[0]["source"];
    }

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT repeated =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    aliasOperation, aliasOperation["managedItems"], installed );
    BOOST_REQUIRE_MESSAGE( repeated.ok, repeated.diagnostics.dump() );
    BOOST_CHECK( repeated.fileActions.empty() );

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT unmanaged =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    aliasOperation, nlohmann::json::array(), installed );
    BOOST_CHECK( !unmanaged.ok );
    BOOST_CHECK_NE( unmanaged.diagnostics.dump().find( "is unmanaged" ),
                    std::string::npos );

    KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT removed =
            KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                    baseOperation, aliasOperation["managedItems"], installed );
    BOOST_REQUIRE_MESSAGE( removed.ok, removed.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( removed.fileActions.size(), 1 );
    BOOST_CHECK_EQUAL( removed.fileActions[0]["source"].get<std::string>().find(
                               "(bus_alias \"SIGNALS\"" ),
                       std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
