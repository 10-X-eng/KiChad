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
#include <kicad/codex/design_script_pcb_planner.h>

#include <import_export.h>
#include <api/board/board_types.pb.h>
#include <google/protobuf/util/json_util.h>


namespace
{

const std::string PCB_PROGRAM = R"KDS((kichad_design
  (version 1)
  (project deterministic_board)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1) (pin R2 1))
  (board
    (outline (rect (id edge) (at 0mm 0mm) (size 20mm 10mm)))
    (route SIGNAL (id trace-a) (from 1mm 2mm) (to 3mm 4mm)
      (width 0.25mm) (layer F.Cu))
    (route SIGNAL (id arc-a) (from 3mm 4mm) (mid 4mm 5mm) (to 5mm 4mm)
      (width 0.2mm) (layer B.Cu) (locked true))
    (via SIGNAL (id via-a) (at 5mm 4mm) (diameter 0.8mm) (drill 0.4mm))))
)KDS";


template<typename Message>
void checkProtobufJson( const nlohmann::json& aJson )
{
    Message message;
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = false;
    google::protobuf::util::Status status =
            google::protobuf::util::JsonStringToMessage( aJson.dump(), &message, options );
    BOOST_CHECK_MESSAGE( status.ok(), status.ToString() );
}

} // namespace


BOOST_AUTO_TEST_SUITE( DesignScriptPcbPlanner )


BOOST_AUTO_TEST_CASE( LowersTypedPhysicalIrIntoExactDeterministicProtobufJson )
{
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( PCB_PROGRAM );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT first =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT second =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( first.fullyLowered, first.diagnostics.dump() );
    BOOST_CHECK_EQUAL( first.operations.dump(), second.operations.dump() );
    BOOST_REQUIRE_EQUAL( first.operations.size(), 4 );
    BOOST_CHECK_EQUAL( first.counts["upserts"].get<int>(), 4 );

    checkProtobufJson<kiapi::board::types::BoardGraphicShape>( first.operations[0]["item"] );
    checkProtobufJson<kiapi::board::types::Track>( first.operations[1]["item"] );
    checkProtobufJson<kiapi::board::types::Arc>( first.operations[2]["item"] );
    checkProtobufJson<kiapi::board::types::Via>( first.operations[3]["item"] );

    BOOST_CHECK_EQUAL( first.operations[1]["item"]["width"]["valueNm"].get<std::string>(),
                       "250000" );
    BOOST_CHECK_EQUAL( first.operations[2]["item"]["locked"].get<std::string>(), "LS_LOCKED" );
    const std::string id = first.operations[1]["itemId"].get<std::string>();
    BOOST_CHECK_EQUAL( id, "c3fc8149-6c3c-8f2d-94c6-2d462e6d2a49" );
    BOOST_CHECK_EQUAL( id[14], '8' );
    BOOST_CHECK( id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b' );
}


BOOST_AUTO_TEST_CASE( MarksStructurallyPreservedStatementsAsUnsupported )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project future_board)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (board
    (stackup (copper_layers 4) (thickness 1.6mm))
    (place R1 (at 1mm 1mm))
    (text "future annotation" (at 1mm 1mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_CHECK( !compiled.plan["boardFullyTyped"].get<bool>() );

    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_CHECK( !plan.fullyLowered );
    BOOST_CHECK_EQUAL( plan.counts["unsupported"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( plan.counts["placements"].get<int>(), 1 );
}


BOOST_AUTO_TEST_CASE( LowersExplicitCopperZoneIntoDeterministicProtobufJson )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project zone_board)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1) (pin R2 1))
  (board
    (zone GND
      (id ground-plane)
      (name "Ground plane")
      (layers F.Cu B.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 20mm 0mm) (point 20mm 10mm) (point 0mm 10mm)
          (hole (point 5mm 3mm) (point 7mm 3mm) (point 7mm 5mm) (point 5mm 5mm))))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection pth_thermal (thermal_gap 0.3mm) (thermal_spoke_width 0.35mm))
      (islands remove_below (minimum_area 2mm2))
      (fill hatched (thickness 0.3mm) (gap 0.4mm) (orientation 45deg)
        (smoothing 0.25) (hole_min_area_ratio 0.1) (border hatch))
      (hatch_offsets (layer F.Cu 0.1mm 0.2mm) (layer B.Cu -0.1mm 0mm))
      (priority 7)
      (border diagonal_full (pitch 0.6mm))
      (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["upserts"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( plan.operations[0]["itemType"].get<std::string>(), "zone" );
    checkProtobufJson<kiapi::board::types::Zone>( plan.operations[0]["item"] );
    const nlohmann::json& item = plan.operations[0]["item"];
    BOOST_CHECK_EQUAL( item["type"].get<std::string>(), "ZT_COPPER" );
    BOOST_REQUIRE_EQUAL( item["layers"].size(), 2 );
    BOOST_CHECK_EQUAL( item["copperSettings"]["connection"]["zoneConnection"].get<std::string>(),
                       "ZCS_PTH_THERMAL" );
    BOOST_CHECK_EQUAL( item["copperSettings"]["minIslandArea"].get<std::string>(),
                       "2000000000000" );
    BOOST_CHECK_EQUAL( item["copperSettings"]["fillMode"].get<std::string>(), "ZFM_HATCHED" );
    BOOST_CHECK_EQUAL(
            item["copperSettings"]["hatchSettings"]["thickness"]["valueNm"].get<std::string>(),
            "300000" );
    BOOST_REQUIRE_EQUAL( item["layerProperties"].size(), 2 );
    BOOST_CHECK_EQUAL( item["layerProperties"][0]["layer"].get<std::string>(), "BL_F_Cu" );
    BOOST_CHECK_EQUAL( item["layerProperties"][0]["hatchingOffset"]["xNm"].get<std::string>(),
                       "100000" );
    BOOST_CHECK_EQUAL( item["layerProperties"][0]["hatchingOffset"]["yNm"].get<std::string>(),
                       "200000" );
    BOOST_CHECK_EQUAL( item["layerProperties"][1]["layer"].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK_EQUAL( item["layerProperties"][1]["hatchingOffset"]["xNm"].get<std::string>(),
                       "-100000" );
    BOOST_CHECK_EQUAL( item["layerProperties"][1]["hatchingOffset"]["yNm"].get<std::string>(),
                       "0" );
    BOOST_REQUIRE_EQUAL( item["outline"]["polygons"][0]["holes"].size(), 1 );
    BOOST_REQUIRE_EQUAL( item["outline"]["polygons"][0]["holes"][0]["nodes"].size(), 4 );
    BOOST_CHECK( !item["filled"].get<bool>() );
    BOOST_CHECK_EQUAL( item["locked"].get<std::string>(), "LS_LOCKED" );
}


BOOST_AUTO_TEST_CASE( FullyLowersTypedComponentPlacement )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project placed_board)
  (component U1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (board (place U1 (at 12.5mm 7.5mm) (rotation 37.5deg) (side back) (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["placements"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( plan.operations[0]["action"].get<std::string>(),
                       "place_by_reference" );
    BOOST_CHECK_EQUAL( plan.operations[0]["position"]["xNm"].get<int64_t>(), 12500000 );
    BOOST_CHECK_EQUAL( plan.operations[0]["rotationDegrees"].get<double>(), 37.5 );
    BOOST_CHECK_EQUAL( plan.operations[0]["side"].get<std::string>(), "back" );
    BOOST_CHECK( plan.operations[0]["locked"].get<bool>() );
}


BOOST_AUTO_TEST_CASE( RejectsMalformedCompilerIrWithoutThrowing )
{
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT missing =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( nlohmann::json::object() );
    BOOST_CHECK( !missing.fullyLowered );
    BOOST_REQUIRE_EQUAL( missing.diagnostics.size(), 1 );

    nlohmann::json malformed = {
        { "language", "kichad-design" }, { "version", 1 },
        { "project", { { "name", "broken" } } },
        { "pcb", nlohmann::json::array( { { { "kind", "trace" } } } ) }
    };
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT invalid =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( malformed );
    BOOST_CHECK( !invalid.fullyLowered );
    BOOST_CHECK( invalid.operations.empty() );
    BOOST_REQUIRE_EQUAL( invalid.diagnostics.size(), 1 );
}


BOOST_AUTO_TEST_SUITE_END()
