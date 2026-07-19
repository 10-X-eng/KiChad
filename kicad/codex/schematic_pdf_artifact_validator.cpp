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

#include "schematic_pdf_artifact_validator.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string_view>

#include <wx/string.h>


namespace
{

constexpr uintmax_t MAX_SCHEMATIC_PDF_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t MAX_SCHEMATIC_PAGES = 10'000;

constexpr std::string_view KICAD_JAVASCRIPT_NAMES = R"PDF(<< /JavaScript
 << /Names
    [ (JSInit) << /Type /Action /S /JavaScript /JS (
function ShM\(aEntries\) {
    var aParams = [];
    for \(var i = 0; i < aEntries.length; ++i\) {
        aParams.push\({
            cName: aEntries[i][0],
            cReturn: aEntries[i].length > 1 ? aEntries[i][1] : ''
        }\)
    }

    var cChoice = app.popUpMenuEx.apply\(app, aParams\);
    if \(cChoice == null || cChoice == ''\) return;

    if \(cChoice.substring\(0, 1\) == '#'\) {
        this.pageNum = parseInt\(cChoice.slice\(1\)\);
        return;
    }

    // Fallback: some viewers return cName instead of cReturn
    var url = cChoice;
    if \(url.substring\(0, 4\) != 'http' && url.substring\(0, 4\) != 'file'\) {
        var idx = url.indexOf\('http'\);
        if \(idx < 0\) idx = url.indexOf\('file:'\);
        if \(idx >= 0\) url = url.substring\(idx\);
        else return;
    }

    if \(url.substring\(0, 8\) == 'file:///'\) app.openDoc\(url.substring\(7\)\);
    else if \(url.substring\(0, 7\) == 'file://'\) app.openDoc\('//' + url.substring\(7\)\);
    else app.launchURL\(url\);
}
) >> ]
 >>
>>
endobj
)PDF";


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


bool parseUnsigned( std::string_view aText, uint64_t& aValue )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const auto [parsedEnd, error] = std::from_chars( begin, end, aValue );
    return error == std::errc() && parsedEnd != begin;
}


std::string encodePdfString( std::string_view aText )
{
    const wxString decoded = wxString::FromUTF8( aText.data(), aText.size() );
    bool ascii = true;

    for( wxUniChar character : decoded )
    {
        if( character.GetValue() >= 0x7F )
        {
            ascii = false;
            break;
        }
    }

    if( ascii )
    {
        std::string escaped = "(";
        escaped.reserve( aText.size() + 2 );

        for( char character : aText )
        {
            if( character == '\\' || character == '(' || character == ')' )
                escaped.push_back( '\\' );

            escaped.push_back( character );
        }

        escaped.push_back( ')' );
        return escaped;
    }

    std::ostringstream encoded;
    encoded << "<FEFF" << std::uppercase << std::hex << std::setfill( '0' );

    for( wxUniChar character : decoded )
    {
        encoded << std::setw( 4 )
                << static_cast<uint32_t>( character.GetValue() );
    }

    encoded << '>';
    return encoded.str();
}

} // namespace


bool KICHAD::SCHEMATIC_PDF_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const std::string& aExpectedTitle,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes < 256 || bytes > MAX_SCHEMATIC_PDF_BYTES )
    {
        aError = "schematic PDF artifact has an invalid size";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string contents{ std::istreambuf_iterator<char>( input ),
                          std::istreambuf_iterator<char>() };
    const std::string expectedTitle =
            "/Title " + encodePdfString( aExpectedTitle ) + "\n";

    if( input.bad() || contents.size() != bytes
        || !contents.starts_with( "%PDF-1.5\n" )
        || countOccurrences( contents, "/Producer (KiCad PDF)\n" ) != 1
        || countOccurrences( contents, "/Creator (Eeschema-PDF)\n" ) != 1
        || countOccurrences( contents, expectedTitle ) != 1
        || countOccurrences( contents, "/Type /Pages\n" ) != 1
        || countOccurrences( contents, "/Type /Catalog\n" ) != 1 )
    {
        aError = "schematic PDF artifact has the wrong native document identity";
        return false;
    }

    const size_t pageTree = contents.find( "/Type /Pages\n" );
    const size_t pageTreeEnd = contents.find( "endobj\n", pageTree );
    const size_t countField = contents.find( "/Count ", pageTree );
    uint64_t pageCount = 0;

    if( pageTreeEnd == std::string::npos || countField == std::string::npos
        || countField > pageTreeEnd
        || !parseUnsigned( std::string_view( contents ).substr( countField + 7 ),
                           pageCount )
        || pageCount == 0 || pageCount > MAX_SCHEMATIC_PAGES
        || countOccurrences( contents, "/Type /Page\n" ) != pageCount )
    {
        aError = "schematic PDF artifact has an invalid page tree";
        return false;
    }

    if( countOccurrences( contents, "/JavaScript" ) != 2
        || countOccurrences( contents, "/JS (" ) != 1
        || countOccurrences( contents, KICAD_JAVASCRIPT_NAMES ) != 1 )
    {
        aError = "schematic PDF artifact has an altered KiCad names object";
        return false;
    }

    for( std::string_view forbidden : { "/Subtype /Link", "/URI ", "/Launch",
                                        "/OpenAction", "/EmbeddedFile", "/RichMedia",
                                        "/GoToR", "/GoToE", "/Filespec" } )
    {
        if( contents.find( forbidden ) != std::string::npos )
        {
            aError = "schematic PDF artifact contains an external or active action";
            return false;
        }
    }

    const size_t startXref = contents.rfind( "startxref\n" );
    const size_t endOfFile = contents.rfind( "%%EOF" );
    uint64_t xrefOffset = 0;

    if( startXref == std::string::npos || endOfFile == std::string::npos
        || endOfFile < startXref
        || !parseUnsigned( std::string_view( contents ).substr( startXref + 10 ),
                           xrefOffset )
        || xrefOffset >= contents.size()
        || !std::string_view( contents ).substr( xrefOffset ).starts_with( "xref\n" )
        || contents.find( "trailer\n", xrefOffset ) == std::string::npos
        || !std::all_of( contents.begin() + endOfFile + 5, contents.end(),
                         []( char aCharacter )
                         {
                             return std::isspace(
                                     static_cast<unsigned char>( aCharacter ) );
                         } ) )
    {
        aError = "schematic PDF artifact has an invalid cross-reference trailer";
        return false;
    }

    return true;
}
