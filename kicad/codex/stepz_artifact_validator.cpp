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

#include "stepz_artifact_validator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>

#include <zlib.h>


namespace
{

constexpr uintmax_t MAX_STEPZ_COMPRESSED_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr uintmax_t MAX_STEPZ_EXPANDED_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr size_t SCAN_WINDOW_BYTES = 8192;


std::string_view trim( std::string_view aText )
{
    while( !aText.empty()
           && std::isspace( static_cast<unsigned char>( aText.front() ) ) )
    {
        aText.remove_prefix( 1 );
    }

    while( !aText.empty()
           && std::isspace( static_cast<unsigned char>( aText.back() ) ) )
    {
        aText.remove_suffix( 1 );
    }

    return aText;
}

} // namespace


bool KICHAD::STEPZ_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes < 18 || bytes > MAX_STEPZ_COMPRESSED_BYTES )
    {
        aError = "STEPZ artifact has an invalid compressed size";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string compressed{ std::istreambuf_iterator<char>( input ),
                            std::istreambuf_iterator<char>() };

    if( input.bad() || compressed.size() != bytes
        || static_cast<unsigned char>( compressed[0] ) != 0x1F
        || static_cast<unsigned char>( compressed[1] ) != 0x8B
        || static_cast<unsigned char>( compressed[2] ) != Z_DEFLATED
        || ( static_cast<unsigned char>( compressed[3] ) & 0xE0 ) != 0 )
    {
        aError = "STEPZ artifact has an invalid gzip container";
        return false;
    }

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>( compressed.data() );
    stream.avail_in = static_cast<uInt>( compressed.size() );

    if( inflateInit2( &stream, MAX_WBITS + 16 ) != Z_OK )
    {
        aError = "STEPZ gzip validator could not initialize";
        return false;
    }

    const std::string description = "FILE_DESCRIPTION(('KiCad electronic assembly')";
    const std::string fileName = "FILE_NAME('" + aExpectedStem + ".stpz'";
    static const std::string producer = "'KiCad to STEP converter'";
    std::array<unsigned char, 64 * 1024> output{};
    std::string prefix;
    std::string window;
    std::string suffix;
    uintmax_t expandedBytes = 0;
    bool foundDescription = false;
    bool foundFileName = false;
    bool foundProducer = false;
    int status = Z_OK;

    do
    {
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>( output.size() );
        status = inflate( &stream, Z_NO_FLUSH );

        if( status != Z_OK && status != Z_STREAM_END )
            break;

        const size_t produced = output.size() - stream.avail_out;
        expandedBytes += produced;

        if( expandedBytes > MAX_STEPZ_EXPANDED_BYTES
            || std::find( output.begin(), output.begin() + produced, '\0' )
                       != output.begin() + produced )
        {
            status = Z_MEM_ERROR;
            break;
        }

        const std::string_view chunk( reinterpret_cast<const char*>( output.data() ),
                                      produced );

        if( prefix.size() < SCAN_WINDOW_BYTES )
        {
            prefix.append( chunk.substr(
                    0, std::min( SCAN_WINDOW_BYTES - prefix.size(), chunk.size() ) ) );
        }

        window.append( chunk );
        foundDescription = foundDescription || window.find( description ) != std::string::npos;
        foundFileName = foundFileName || window.find( fileName ) != std::string::npos;
        foundProducer = foundProducer || window.find( producer ) != std::string::npos;

        if( window.size() > SCAN_WINDOW_BYTES )
            window.erase( 0, window.size() - SCAN_WINDOW_BYTES );

        suffix.append( chunk );

        if( suffix.size() > SCAN_WINDOW_BYTES )
            suffix.erase( 0, suffix.size() - SCAN_WINDOW_BYTES );
    } while( status != Z_STREAM_END );

    const bool consumedExactly = status == Z_STREAM_END && stream.avail_in == 0;
    inflateEnd( &stream );

    if( !consumedExactly || expandedBytes == 0
        || !prefix.starts_with( "ISO-10303-21;\nHEADER;" )
        || !trim( suffix ).ends_with( "END-ISO-10303-21;" )
        || !foundDescription || !foundFileName || !foundProducer )
    {
        aError = "STEPZ artifact is not one complete native KiCad STEP stream";
        return false;
    }

    return true;
}
