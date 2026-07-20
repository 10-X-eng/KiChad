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

#include "board_render_artifact_validator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include <zlib.h>


namespace
{

constexpr uintmax_t MAX_BOARD_RENDER_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_BOARD_RENDER_CHUNKS = 100'000;
constexpr uint32_t EXPECTED_WIDTH = 1008;
constexpr uint32_t EXPECTED_HEIGHT = 1008;
constexpr size_t BYTES_PER_PIXEL = 4;
constexpr std::array<unsigned char, 8> PNG_SIGNATURE = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'
};


uint32_t bigEndian32( const unsigned char* aBytes )
{
    return static_cast<uint32_t>( aBytes[0] ) << 24
           | static_cast<uint32_t>( aBytes[1] ) << 16
           | static_cast<uint32_t>( aBytes[2] ) << 8
           | static_cast<uint32_t>( aBytes[3] );
}


unsigned char paeth( unsigned char aLeft, unsigned char aUp,
                     unsigned char aUpLeft )
{
    const int prediction = static_cast<int>( aLeft ) + static_cast<int>( aUp )
                           - static_cast<int>( aUpLeft );
    const int leftDistance = std::abs( prediction - static_cast<int>( aLeft ) );
    const int upDistance = std::abs( prediction - static_cast<int>( aUp ) );
    const int upLeftDistance = std::abs( prediction - static_cast<int>( aUpLeft ) );

    if( leftDistance <= upDistance && leftDistance <= upLeftDistance )
        return aLeft;

    return upDistance <= upLeftDistance ? aUp : aUpLeft;
}


bool validatePixels( const std::vector<unsigned char>& aInflated,
                     std::string& aError )
{
    constexpr size_t ROW_BYTES = EXPECTED_WIDTH * BYTES_PER_PIXEL;
    constexpr size_t FILTERED_ROW_BYTES = ROW_BYTES + 1;

    if( aInflated.size() != FILTERED_ROW_BYTES * EXPECTED_HEIGHT )
    {
        aError = "board render PNG has the wrong decompressed image size";
        return false;
    }

    std::vector<unsigned char> previous( ROW_BYTES, 0 );
    std::vector<unsigned char> current( ROW_BYTES, 0 );
    std::array<unsigned char, BYTES_PER_PIXEL> firstPixel{};
    size_t visiblePixels = 0;
    bool sawFirstPixel = false;
    bool variedPixels = false;

    for( size_t y = 0; y < EXPECTED_HEIGHT; ++y )
    {
        const size_t rowOffset = y * FILTERED_ROW_BYTES;
        const unsigned char filter = aInflated[rowOffset];

        if( filter > 4 )
        {
            aError = "board render PNG uses an invalid scanline filter";
            return false;
        }

        for( size_t x = 0; x < ROW_BYTES; ++x )
        {
            const unsigned char encoded = aInflated[rowOffset + 1 + x];
            const unsigned char left = x >= BYTES_PER_PIXEL
                                               ? current[x - BYTES_PER_PIXEL]
                                               : 0;
            const unsigned char up = previous[x];
            const unsigned char upLeft = x >= BYTES_PER_PIXEL
                                                 ? previous[x - BYTES_PER_PIXEL]
                                                 : 0;
            unsigned char predictor = 0;

            if( filter == 1 )
                predictor = left;
            else if( filter == 2 )
                predictor = up;
            else if( filter == 3 )
                predictor = static_cast<unsigned char>(
                        ( static_cast<unsigned int>( left ) + up ) / 2 );
            else if( filter == 4 )
                predictor = paeth( left, up, upLeft );

            current[x] = static_cast<unsigned char>( encoded + predictor );
        }

        for( size_t x = 0; x < EXPECTED_WIDTH; ++x )
        {
            const size_t pixelOffset = x * BYTES_PER_PIXEL;
            std::array<unsigned char, BYTES_PER_PIXEL> pixel;
            std::copy_n( current.begin() + static_cast<std::ptrdiff_t>( pixelOffset ),
                         BYTES_PER_PIXEL, pixel.begin() );

            if( !sawFirstPixel )
            {
                firstPixel = pixel;
                sawFirstPixel = true;
            }
            else
            {
                variedPixels = variedPixels || pixel != firstPixel;
            }

            if( pixel[3] != 0 )
                ++visiblePixels;
        }

        previous.swap( current );
    }

    if( visiblePixels < 16 || !variedPixels )
    {
        aError = "board render PNG contains no visible varied board image";
        return false;
    }

    return true;
}

} // namespace


bool KICHAD::BOARD_RENDER_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_BOARD_RENDER_BYTES
        || aPath.filename() != aExpectedStem + "-board-render.png" )
    {
        aError = "board render PNG has an invalid size or filename";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::vector<unsigned char> source{ std::istreambuf_iterator<char>( input ),
                                       std::istreambuf_iterator<char>() };

    if( input.bad() || source.size() != bytes || source.size() < PNG_SIGNATURE.size()
        || !std::equal( PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), source.begin() ) )
    {
        aError = "board render artifact has the wrong PNG signature";
        return false;
    }

    size_t offset = PNG_SIGNATURE.size();
    size_t chunks = 0;
    size_t ihdrChunks = 0;
    size_t idatChunks = 0;
    size_t iendChunks = 0;
    bool idatClosed = false;
    std::vector<unsigned char> compressed;

    while( offset < source.size() )
    {
        if( source.size() - offset < 12 || ++chunks > MAX_BOARD_RENDER_CHUNKS )
        {
            aError = "board render PNG has a truncated or excessive chunk table";
            return false;
        }

        const uint32_t length = bigEndian32( source.data() + offset );

        if( length > source.size() - offset - 12 )
        {
            aError = "board render PNG has a truncated chunk";
            return false;
        }

        const unsigned char* typeBytes = source.data() + offset + 4;
        const std::string type( reinterpret_cast<const char*>( typeBytes ), 4 );

        if( !std::all_of( type.begin(), type.end(),
                          []( unsigned char aCharacter )
                          {
                              return std::isalpha( aCharacter );
                          } ) )
        {
            aError = "board render PNG has an invalid chunk type";
            return false;
        }

        const unsigned char* data = source.data() + offset + 8;
        const uint32_t expectedCrc = bigEndian32( data + length );
        uLong actualCrc = crc32( 0L, Z_NULL, 0 );
        actualCrc = crc32( actualCrc, typeBytes, 4 );
        actualCrc = crc32( actualCrc, data, static_cast<uInt>( length ) );

        if( static_cast<uint32_t>( actualCrc ) != expectedCrc )
        {
            aError = "board render PNG has a chunk CRC mismatch";
            return false;
        }

        if( type == "IHDR" )
        {
            if( chunks != 1 || ++ihdrChunks != 1 || length != 13
                || bigEndian32( data ) != EXPECTED_WIDTH
                || bigEndian32( data + 4 ) != EXPECTED_HEIGHT
                || data[8] != 8 || data[9] != 6 || data[10] != 0
                || data[11] != 0 || data[12] != 0 )
            {
                aError = "board render PNG has the wrong fixed RGBA image header";
                return false;
            }
        }
        else if( type == "IDAT" )
        {
            if( ihdrChunks != 1 || iendChunks != 0 || idatClosed
                || compressed.size() + length > MAX_BOARD_RENDER_BYTES )
            {
                aError = "board render PNG has an invalid image-data sequence";
                return false;
            }

            ++idatChunks;
            compressed.insert( compressed.end(), data, data + length );
        }
        else if( type == "IEND" )
        {
            if( length != 0 || idatChunks == 0 || ++iendChunks != 1
                || offset + 12 != source.size() )
            {
                aError = "board render PNG has an invalid end marker";
                return false;
            }

            idatClosed = true;
        }
        else
        {
            idatClosed = idatClosed || idatChunks != 0;

            if( ( typeBytes[0] & 0x20 ) == 0 )
            {
                aError = "board render PNG contains an unsupported critical chunk";
                return false;
            }
        }

        offset += static_cast<size_t>( length ) + 12;
    }

    if( ihdrChunks != 1 || idatChunks == 0 || iendChunks != 1 )
    {
        aError = "board render PNG is missing a required chunk";
        return false;
    }

    constexpr size_t ROW_BYTES = EXPECTED_WIDTH * BYTES_PER_PIXEL + 1;
    std::vector<unsigned char> inflated( ROW_BYTES * EXPECTED_HEIGHT );
    uLongf inflatedBytes = static_cast<uLongf>( inflated.size() );

    if( uncompress( inflated.data(), &inflatedBytes, compressed.data(),
                    static_cast<uLong>( compressed.size() ) ) != Z_OK
        || inflatedBytes != inflated.size() )
    {
        aError = "board render PNG image data could not be decompressed exactly";
        return false;
    }

    return validatePixels( inflated, aError );
}
