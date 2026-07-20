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

#include <kicad/codex/board_render_artifact_validator.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <kiid.h>
#include <wx/filename.h>
#include <zlib.h>


namespace
{

void appendBigEndian32( std::string& aOutput, uint32_t aValue )
{
    aOutput.push_back( static_cast<char>( aValue >> 24 ) );
    aOutput.push_back( static_cast<char>( aValue >> 16 ) );
    aOutput.push_back( static_cast<char>( aValue >> 8 ) );
    aOutput.push_back( static_cast<char>( aValue ) );
}


void appendChunk( std::string& aOutput, const char* aType,
                  const std::string& aData )
{
    appendBigEndian32( aOutput, static_cast<uint32_t>( aData.size() ) );
    const size_t crcBegin = aOutput.size();
    aOutput.append( aType, 4 );
    aOutput += aData;
    uLong crc = crc32( 0L, Z_NULL, 0 );
    crc = crc32( crc,
                 reinterpret_cast<const Bytef*>( aOutput.data() + crcBegin ),
                 static_cast<uInt>( 4 + aData.size() ) );
    appendBigEndian32( aOutput, static_cast<uint32_t>( crc ) );
}


std::string renderPng( bool aVisibleBoard )
{
    constexpr size_t WIDTH = 1008;
    constexpr size_t HEIGHT = 1008;
    constexpr size_t ROW_BYTES = 1 + WIDTH * 4;
    std::string raw( ROW_BYTES * HEIGHT, '\0' );

    if( aVisibleBoard )
    {
        for( size_t y = 500; y < 504; ++y )
        {
            for( size_t x = 500; x < 504; ++x )
            {
                const size_t pixel = y * ROW_BYTES + 1 + x * 4;
                raw[pixel] = static_cast<char>( 0x22 );
                raw[pixel + 1] = static_cast<char>( 0x88 );
                raw[pixel + 2] = static_cast<char>( 0x44 );
                raw[pixel + 3] = static_cast<char>( 0xFF );
            }
        }
    }

    uLongf compressedBytes = compressBound( static_cast<uLong>( raw.size() ) );
    std::string compressed( compressedBytes, '\0' );
    BOOST_REQUIRE_EQUAL(
            compress2( reinterpret_cast<Bytef*>( compressed.data() ), &compressedBytes,
                       reinterpret_cast<const Bytef*>( raw.data() ),
                       static_cast<uLong>( raw.size() ), Z_BEST_COMPRESSION ),
            Z_OK );
    compressed.resize( compressedBytes );

    std::string png( "\x89PNG\r\n\x1A\n", 8 );
    std::string header;
    appendBigEndian32( header, WIDTH );
    appendBigEndian32( header, HEIGHT );
    header.append( "\x08\x06\x00\x00\x00", 5 );
    appendChunk( png, "IHDR", header );
    appendChunk( png, "IDAT", compressed );
    appendChunk( png, "IEND", {} );
    return png;
}


class RENDER_FIXTURE
{
public:
    RENDER_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-board-render-validator-"
                     + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~RENDER_FIXTURE()
    {
        std::error_code ignored;
        std::filesystem::remove_all( m_root, ignored );
    }

    std::filesystem::path Write( const std::string& aName,
                                 const std::string& aSource )
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


BOOST_AUTO_TEST_SUITE( BoardRenderArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsCompleteVisiblePngAndRejectsCorruptionOrBlankImage )
{
    RENDER_FIXTURE fixture;
    const std::string valid = renderPng( true );
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::BOARD_RENDER_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "controller-board-render.png", valid ),
                    "controller", error ), error );

    error.clear();
    std::string corrupt = valid;
    corrupt.back() ^= 1;
    BOOST_CHECK( !KICHAD::BOARD_RENDER_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "controller-board-render.png", corrupt ),
            "controller", error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    BOOST_CHECK( !KICHAD::BOARD_RENDER_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "controller-board-render.png", renderPng( false ) ),
            "controller", error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
