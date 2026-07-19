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

#include <board_stackup_manager/board_stackup.h>

#include <api/board/board.pb.h>
#include <google/protobuf/any.pb.h>


namespace
{

using namespace kiapi::board;
using kiapi::board::types::BoardLayer;


BoardStackupLayer* addTechnicalLayer( BoardStackup& aStackup, BoardStackupLayerType aType,
                                      BoardLayer aLayer, const std::string& aTypeName )
{
    BoardStackupLayer* layer = aStackup.add_layers();
    layer->set_type( aType );
    layer->set_layer( aLayer );
    layer->set_type_name( aTypeName );
    layer->set_enabled( true );
    return layer;
}


BoardStackup makeStackup()
{
    BoardStackup stackup;
    stackup.mutable_finish()->set_type_name( "ENIG" );
    stackup.mutable_impedance()->set_is_controlled( true );
    stackup.mutable_edge()->mutable_connector()->set_bevelled( true );
    stackup.mutable_edge()->mutable_plating()->set_has_edge_plating( true );

    BoardStackupLayer* layer = addTechnicalLayer(
            stackup, BSLT_SILKSCREEN, BoardLayer::BL_F_SilkS, "Top Silk Screen" );
    layer->set_material_name( "Epoxy ink" );
    layer->set_color_name( "White" );

    addTechnicalLayer( stackup, BSLT_SOLDERPASTE, BoardLayer::BL_F_Paste,
                       "Top Solder Paste" );

    layer = addTechnicalLayer( stackup, BSLT_SOLDERMASK, BoardLayer::BL_F_Mask,
                               "Top Solder Mask" );
    layer->mutable_thickness()->set_value_nm( 10000 );
    layer->set_material_name( "LPI" );
    layer->set_color_name( "Green" );
    layer->set_epsilon_r( 3.5 );
    layer->set_loss_tangent( 0.025 );

    layer = addTechnicalLayer( stackup, BSLT_COPPER, BoardLayer::BL_F_Cu, "copper" );
    layer->mutable_thickness()->set_value_nm( 35000 );

    layer = addTechnicalLayer( stackup, BSLT_DIELECTRIC, BoardLayer::BL_UNDEFINED, "core" );
    layer->set_dielectric_layer_id( 1 );
    layer->mutable_thickness()->set_value_nm( 1530000 );
    BoardStackupDielectricProperties* dielectric = layer->mutable_dielectric()->add_layer();
    dielectric->mutable_thickness()->set_value_nm( 700000 );
    dielectric->set_material_name( "FR408HR" );
    dielectric->set_epsilon_r( 3.68 );
    dielectric->set_loss_tangent( 0.0092 );
    dielectric->set_thickness_locked( true );
    dielectric->set_color_name( "Amber" );
    dielectric = layer->mutable_dielectric()->add_layer();
    dielectric->mutable_thickness()->set_value_nm( 830000 );
    dielectric->set_material_name( "FR408HR prepreg" );
    dielectric->set_epsilon_r( 3.6 );
    dielectric->set_loss_tangent( 0.009 );
    dielectric->set_thickness_locked( false );
    dielectric->set_color_name( "Natural" );

    layer = addTechnicalLayer( stackup, BSLT_COPPER, BoardLayer::BL_B_Cu, "copper" );
    layer->mutable_thickness()->set_value_nm( 35000 );

    layer = addTechnicalLayer( stackup, BSLT_SOLDERMASK, BoardLayer::BL_B_Mask,
                               "Bottom Solder Mask" );
    layer->mutable_thickness()->set_value_nm( 10000 );
    layer->set_material_name( "LPI" );
    layer->set_color_name( "Green" );
    layer->set_epsilon_r( 3.5 );
    layer->set_loss_tangent( 0.025 );

    addTechnicalLayer( stackup, BSLT_SOLDERPASTE, BoardLayer::BL_B_Paste,
                       "Bottom Solder Paste" );

    layer = addTechnicalLayer( stackup, BSLT_SILKSCREEN, BoardLayer::BL_B_SilkS,
                               "Bottom Silk Screen" );
    layer->set_material_name( "Epoxy ink" );
    layer->set_color_name( "White" );
    return stackup;
}


BoardStackup makeFourLayerStackup()
{
    BoardStackup stackup;
    stackup.mutable_finish()->set_type_name( "ENIG" );
    stackup.mutable_impedance()->set_is_controlled( true );
    stackup.mutable_edge()->mutable_plating()->set_has_edge_plating( false );
    const auto addCopper = [&]( BoardLayer aLayer )
    {
        BoardStackupLayer* copper = addTechnicalLayer(
                stackup, BSLT_COPPER, aLayer, "copper" );
        copper->mutable_thickness()->set_value_nm( 35000 );
    };
    const auto addDielectric = [&]( uint32_t aIndex, int64_t aThickness,
                                    const std::string& aType )
    {
        BoardStackupLayer* layer = addTechnicalLayer(
                stackup, BSLT_DIELECTRIC, BoardLayer::BL_UNDEFINED, aType );
        layer->set_dielectric_layer_id( aIndex );
        layer->mutable_thickness()->set_value_nm( aThickness );
        BoardStackupDielectricProperties* properties =
                layer->mutable_dielectric()->add_layer();
        properties->mutable_thickness()->set_value_nm( aThickness );
        properties->set_material_name( "FR4" );
        properties->set_epsilon_r( 4.5 );
        properties->set_loss_tangent( 0.02 );
        properties->set_thickness_locked( true );
    };
    addCopper( BoardLayer::BL_F_Cu );
    addDielectric( 1, 486000, "core" );
    addCopper( BoardLayer::BL_In1_Cu );
    addDielectric( 2, 486000, "prepreg" );
    addCopper( BoardLayer::BL_In2_Cu );
    addDielectric( 3, 488000, "core" );
    addCopper( BoardLayer::BL_B_Cu );
    return stackup;
}

} // namespace


BOOST_AUTO_TEST_SUITE( BoardStackupSerialization )


BOOST_AUTO_TEST_CASE( RoundTripsEveryNativeFabricationProperty )
{
    BoardStackup source = makeStackup();
    google::protobuf::Any packed;
    packed.PackFrom( source );
    BOARD_STACKUP native;
    BOOST_REQUIRE( native.Deserialize( packed ) );
    BOOST_CHECK_EQUAL( native.GetCount(), 9 );
    BOOST_CHECK_EQUAL( native.BuildBoardThicknessFromStackup(), 1620000 );
    BOOST_CHECK_EQUAL( native.m_FinishType, "ENIG" );
    BOOST_CHECK( native.m_HasDielectricConstrains );
    BOOST_CHECK_EQUAL( native.m_EdgeConnectorConstraints, BS_EDGE_CONNECTOR_BEVELLED );
    BOOST_CHECK( native.m_EdgePlating );

    google::protobuf::Any normalized;
    native.Serialize( normalized );
    BoardStackup roundTrip;
    BOOST_REQUIRE( normalized.UnpackTo( &roundTrip ) );
    BOOST_REQUIRE_EQUAL( roundTrip.layers_size(), 9 );
    BOOST_CHECK_EQUAL( roundTrip.finish().type_name(), "ENIG" );
    BOOST_CHECK( roundTrip.impedance().is_controlled() );
    BOOST_CHECK( roundTrip.edge().connector().bevelled() );
    BOOST_CHECK( roundTrip.edge().plating().has_edge_plating() );
    BOOST_CHECK_EQUAL( roundTrip.layers( 0 ).material_name(), "Epoxy ink" );
    BOOST_CHECK_EQUAL( roundTrip.layers( 0 ).color_name(), "White" );
    BOOST_CHECK_EQUAL( roundTrip.layers( 4 ).type_name(), "core" );
    BOOST_CHECK_EQUAL( roundTrip.layers( 4 ).dielectric_layer_id(), 1 );
    BOOST_REQUIRE_EQUAL( roundTrip.layers( 4 ).dielectric().layer_size(), 2 );
    BOOST_CHECK( roundTrip.layers( 4 ).dielectric().layer( 0 ).thickness_locked() );
    BOOST_CHECK_EQUAL( roundTrip.layers( 4 ).dielectric().layer( 0 ).color_name(), "Amber" );
    BOOST_CHECK_EQUAL( roundTrip.layers( 4 ).dielectric().layer( 1 ).material_name(),
                       "FR408HR prepreg" );
    BOOST_CHECK_EQUAL( roundTrip.layers( 4 ).dielectric().layer( 1 ).color_name(), "Natural" );
    BOOST_CHECK( !roundTrip.layers( 4 ).dielectric().layer( 1 ).thickness_locked() );
    BOOST_CHECK_CLOSE( roundTrip.layers( 6 ).epsilon_r(), 3.5, 0.0001 );
}


BOOST_AUTO_TEST_CASE( RejectsInvalidOrderWithoutChangingTheDestination )
{
    BoardStackup source = makeStackup();
    google::protobuf::Any packed;
    packed.PackFrom( source );
    BOARD_STACKUP native;
    BOOST_REQUIRE( native.Deserialize( packed ) );
    const BOARD_STACKUP before = native;

    source.mutable_layers( 5 )->set_layer( BoardLayer::BL_In1_Cu );
    packed.PackFrom( source );
    BOOST_CHECK( !native.Deserialize( packed ) );
    BOOST_CHECK( native == before );

    source = makeStackup();
    source.mutable_layers( 4 )->mutable_thickness()->set_value_nm( 1529000 );
    packed.PackFrom( source );
    BOOST_CHECK( !native.Deserialize( packed ) );
    BOOST_CHECK( native == before );
}


BOOST_AUTO_TEST_CASE( AcceptsSequentialInnerCopperLayers )
{
    BoardStackup source = makeFourLayerStackup();
    google::protobuf::Any packed;
    packed.PackFrom( source );
    BOARD_STACKUP native;
    BOOST_REQUIRE( native.Deserialize( packed ) );
    BOOST_CHECK_EQUAL( native.GetCount(), 7 );
    BOOST_CHECK_EQUAL( native.BuildBoardThicknessFromStackup(), 1600000 );
    BOOST_CHECK_EQUAL( native.GetStackupLayer( 2 )->GetBrdLayerId(), In1_Cu );
    BOOST_CHECK_EQUAL( native.GetStackupLayer( 4 )->GetBrdLayerId(), In2_Cu );
}


BOOST_AUTO_TEST_SUITE_END()
