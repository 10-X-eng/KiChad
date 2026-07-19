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
    (zone GND (layer F.Cu))))
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
