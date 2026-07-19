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

#include "three_d_pdf_artifact_validator.h"

#include "u3d_artifact_validator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>

#include <zlib.h>


namespace
{

constexpr uintmax_t MAX_3D_PDF_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr uintmax_t MAX_EMBEDDED_U3D_BYTES = 256ULL * 1024ULL * 1024ULL;


size_t countOccurrences( std::string_view aText, std::string_view aNeedle )
{
    size_t count = 0;
    size_t position = 0;

    while( ( position = aText.find( aNeedle, position ) ) != std::string_view::npos )
    {
        ++count;
        position += aNeedle.size();
    }

    return count;
}


bool parseUnsigned( std::string_view aText, uint64_t& aValue, size_t& aConsumed )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const auto [parsedEnd, error] = std::from_chars( begin, end, aValue );

    if( error != std::errc() || parsedEnd == begin )
        return false;

    aConsumed = static_cast<size_t>( parsedEnd - begin );
    return true;
}


bool inflateU3d( std::string_view aCompressed, std::string& aExpanded )
{
    if( aCompressed.empty() || aCompressed.size() > MAX_3D_PDF_BYTES )
        return false;

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>( const_cast<char*>( aCompressed.data() ) );
    stream.avail_in = static_cast<uInt>( aCompressed.size() );

    if( inflateInit( &stream ) != Z_OK )
        return false;

    std::array<char, 64 * 1024> output{};
    int status = Z_OK;

    do
    {
        stream.next_out = reinterpret_cast<Bytef*>( output.data() );
        stream.avail_out = static_cast<uInt>( output.size() );
        status = inflate( &stream, Z_NO_FLUSH );

        if( status != Z_OK && status != Z_STREAM_END )
            break;

        const size_t produced = output.size() - stream.avail_out;

        if( aExpanded.size() > MAX_EMBEDDED_U3D_BYTES - produced )
        {
            status = Z_MEM_ERROR;
            break;
        }

        aExpanded.append( output.data(), produced );
    } while( status != Z_STREAM_END );

    const bool consumedExactly = status == Z_STREAM_END && stream.avail_in == 0;
    inflateEnd( &stream );
    return consumedExactly && !aExpanded.empty();
}

} // namespace


bool KICHAD::THREE_D_PDF_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_3D_PDF_BYTES )
    {
        aError = "3D PDF artifact has an invalid size";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string contents{ std::istreambuf_iterator<char>( input ),
                          std::istreambuf_iterator<char>() };

    if( input.bad() || contents.size() != bytes || !contents.starts_with( "%PDF-1.5\n" )
        || countOccurrences( contents, "/Type /3D\n" ) != 1
        || countOccurrences( contents, "/Subtype /U3D\n" ) != 1
        || countOccurrences( contents, "/Type /3DView\n" ) < 4
        || contents.find( "/Producer (KiCad PDF)" ) == std::string::npos
        || contents.find( "/Type /Annot" ) == std::string::npos
        || contents.find( "/Subtype /3D" ) == std::string::npos )
    {
        aError = "3D PDF artifact has the wrong native interactive-document structure";
        return false;
    }

    const size_t modelObject = contents.find( "/Type /3D\n/Subtype /U3D\n" );
    const size_t lengthField = contents.find( "/Length ", modelObject );
    const size_t streamMarker = contents.find( ">>\nstream\n", modelObject );
    uint64_t lengthObject = 0;
    size_t consumed = 0;

    if( modelObject == std::string::npos || lengthField == std::string::npos
        || streamMarker == std::string::npos || lengthField > streamMarker
        || !parseUnsigned( std::string_view( contents ).substr( lengthField + 8 ),
                           lengthObject, consumed )
        || std::string_view( contents ).substr( lengthField + 8 + consumed,
                                                5 ) != " 0 R\n" )
    {
        aError = "3D PDF artifact has an invalid indirect model length";
        return false;
    }

    const std::string_view modelDictionary =
            std::string_view( contents ).substr( modelObject,
                                                 streamMarker - modelObject );

    if( countOccurrences( modelDictionary, "/Filter /FlateDecode\n" ) != 1
        || modelDictionary.find( "/DV " ) == std::string_view::npos
        || modelDictionary.find( "/VA [" ) == std::string_view::npos )
    {
        aError = "3D PDF artifact has an invalid native model dictionary";
        return false;
    }

    const std::string lengthObjectHeader =
            "\n" + std::to_string( lengthObject ) + " 0 obj\n";
    const size_t lengthObjectPosition = contents.find( lengthObjectHeader, streamMarker );
    uint64_t modelBytes = 0;

    if( lengthObjectPosition == std::string::npos
        || !parseUnsigned( std::string_view( contents ).substr(
                                   lengthObjectPosition + lengthObjectHeader.size() ),
                           modelBytes, consumed )
        || std::string_view( contents ).substr(
                   lengthObjectPosition + lengthObjectHeader.size() + consumed,
                   8 ) != "\nendobj\n" )
    {
        aError = "3D PDF artifact does not resolve its model stream length";
        return false;
    }

    const size_t modelStart = streamMarker + std::string_view( ">>\nstream\n" ).size();
    static constexpr std::string_view END_STREAM = "\nendstream";

    if( modelBytes == 0 || modelBytes > contents.size() - modelStart
        || std::string_view( contents ).substr( modelStart + modelBytes,
                                                END_STREAM.size() ) != END_STREAM )
    {
        aError = "3D PDF artifact model stream crosses its declared bounds";
        return false;
    }

    std::string outsideModel = contents.substr( 0, modelStart );
    outsideModel.append( contents.substr( modelStart + static_cast<size_t>( modelBytes ) ) );

    for( std::string_view forbidden : { "/JavaScript", "/JS ", "/Launch",
                                        "/OpenAction", "/EmbeddedFile", "/RichMedia",
                                        "/URI ", "/XFA" } )
    {
        if( outsideModel.find( forbidden ) != std::string::npos )
        {
            aError = "3D PDF artifact contains active or external content";
            return false;
        }
    }

    const size_t startXref = outsideModel.rfind( "startxref\n" );
    const size_t endOfFile = contents.rfind( "%%EOF" );
    uint64_t xrefOffset = 0;

    if( startXref == std::string::npos
        || endOfFile == std::string::npos
        || countOccurrences( outsideModel, "%%EOF" ) != 1
        || !std::all_of( contents.begin() + endOfFile + 5, contents.end(),
                         []( char aCharacter )
                         {
                             return std::isspace(
                                     static_cast<unsigned char>( aCharacter ) );
                         } )
        || !parseUnsigned( std::string_view( outsideModel ).substr( startXref + 10 ),
                           xrefOffset, consumed )
        || xrefOffset >= contents.size()
        || !std::string_view( contents ).substr( xrefOffset ).starts_with( "xref\n" )
        || endOfFile < xrefOffset )
    {
        aError = "3D PDF artifact has an invalid cross-reference trailer";
        return false;
    }

    std::string expanded;

    if( !inflateU3d( std::string_view( contents ).substr(
                              modelStart, static_cast<size_t>( modelBytes ) ),
                     expanded )
        || !KICHAD::U3D_ARTIFACT_VALIDATOR::ValidateContents( expanded, aError ) )
    {
        if( aError.empty() )
            aError = "3D PDF artifact contains an invalid compressed U3D model";

        return false;
    }

    return true;
}
