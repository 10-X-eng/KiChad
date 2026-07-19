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

#include <import_export.h>
#include <api/common/types/project_settings.pb.h>
#include <base_units.h>
#include <google/protobuf/any.pb.h>
#include <netclass.h>


BOOST_AUTO_TEST_SUITE( NetclassSerialization )


BOOST_AUTO_TEST_CASE( RoundTripsEveryExplicitNativeFieldWithoutUnitLoss )
{
    NETCLASS original( wxS( "Controlled" ), false );
    original.SetPriority( 2 );
    original.SetClearance( pcbIUScale.mmToIU( 0.25 ) );
    original.SetTrackWidth( pcbIUScale.mmToIU( 0.2 ) );
    original.SetViaDiameter( pcbIUScale.mmToIU( 0.6 ) );
    original.SetViaDrill( pcbIUScale.mmToIU( 0.3 ) );
    original.SetuViaDiameter( pcbIUScale.mmToIU( 0.3 ) );
    original.SetuViaDrill( pcbIUScale.mmToIU( 0.1 ) );
    original.SetDiffPairWidth( pcbIUScale.mmToIU( 0.18 ) );
    original.SetDiffPairGap( pcbIUScale.mmToIU( 0.2 ) );
    original.SetDiffPairViaGap( pcbIUScale.mmToIU( 0.22 ) );
    original.SetPcbColor( KIGFX::COLOR4D( 0.1, 0.2, 0.3, 1.0 ) );
    original.SetTuningProfile( wxS( "usb_hs" ) );
    original.SetWireWidth( schIUScale.mmToIU( 0.15 ) );
    original.SetBusWidth( schIUScale.mmToIU( 0.3 ) );
    original.SetSchematicColor( KIGFX::COLOR4D( 0.4, 0.5, 0.6, 1.0 ) );
    original.SetLineStyle( static_cast<int>( LINE_STYLE::DASHDOT ) );

    google::protobuf::Any container;
    original.Serialize( container );
    kiapi::common::project::NetClass encoded;
    BOOST_REQUIRE( container.UnpackTo( &encoded ) );
    BOOST_CHECK_EQUAL( encoded.type(), kiapi::common::project::NCT_EXPLICIT );
    BOOST_CHECK_EQUAL( encoded.constituents_size(), 0 );
    BOOST_REQUIRE( encoded.board().has_microvia_stack() );
    BOOST_CHECK_EQUAL(
            encoded.board().microvia_stack().copper_layers( 0 ).size().x_nm(), 300000 );
    BOOST_CHECK_EQUAL(
            encoded.board().microvia_stack().drill().diameter().x_nm(), 100000 );
    BOOST_CHECK_EQUAL( encoded.schematic().wire_width().value_nm(), 150000 );
    BOOST_CHECK_EQUAL( encoded.schematic().bus_width().value_nm(), 300000 );
    BOOST_CHECK_EQUAL( encoded.schematic().line_style(), kiapi::common::types::SLS_DASHDOT );

    NETCLASS restored( wxS( "restored" ), false );
    BOOST_REQUIRE( restored.Deserialize( container ) );
    BOOST_CHECK( restored.GetName() == wxS( "Controlled" ) );
    BOOST_CHECK_EQUAL( restored.GetuViaDiameter(), pcbIUScale.mmToIU( 0.3 ) );
    BOOST_CHECK_EQUAL( restored.GetuViaDrill(), pcbIUScale.mmToIU( 0.1 ) );
    BOOST_CHECK_EQUAL( restored.GetWireWidth(), schIUScale.mmToIU( 0.15 ) );
    BOOST_CHECK_EQUAL( restored.GetBusWidth(), schIUScale.mmToIU( 0.3 ) );
    BOOST_CHECK_EQUAL( restored.GetLineStyle(), static_cast<int>( LINE_STYLE::DASHDOT ) );

    google::protobuf::Any reserialized;
    restored.Serialize( reserialized );
    kiapi::common::project::NetClass normalized;
    BOOST_REQUIRE( reserialized.UnpackTo( &normalized ) );
    BOOST_CHECK_EQUAL( normalized.type(), kiapi::common::project::NCT_EXPLICIT );
    BOOST_CHECK_EQUAL( normalized.constituents_size(), 0 );
}


BOOST_AUTO_TEST_CASE( DistinguishesImplicitCompositeClassesAndRejectsUnknownStyles )
{
    NETCLASS first( wxS( "First" ), false );
    NETCLASS second( wxS( "Second" ), false );
    NETCLASS composite( wxS( "effective" ), false );
    composite.SetConstituentNetclasses( { &first, &second } );
    google::protobuf::Any container;
    composite.Serialize( container );
    kiapi::common::project::NetClass encoded;
    BOOST_REQUIRE( container.UnpackTo( &encoded ) );
    BOOST_CHECK_EQUAL( encoded.type(), kiapi::common::project::NCT_IMPLICIT );
    BOOST_REQUIRE_EQUAL( encoded.constituents_size(), 2 );
    BOOST_CHECK_EQUAL( encoded.constituents( 0 ), "First" );
    BOOST_CHECK_EQUAL( encoded.constituents( 1 ), "Second" );

    kiapi::common::project::NetClass invalid;
    invalid.set_name( "Invalid" );
    invalid.set_type( kiapi::common::project::NCT_EXPLICIT );
    invalid.mutable_schematic()->set_line_style( kiapi::common::types::SLS_UNKNOWN );
    container.PackFrom( invalid );
    NETCLASS rejected( wxS( "Rejected" ), false );
    BOOST_CHECK( !rejected.Deserialize( container ) );
}


BOOST_AUTO_TEST_SUITE_END()
