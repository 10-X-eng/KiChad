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

#include "u3d_artifact_validator.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>


namespace
{

constexpr uintmax_t MAX_U3D_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_U3D_BLOCKS = 1'000'000;
constexpr uint32_t FILE_HEADER = 0x00443355;
constexpr uint32_t MODIFIER_CHAIN = 0xFFFFFF14;
constexpr uint32_t MESH_CONTINUATION = 0xFFFFFF3B;
constexpr uint32_t LIT_TEXTURE_SHADER = 0xFFFFFF53;
constexpr uint32_t MATERIAL_RESOURCE = 0xFFFFFF54;
constexpr double KICAD_METRE_UNITS = static_cast<double>( 0.001F );


uint16_t littleEndian16( std::string_view aBytes, size_t aOffset )
{
    const auto* bytes = reinterpret_cast<const unsigned char*>( aBytes.data() + aOffset );
    return static_cast<uint16_t>( bytes[0] )
           | static_cast<uint16_t>( bytes[1] ) << 8;
}


uint32_t littleEndian32( std::string_view aBytes, size_t aOffset )
{
    const auto* bytes = reinterpret_cast<const unsigned char*>( aBytes.data() + aOffset );
    return static_cast<uint32_t>( bytes[0] )
           | static_cast<uint32_t>( bytes[1] ) << 8
           | static_cast<uint32_t>( bytes[2] ) << 16
           | static_cast<uint32_t>( bytes[3] ) << 24;
}


uint64_t littleEndian64( std::string_view aBytes, size_t aOffset )
{
    return static_cast<uint64_t>( littleEndian32( aBytes, aOffset ) )
           | static_cast<uint64_t>( littleEndian32( aBytes, aOffset + 4 ) ) << 32;
}


bool paddedSize( uint32_t aBytes, size_t& aPadded )
{
    const uint64_t padded = ( static_cast<uint64_t>( aBytes ) + 3 ) & ~uint64_t( 3 );

    if( padded > MAX_U3D_BYTES )
        return false;

    aPadded = static_cast<size_t>( padded );
    return true;
}

} // namespace


bool KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateContents(
        std::string_view aContents, std::string& aError )
{
    if( aContents.size() < 44 || aContents.size() > MAX_U3D_BYTES
        || littleEndian32( aContents, 0 ) != FILE_HEADER
        || littleEndian32( aContents, 4 ) != 32
        || littleEndian32( aContents, 8 ) != 0
        || littleEndian16( aContents, 12 ) != 256
        || littleEndian16( aContents, 14 ) != 0
        || littleEndian32( aContents, 16 ) != 0x00000008
        || littleEndian32( aContents, 32 ) != 106 )
    {
        aError = "U3D artifact has the wrong ECMA-363 file header";
        return false;
    }

    const uint64_t declarationEnd64 =
            static_cast<uint64_t>( littleEndian32( aContents, 20 ) ) + 8;
    const uint64_t declaredFileBytes = littleEndian64( aContents, 24 ) + 8;
    const double units = std::bit_cast<double>( littleEndian64( aContents, 36 ) );

    if( declaredFileBytes != aContents.size() || declarationEnd64 < 44
        || declarationEnd64 > aContents.size() || !std::isfinite( units )
        || units != KICAD_METRE_UNITS )
    {
        aError = "U3D artifact has invalid declared bounds or millimetre units";
        return false;
    }

    const size_t declarationEnd = static_cast<size_t>( declarationEnd64 );
    size_t offset = 0;
    size_t blocks = 0;
    size_t modifierChains = 0;
    size_t shaders = 0;
    size_t materials = 0;
    size_t continuations = 0;
    bool reachedContinuation = false;

    while( offset < aContents.size() )
    {
        if( aContents.size() - offset < 12 || ++blocks > MAX_U3D_BLOCKS )
        {
            aError = "U3D artifact has a truncated or excessive block sequence";
            return false;
        }

        const uint32_t type = littleEndian32( aContents, offset );
        const uint32_t dataBytes = littleEndian32( aContents, offset + 4 );
        const uint32_t metadataBytes = littleEndian32( aContents, offset + 8 );
        size_t paddedData = 0;
        size_t paddedMetadata = 0;

        if( !paddedSize( dataBytes, paddedData )
            || !paddedSize( metadataBytes, paddedMetadata )
            || metadataBytes != 0 || paddedData > aContents.size() - offset - 12
            || paddedMetadata > aContents.size() - offset - 12 - paddedData )
        {
            aError = "U3D artifact has invalid block bounds or metadata";
            return false;
        }

        const size_t next = offset + 12 + paddedData + paddedMetadata;

        if( blocks == 1 )
        {
            if( type != FILE_HEADER || next != 44 )
            {
                aError = "U3D artifact does not begin with one exact header block";
                return false;
            }
        }
        else if( offset < declarationEnd )
        {
            if( reachedContinuation || next > declarationEnd )
            {
                aError = "U3D declaration blocks cross their declared boundary";
                return false;
            }

            if( type == MODIFIER_CHAIN )
                ++modifierChains;
            else if( type == LIT_TEXTURE_SHADER )
                ++shaders;
            else if( type == MATERIAL_RESOURCE )
                ++materials;
            else
            {
                aError = "U3D declaration contains an unexpected top-level block";
                return false;
            }
        }
        else
        {
            reachedContinuation = true;

            if( offset == declarationEnd && type != MESH_CONTINUATION )
            {
                aError = "U3D continuation section has the wrong first block";
                return false;
            }

            if( type != MESH_CONTINUATION )
            {
                aError = "U3D continuation contains an unexpected top-level block";
                return false;
            }

            ++continuations;
        }

        for( size_t padding = dataBytes; padding < paddedData; ++padding )
        {
            if( aContents[offset + 12 + padding] != '\0' )
            {
                aError = "U3D block has nonzero alignment padding";
                return false;
            }
        }

        offset = next;
    }

    if( offset != aContents.size() || !reachedContinuation
        || modifierChains == 0 || shaders == 0 || materials == 0
        || continuations == 0 || shaders != materials
        || materials != continuations
        || aContents.substr( 44, declarationEnd - 44 ).find( "groups" )
                   == std::string_view::npos )
    {
        aError = "U3D artifact has an incomplete KiCad scene structure";
        return false;
    }

    return true;
}


bool KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_U3D_BYTES )
    {
        aError = "U3D artifact has an invalid size";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string contents{ std::istreambuf_iterator<char>( input ),
                          std::istreambuf_iterator<char>() };

    if( input.bad() || contents.size() != bytes )
    {
        aError = "U3D artifact could not be read completely";
        return false;
    }

    return ValidateContents( contents, aError );
}
