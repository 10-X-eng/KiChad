/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <boost/test/unit_test.hpp>

#include <kicad/codex/u3d_artifact_validator.h>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <kiid.h>
#include <wx/filename.h>


namespace
{

void append16( std::string& aOutput, uint16_t aValue )
{
    aOutput.push_back( static_cast<char>( aValue ) );
    aOutput.push_back( static_cast<char>( aValue >> 8 ) );
}


void append32( std::string& aOutput, uint32_t aValue )
{
    for( size_t byte = 0; byte < 4; ++byte )
        aOutput.push_back( static_cast<char>( aValue >> ( byte * 8 ) ) );
}


void append64( std::string& aOutput, uint64_t aValue )
{
    append32( aOutput, static_cast<uint32_t>( aValue ) );
    append32( aOutput, static_cast<uint32_t>( aValue >> 32 ) );
}


void appendBlock( std::string& aOutput, uint32_t aType, std::string aData )
{
    append32( aOutput, aType );
    append32( aOutput, static_cast<uint32_t>( aData.size() ) );
    append32( aOutput, 0 );
    aOutput += aData;

    while( aOutput.size() % 4 != 0 )
        aOutput.push_back( '\0' );
}


std::string validU3d()
{
    std::string header;
    append16( header, 256 );
    append16( header, 0 );
    append32( header, 8 );
    append32( header, 88 );
    append64( header, 104 );
    append32( header, 106 );
    append64( header,
              std::bit_cast<uint64_t>( static_cast<double>( 0.001F ) ) );

    std::string result;
    appendBlock( result, 0x00443355, header );
    appendBlock( result, 0xFFFFFF14, std::string( "groups\0\0", 8 ) );
    appendBlock( result, 0xFFFFFF53, std::string( 4, '\0' ) );
    appendBlock( result, 0xFFFFFF54, std::string( 4, '\0' ) );
    appendBlock( result, 0xFFFFFF3B, std::string( 4, '\0' ) );
    return result;
}


class U3D_FIXTURE
{
public:
    U3D_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-u3d-validator-" + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~U3D_FIXTURE()
    {
        std::error_code ignored;
        std::filesystem::remove_all( m_root, ignored );
    }

    std::filesystem::path Write( const std::string& aName, const std::string& aSource )
    {
        const std::filesystem::path path = m_root / aName;
        std::ofstream output( path, std::ios::binary | std::ios::trunc );
        output.write( aSource.data(), static_cast<std::streamsize>( aSource.size() ) );
        output.close();
        BOOST_REQUIRE( output.good() );
        return path;
    }

private:
    std::filesystem::path m_root;
};

} // namespace


BOOST_AUTO_TEST_SUITE( U3dArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsBoundedEcma363SceneAndRejectsDeclaredSizeDrift )
{
    U3D_FIXTURE fixture;
    const std::string valid = validU3d();
    std::string error;
    BOOST_REQUIRE_EQUAL( valid.size(), 112 );
    BOOST_CHECK_MESSAGE(
            KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "valid.u3d", valid ), error ), error );

    error.clear();
    std::string drifted = valid;
    drifted[24] = static_cast<char>( drifted[24] + 1 );
    BOOST_CHECK( !KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "drifted.u3d", drifted ), error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string wrongUnits = valid;
    wrongUnits[36] = static_cast<char>( wrongUnits[36] ^ 0x01 );
    BOOST_CHECK( !KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "wrong-units.u3d", wrongUnits ), error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string wrongContinuation = valid;
    wrongContinuation[96] = static_cast<char>( 0x54 );
    BOOST_CHECK( !KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "wrong-continuation.u3d", wrongContinuation ), error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
