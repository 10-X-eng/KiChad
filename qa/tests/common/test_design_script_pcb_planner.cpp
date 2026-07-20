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

#include <limits>

#include <kicad/codex/design_script_compiler.h>
#include <kicad/codex/design_script_pcb_planner.h>

#include <import_export.h>
#include <api/board/board.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/types/base_types.pb.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/util/json_util.h>
#include <libraries/library_table_parser.h>


namespace
{

const std::string PCB_PROGRAM = R"KDS((kichad_design
  (version 1)
  (project deterministic_board)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
  (board
    (outline (rect (id edge) (at 0mm 0mm) (size 20mm 10mm)))
    (route SIGNAL (id trace-a) (from 1mm 2mm) (to 3mm 4mm)
      (width 0.25mm) (layer F.Cu))
    (route SIGNAL (id arc-a) (from 3mm 4mm) (mid 4mm 5mm) (to 5mm 4mm)
      (width 0.2mm) (layer B.Cu) (locked true))
    (via SIGNAL (id via-a) (at 5mm 4mm) (diameter 0.8mm) (drill 0.4mm)
      (protection
        (tenting (front open) (back tented))
        (covering (front covered) (back inherit))
        (plugging (front plugged) (back unplugged))
        (filling filled) (capping uncapped)
        (post_machining front counterbore (diameter 0.6mm) (depth 0.15mm))))
    (via SIGNAL (id via-b) (at 6mm 4mm) (drill 0.3mm)
      (padstack custom
        (layer F.Cu (shape circle) (size 0.7mm 0.7mm))
        (layer B.Cu (shape chamfered_rect) (size 0.8mm 0.7mm)
          (offset 0.05mm 0mm) (roundrect_radius 0.05mm)
          (chamfer_ratio 0.2) (chamfer top_left bottom_right))))))
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
    BOOST_REQUIRE_EQUAL( first.operations.size(), 5 );
    BOOST_CHECK_EQUAL( first.counts["upserts"].get<int>(), 5 );

    checkProtobufJson<kiapi::board::types::BoardGraphicShape>( first.operations[0]["item"] );
    checkProtobufJson<kiapi::board::types::Track>( first.operations[1]["item"] );
    checkProtobufJson<kiapi::board::types::Arc>( first.operations[2]["item"] );
    checkProtobufJson<kiapi::board::types::Via>( first.operations[3]["item"] );
    checkProtobufJson<kiapi::board::types::Via>( first.operations[4]["item"] );

    BOOST_CHECK_EQUAL( first.operations[1]["item"]["width"]["valueNm"].get<std::string>(),
                       "250000" );
    BOOST_CHECK_EQUAL( first.operations[2]["item"]["locked"].get<std::string>(), "LS_LOCKED" );
    const nlohmann::json& viaStack = first.operations[3]["item"]["padStack"];
    BOOST_CHECK_EQUAL( viaStack["frontOuterLayers"]["solderMaskMode"], "SMM_MASKED" );
    BOOST_CHECK_EQUAL( viaStack["backOuterLayers"]["solderMaskMode"], "SMM_UNMASKED" );
    BOOST_CHECK_EQUAL( viaStack["frontOuterLayers"]["coveringMode"], "VCM_COVERED" );
    BOOST_CHECK_EQUAL( viaStack["frontOuterLayers"]["pluggingMode"], "VPM_PLUGGED" );
    BOOST_CHECK_EQUAL( viaStack["drill"]["filled"], "VDFM_FILLED" );
    BOOST_CHECK_EQUAL( viaStack["drill"]["capped"], "VDCM_UNCAPPED" );
    BOOST_CHECK_EQUAL( viaStack["frontPostMachining"]["mode"], "VDPM_COUNTERBORE" );
    BOOST_CHECK_EQUAL( viaStack["frontPostMachining"]["size"], 600000 );
    BOOST_CHECK_EQUAL( viaStack["frontPostMachining"]["depth"], 150000 );
    const nlohmann::json& customStack = first.operations[4]["item"]["padStack"];
    BOOST_CHECK_EQUAL( customStack["type"], "PST_CUSTOM" );
    BOOST_REQUIRE_EQUAL( customStack["copperLayers"].size(), 2 );
    BOOST_CHECK_EQUAL( customStack["copperLayers"][1]["shape"], "PSS_CHAMFEREDRECT" );
    BOOST_CHECK_CLOSE( customStack["copperLayers"][1]["cornerRoundingRatio"].get<double>(),
                       0.0714285714, 0.0001 );
    BOOST_CHECK( customStack["copperLayers"][1]["chamferedCorners"]["topLeft"]
                         .get<bool>() );
    const std::string id = first.operations[1]["itemId"].get<std::string>();
    BOOST_CHECK_EQUAL( id, "c3fc8149-6c3c-8f2d-94c6-2d462e6d2a49" );
    BOOST_CHECK_EQUAL( id[14], '8' );
    BOOST_CHECK( id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b' );
}


BOOST_AUTO_TEST_CASE( LowersCompleteFourLayerViaStackAndTwoSidedBackdrilling )
{
    const std::string source = R"KDS((kichad_design
  (version 1) (project advanced_via)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1 1) (pin R2 1 1))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled false)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.3mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked false))
        (copper In1.Cu (thickness 35um))
        (dielectric core (thickness 0.8mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper In2.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.3mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
    (via SIGNAL (id stacked) (at 5mm 5mm) (drill 0.3mm)
      (padstack custom
        (layer F.Cu (shape circle) (size 0.7mm 0.7mm))
        (layer In1.Cu (shape oval) (size 0.8mm 0.6mm))
        (layer In2.Cu (shape rect) (size 0.7mm 0.7mm) (offset 0.05mm 0mm))
        (layer B.Cu (shape roundrect) (size 0.8mm 0.7mm)
          (roundrect_radius 0.1mm)))
      (backdrills
        (top (diameter 0.5mm) (stop_layer In1.Cu))
        (bottom (diameter 0.55mm) (stop_layer In2.Cu))))))
)KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    const KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump( 2 ) );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 2 );
    const nlohmann::json& item = plan.operations[1]["item"];
    checkProtobufJson<kiapi::board::types::Via>( item );
    const nlohmann::json& stack = item["padStack"];
    BOOST_CHECK_EQUAL( stack["type"], "PST_CUSTOM" );
    BOOST_REQUIRE_EQUAL( stack["copperLayers"].size(), 4 );
    BOOST_CHECK_EQUAL( stack["secondaryDrill"]["startLayer"], "BL_F_Cu" );
    BOOST_CHECK_EQUAL( stack["secondaryDrill"]["endLayer"], "BL_In1_Cu" );
    BOOST_CHECK_EQUAL( stack["tertiaryDrill"]["startLayer"], "BL_B_Cu" );
    BOOST_CHECK_EQUAL( stack["tertiaryDrill"]["endLayer"], "BL_In2_Cu" );
}


BOOST_AUTO_TEST_CASE( LowersOneCompleteProjectTitleBlockToNativePcbSettings )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project titled
    (title "Production Controller") (date "2026-07-19") (revision "C")
    (company "KiChad QA") (comment 1 "Release candidate")
    (comment 9 "Generated from KDS"))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.operations[0]["action"], "update_title_block" );
    checkProtobufJson<kiapi::common::types::TitleBlockInfo>(
            plan.operations[0]["titleBlock"] );
    BOOST_CHECK_EQUAL( plan.operations[0]["titleBlock"]["title"],
                       "Production Controller" );
    BOOST_CHECK_EQUAL( plan.operations[0]["titleBlock"]["comment1"],
                       "Release candidate" );
    BOOST_CHECK_EQUAL( plan.operations[0]["titleBlock"]["comment9"],
                       "Generated from KDS" );
    BOOST_CHECK_EQUAL( plan.counts["titleBlocks"], 1 );
}


BOOST_AUTO_TEST_CASE( LowersProjectTextVariablesAndFieldTemplatesToTypedNativeSettings )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project project_settings
    (text_variables
      (variable PRODUCT_NAME "AI Controller")
      (variable AUTHOR "KiChad"))
    (field_templates
      (field MPN (visible true) (url false))
      (field "Compliance URL" (visible false) (url true))))
))KDS";
    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    const KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 2 );
    BOOST_CHECK_EQUAL( plan.operations[0]["action"], "update_text_variables" );
    BOOST_CHECK_EQUAL( plan.operations[1]["action"],
                       "update_schematic_field_templates" );
    checkProtobufJson<kiapi::common::project::TextVariables>(
            plan.operations[0]["textVariables"] );
    checkProtobufJson<kiapi::common::project::SchematicFieldTemplates>(
            plan.operations[1]["fieldTemplates"] );
    BOOST_CHECK_EQUAL(
            plan.operations[0]["textVariables"]["variables"]["PRODUCT_NAME"],
            "AI Controller" );
    BOOST_CHECK_EQUAL( plan.operations[1]["fieldTemplates"]["fields"][1]["name"],
                       "Compliance URL" );
    BOOST_CHECK_EQUAL( plan.counts["textVariables"], 2 );
    BOOST_CHECK_EQUAL( plan.counts["fieldTemplates"], 2 );
}


BOOST_AUTO_TEST_CASE( LowersCanonicalLibrariesIntoExactNativeProjectTables )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project canonical_libraries)
  (library symbol Device (table global))
  (library symbol LocalSymbols (table project)
    (uri "${KIPRJMOD}/libraries/local.kicad_sym"))
  (library footprint Resistor_SMD (table global))
  (library footprint LocalFootprints (table project)
    (uri "${KIPRJMOD}/libraries/Local.pretty"))
  (library model LocalModels (table project)
    (uri "${KIPRJMOD}/libraries/models"))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );

    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 2 );
    BOOST_CHECK_EQUAL( plan.counts["symbolLibraries"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( plan.counts["footprintLibraries"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( plan.counts["modelLibraries"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["libraryTables"].get<int>(), 2 );

    const std::string expectedSymbols =
            "(sym_lib_table\n"
            "  (version 7)\n"
            "  (lib (name \"LocalSymbols\")(type \"KiCad\")(uri "
            "\"${KIPRJMOD}/libraries/local.kicad_sym\")(options \"\")(descr \"\"))\n"
            ")\n";
    const std::string expectedFootprints =
            "(fp_lib_table\n"
            "  (version 7)\n"
            "  (lib (name \"LocalFootprints\")(type \"KiCad\")(uri "
            "\"${KIPRJMOD}/libraries/Local.pretty\")(options \"\")(descr \"\"))\n"
            ")\n";
    BOOST_CHECK_EQUAL( plan.operations[0]["path"].get<std::string>(), "sym-lib-table" );
    BOOST_CHECK_EQUAL( plan.operations[0]["source"].get<std::string>(), expectedSymbols );
    BOOST_CHECK_EQUAL( plan.operations[1]["path"].get<std::string>(), "fp-lib-table" );
    BOOST_CHECK_EQUAL( plan.operations[1]["source"].get<std::string>(), expectedFootprints );

    LIBRARY_TABLE_PARSER parser;
    auto symbols = parser.ParseBuffer( plan.operations[0]["source"].get<std::string>() );
    auto footprints = parser.ParseBuffer( plan.operations[1]["source"].get<std::string>() );
    BOOST_REQUIRE( symbols.has_value() );
    BOOST_REQUIRE( footprints.has_value() );
    BOOST_CHECK( symbols->type == LIBRARY_TABLE_TYPE::SYMBOL );
    BOOST_CHECK( footprints->type == LIBRARY_TABLE_TYPE::FOOTPRINT );
    BOOST_CHECK_EQUAL( symbols->version, "7" );
    BOOST_CHECK_EQUAL( symbols->rows.size(), 1 );
    BOOST_CHECK_EQUAL( footprints->rows.size(), 1 );
}


BOOST_AUTO_TEST_CASE( LowersExplicitStackupIntoOneLosslessNativeMessage )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project future_board)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (board
    (stackup
      (finish "ENIG") (impedance_controlled true)
      (edge_connector none) (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper In1.Cu (thickness 35um))
        (dielectric prepreg (thickness 0.486mm) (material "FR4")
          (epsilon_r 4.2) (loss_tangent 0.02) (locked true))
        (copper In2.Cu (thickness 35um))
        (dielectric core (thickness 0.488mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))))
    (place R1 (at 1mm 1mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    BOOST_CHECK( compiled.plan["boardFullyTyped"].get<bool>() );

    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["unsupported"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( plan.counts["stackups"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["placements"].get<int>(), 1 );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 2 );
    BOOST_CHECK_EQUAL( plan.operations[0]["action"].get<std::string>(), "update_stackup" );
    checkProtobufJson<kiapi::board::BoardStackup>( plan.operations[0]["stackup"] );
    const nlohmann::json& stackup = plan.operations[0]["stackup"];
    BOOST_CHECK_EQUAL( stackup["finish"]["typeName"].get<std::string>(), "ENIG" );
    BOOST_CHECK( stackup["impedance"]["isControlled"].get<bool>() );
    BOOST_REQUIRE_EQUAL( stackup["layers"].size(), 7 );
    BOOST_CHECK_EQUAL( stackup["layers"][0]["type"].get<std::string>(), "BSLT_COPPER" );
    BOOST_CHECK_EQUAL( stackup["layers"][1]["dielectricLayerId"].get<int>(), 1 );
    BOOST_CHECK_EQUAL(
            stackup["layers"][1]["dielectric"]["layer"][0]["thickness"]["valueNm"]
                    .get<std::string>(),
            "486000" );
    BOOST_CHECK_EQUAL( stackup["layers"][2]["layer"].get<std::string>(), "BL_In1_Cu" );
    BOOST_CHECK_EQUAL( stackup["layers"][6]["layer"].get<std::string>(), "BL_B_Cu" );
}


BOOST_AUTO_TEST_CASE( LowersCanonicalGlobalRulesIntoOneNativeMessage )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controlled_rules)
  (rules
    (minimum_clearance 0.2mm)
    (minimum_connection_width 0.15mm)
    (minimum_track_width 0.18mm)
    (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.6mm)
    (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.3mm)
    (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm)
    (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance -0.01mm)
    (minimum_groove_width 0.3mm)
    (minimum_resolved_spokes 3)
    (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance legacy)
    (use_height_for_length_calculations true)
    (maximum_error 0.005mm)
    (allow_fillets_outside_zone_outline false)))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["rules"].get<int>(), 1 );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.operations[0]["action"].get<std::string>(), "update_rules" );
    checkProtobufJson<kiapi::board::BoardDesignRules>( plan.operations[0]["rules"] );
    const nlohmann::json& rules = plan.operations[0]["rules"];
    BOOST_CHECK_EQUAL( rules["minimumClearance"]["valueNm"].get<std::string>(), "200000" );
    BOOST_CHECK_EQUAL( rules["minimumSilkscreenClearance"]["valueNm"].get<std::string>(),
                       "-10000" );
    BOOST_CHECK_EQUAL( rules["minimumResolvedSpokes"].get<int>(), 3 );
    BOOST_CHECK_EQUAL( rules["copperEdgeClearanceMode"].get<std::string>(), "BCECM_LEGACY" );
    BOOST_CHECK_EQUAL( rules["minimumCopperToEdgeClearance"]["valueNm"].get<std::string>(),
                       "0" );
    BOOST_CHECK( rules["useHeightForLengthCalculations"].get<bool>() );
    BOOST_CHECK_EQUAL( rules["maximumError"]["valueNm"].get<std::string>(), "5000" );
    BOOST_CHECK( !rules["allowFilletsOutsideZoneOutline"].get<bool>() );
}


BOOST_AUTO_TEST_CASE( LowersCanonicalNetclassesIntoOneCompleteNativeMessage )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controlled_netclasses)
  (net_classes
    (class Default
      (clearance 0.2mm) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile none) (pcb_color default)
      (wire_width 0.15mm) (bus_width 0.3mm)
      (schematic_color default) (line_style solid))
    (class USB_HS
      (clearance inherit) (track_width 0.15mm)
      (via_diameter inherit) (via_drill inherit)
      (microvia_diameter 0.25mm) (microvia_drill inherit)
      (diff_pair_width 0.15mm) (diff_pair_gap 0.18mm) (diff_pair_via_gap inherit)
      (tuning_profile usb_hs) (pcb_color "#112233CC")
      (wire_width inherit) (bus_width inherit)
      (schematic_color "#AABBCC") (line_style dash_dot))
    (assign (pattern "USB_[PN]") (classes USB_HS))
    (assign (pattern "/usb/D[PN]") (classes USB_HS))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_CHECK_EQUAL( plan.counts["netClasses"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( plan.counts["netClassAssignments"].get<int>(), 2 );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.operations[0]["action"].get<std::string>(),
                       "update_net_classes" );
    const nlohmann::json& settings = plan.operations[0]["settings"];
    checkProtobufJson<kiapi::common::project::NetClassSettings>( settings );
    BOOST_REQUIRE_EQUAL( settings["netClasses"].size(), 2 );
    BOOST_REQUIRE_EQUAL( settings["assignments"].size(), 2 );
    BOOST_CHECK_EQUAL( settings["netClasses"][0]["priority"].get<int64_t>(),
                       std::numeric_limits<int32_t>::max() );
    BOOST_CHECK_EQUAL( settings["netClasses"][1]["priority"].get<int64_t>(), 0 );
    BOOST_CHECK_EQUAL(
            settings["netClasses"][0]["board"]["viaStack"]["copperLayers"][0]["size"]
                    ["xNm"].get<std::string>(),
            "600000" );
    BOOST_CHECK_EQUAL(
            settings["netClasses"][0]["board"]["microviaStack"]["drill"]["diameter"]
                    ["xNm"].get<std::string>(),
            "100000" );
    BOOST_CHECK_EQUAL(
            settings["netClasses"][0]["schematic"]["wireWidth"]["valueNm"]
                    .get<std::string>(),
            "150000" );
    BOOST_CHECK_EQUAL( settings["netClasses"][1]["schematic"]["lineStyle"]
                               .get<std::string>(),
                       "SLS_DASHDOT" );
    BOOST_CHECK_CLOSE( settings["netClasses"][1]["board"]["color"]["r"].get<double>(),
                       17.0 / 255.0, 0.0001 );
    BOOST_CHECK_EQUAL( settings["assignments"][0]["netClass"].get<std::string>(),
                       "USB_HS" );
}


BOOST_AUTO_TEST_CASE( LowersCanonicalCustomRulesIntoOneGeneratedNativeDocument )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project controlled_custom_rules)
  (custom_rules
    (rule usb_diff_pair
      (condition "A.hasNetclass('USB_HS')")
      (layer outer)
      (severity error)
      (constraint diff_pair_gap (min 0.15mm) (opt 0.2mm) (max 0.25mm))
      (constraint skew (domain diff_pairs) (max 5ps)))
    (rule assembly_policy
      (condition always)
      (layer all)
      (severity warning)
      (constraint disallow (items track through_via pad footprint))
      (constraint assertion (test "A.Type != 'Footprint' || A.Orientation != 13deg"))
      (constraint solder_paste_rel_margin (ratio -0.1)))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.counts["customRules"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( plan.operations[0]["action"].get<std::string>(),
                       "update_custom_rules" );
    BOOST_CHECK( plan.operations[0]["customRules"]["present"].get<bool>() );
    const std::string generated =
            plan.operations[0]["customRules"]["source"].get<std::string>();
    const std::string expected = R"DRU((version 1)

(rule "usb_diff_pair"
  (condition "A.hasNetclass('USB_HS')")
  (layer outer)
  (severity error)
    (constraint diff_pair_gap (min 0.15mm) (opt 0.2mm) (max 0.25mm))
    (constraint skew (max 5000fs) (within_diff_pairs))
)

(rule "assembly_policy"
  (severity warning)
    (constraint disallow track through_via pad footprint)
    (constraint assertion "A.Type != 'Footprint' || A.Orientation != 13deg")
    (constraint solder_paste_rel_margin (opt -100))
)
)DRU";
    BOOST_CHECK_EQUAL( generated, expected );
}


BOOST_AUTO_TEST_CASE( LowersExplicitCopperZoneIntoDeterministicProtobufJson )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project zone_board)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1 1) (pin R2 1 1))
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


BOOST_AUTO_TEST_CASE( LowersExplicitKeepoutIntoRuleAreaProtobufJson )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project rule_area_board)
  (board
    (keepout
      (id no-routing)
      (name "No routing")
      (layers F.Cu B.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 10mm 0mm) (point 10mm 5mm) (point 0mm 5mm)))
      (prohibit
        (copper true) (vias true) (tracks true) (pads false) (footprints false))
      (border diagonal_full (pitch 0.4mm))
      (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.operations[0]["itemType"].get<std::string>(), "rule_area" );
    checkProtobufJson<kiapi::board::types::Zone>( plan.operations[0]["item"] );
    const nlohmann::json& item = plan.operations[0]["item"];
    BOOST_CHECK_EQUAL( item["type"].get<std::string>(), "ZT_RULE_AREA" );
    BOOST_CHECK( item["ruleAreaSettings"]["keepoutCopper"].get<bool>() );
    BOOST_CHECK( item["ruleAreaSettings"]["keepoutTracks"].get<bool>() );
    BOOST_CHECK( !item["ruleAreaSettings"]["keepoutPads"].get<bool>() );
    BOOST_CHECK( !item["ruleAreaSettings"]["placementEnabled"].get<bool>() );
    BOOST_CHECK_EQUAL( item["ruleAreaSettings"]["placementSourceType"].get<std::string>(),
                       "PRST_UNKNOWN" );
    BOOST_CHECK_EQUAL( item["border"]["pitch"]["valueNm"].get<std::string>(), "400000" );
    BOOST_CHECK_EQUAL( item["locked"].get<std::string>(), "LS_LOCKED" );
    BOOST_CHECK_EQUAL( plan.operations[0]["itemId"].get<std::string>(),
                       KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                               "rule_area_board", "rule_area", "no-routing" ) );
}


BOOST_AUTO_TEST_CASE( LowersBoardTextIntoDeterministicProtobufJson )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project annotated_board)
  (board
    (text "Fab\nrevision B"
      (id fab-note)
      (layer F.Fab)
      (at 4mm 6mm)
      (size 1mm 1.5mm)
      (stroke 0.2mm)
      (angle -45deg)
      (justify right bottom)
      (font "Noto Sans")
      (line_spacing 1.2)
      (bold true)
      (italic true)
      (underlined true)
      (mirrored true)
      (keep_upright true)
      (hyperlink "https://example.com/fab")
      (knockout true)
      (locked true))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 1 );
    BOOST_CHECK_EQUAL( plan.operations[0]["itemType"].get<std::string>(), "text" );
    checkProtobufJson<kiapi::board::types::BoardText>( plan.operations[0]["item"] );
    const nlohmann::json& item = plan.operations[0]["item"];
    BOOST_CHECK_EQUAL( item["layer"].get<std::string>(), "BL_F_Fab" );
    BOOST_CHECK_EQUAL( item["text"]["text"].get<std::string>(), "Fab\nrevision B" );
    BOOST_CHECK_EQUAL( item["text"]["attributes"]["fontName"].get<std::string>(),
                       "Noto Sans" );
    BOOST_CHECK_EQUAL( item["text"]["attributes"]["horizontalAlignment"].get<std::string>(),
                       "HA_RIGHT" );
    BOOST_CHECK_EQUAL( item["text"]["attributes"]["verticalAlignment"].get<std::string>(),
                       "VA_BOTTOM" );
    BOOST_CHECK_EQUAL( item["text"]["attributes"]["strokeWidth"]["valueNm"].get<std::string>(),
                       "200000" );
    BOOST_CHECK( item["text"]["attributes"]["multiline"].get<bool>() );
    BOOST_CHECK( item["knockout"].get<bool>() );
    BOOST_CHECK_EQUAL( item["locked"].get<std::string>(), "LS_LOCKED" );
    BOOST_CHECK_EQUAL( plan.operations[0]["itemId"].get<std::string>(),
                       KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                               "annotated_board", "text", "fab-note" ) );
}


BOOST_AUTO_TEST_CASE( LowersEveryDimensionStyleIntoOneNativeProtobufType )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project dimensions)
  (board
    (dimension aligned
      (id a) (layer Dwgs.User) (from 0mm 0mm) (to 10mm 0mm)
      (height 2mm) (extension_height 0.2mm)
      (units mm) (unit_format bare_suffix) (precision fixed_2)
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0.1mm)
      (arrow_direction inward) (text_position inline)
      (text_size 1mm 1mm) (text_stroke 0.15mm))
    (dimension orthogonal
      (id o) (layer Dwgs.User) (from 0mm 0mm) (to 0mm 10mm)
      (height 2mm) (extension_height 0.2mm) (axis y)
      (units automatic) (unit_format no_suffix) (precision scaled_in_3)
      (line_width 0.15mm) (arrow_length 0.8mm) (extension_offset 0mm)
      (arrow_direction outward) (text_position outside)
      (text_size 1mm 1mm) (text_stroke 0.15mm))
    (dimension radial
      (id r) (layer Dwgs.User) (center 20mm 20mm) (radius_point 25mm 20mm)
      (leader_length 2mm) (units mm) (unit_format paren_suffix) (precision fixed_3)
      (line_width 0.15mm) (arrow_length 0.8mm) (text_at 28mm 22mm)
      (text_size 1mm 1mm) (text_stroke 0.15mm))
    (dimension leader
      (id l) (layer Dwgs.User) (from 5mm 5mm) (to 8mm 5mm)
      (border circle) (label "CHECK") (line_width 0.15mm) (arrow_length 0.8mm)
      (text_at 11mm 5mm) (text_size 1mm 1mm) (text_stroke 0.15mm))
    (dimension center
      (id c) (layer Dwgs.User) (center 30mm 30mm) (to 33mm 30mm)
      (line_width 0.15mm) (arrow_length 0.8mm))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
    KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT plan =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
    BOOST_REQUIRE_MESSAGE( plan.fullyLowered, plan.diagnostics.dump() );
    BOOST_REQUIRE_EQUAL( plan.operations.size(), 5 );

    for( const nlohmann::json& operation : plan.operations )
    {
        BOOST_CHECK_EQUAL( operation["itemType"].get<std::string>(), "dimension" );
        checkProtobufJson<kiapi::board::types::Dimension>( operation["item"] );
    }

    BOOST_CHECK( plan.operations[0]["item"].contains( "aligned" ) );
    BOOST_CHECK_EQUAL( plan.operations[0]["item"]["arrowDirection"].get<std::string>(),
                       "DAD_INWARD" );
    BOOST_CHECK_EQUAL( plan.operations[1]["item"]["orthogonal"]["alignment"].get<std::string>(),
                       "AA_Y_AXIS" );
    BOOST_CHECK_EQUAL( plan.operations[2]["item"]["radial"]["leaderLength"]["valueNm"]
                               .get<std::string>(),
                       "2000000" );
    BOOST_CHECK_EQUAL( plan.operations[3]["item"]["leader"]["borderStyle"].get<std::string>(),
                       "DTBS_CIRCLE" );
    BOOST_CHECK_EQUAL( plan.operations[3]["item"]["overrideText"].get<std::string>(), "CHECK" );
    BOOST_CHECK( plan.operations[4]["item"].contains( "center" ) );
    BOOST_CHECK_EQUAL( plan.operations[0]["itemId"].get<std::string>(),
                       KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                               "dimensions", "dimension", "a" ) );
}


BOOST_AUTO_TEST_CASE( FullyLowersTypedComponentPlacement )
{
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project placed_board)
  (sheet root (parent none) (file "placed_board.kicad_sch") (title "Root"))
  (component U1 (symbol "Device:R") (value "1k") (footprint "R:R")
    (unit 1 (sheet root) (at 20mm 20mm) (rotation 0deg) (mirror none)))
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
    BOOST_REQUIRE( plan.operations[0].contains( "instance" ) );
    BOOST_CHECK_EQUAL( plan.operations[0]["instance"]["libraryId"].get<std::string>(),
                       "R:R" );
    BOOST_CHECK_EQUAL( plan.operations[0]["instance"]["value"].get<std::string>(), "1k" );
    BOOST_CHECK_EQUAL( plan.operations[0]["instance"]["symbolSheetFilename"]
                               .get<std::string>(),
                       "placed_board.kicad_sch" );
    BOOST_REQUIRE_EQUAL( plan.operations[0]["instance"]["symbolPath"].size(), 1 );
    BOOST_CHECK_EQUAL(
            plan.operations[0]["instance"]["symbolPath"][0].get<std::string>(),
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    "placed_board", "schematic_symbol", "U1/1" ) );
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
