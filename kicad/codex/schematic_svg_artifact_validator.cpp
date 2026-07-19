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

#include "schematic_svg_artifact_validator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <wx/string.h>
#include <wx/xml/xml.h>


namespace
{

constexpr uintmax_t MAX_SCHEMATIC_SVG_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_SCHEMATIC_SVG_NODES = 2'000'000;
constexpr size_t MAX_SCHEMATIC_SVG_DEPTH = 256;


bool validFilename( const std::filesystem::path& aPath,
                    const std::string& aExpectedStem )
{
    const std::string filename = aPath.filename().string();
    return filename == aExpectedStem + ".svg"
           || ( filename.starts_with( aExpectedStem + "-" )
                && filename.ends_with( ".svg" ) );
}

} // namespace


bool KICHAD::SCHEMATIC_SVG_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_SCHEMATIC_SVG_BYTES
        || !validFilename( aPath, aExpectedStem ) )
    {
        aError = "schematic SVG artifact has an invalid size or filename";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string text{ std::istreambuf_iterator<char>( input ),
                      std::istreambuf_iterator<char>() };
    std::string lowered = text;
    std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );

    if( input.bad() || text.size() != bytes || text.find( '\0' ) != std::string::npos
        || lowered.find( "<!entity" ) != std::string::npos
        || lowered.find( "javascript:" ) != std::string::npos
        || lowered.find( "url(http" ) != std::string::npos
        || lowered.find( "url(file" ) != std::string::npos
        || lowered.find( "@import" ) != std::string::npos )
    {
        aError = "schematic SVG artifact could not be read safely";
        return false;
    }

    wxXmlDocument document;

    if( !document.Load( wxString::FromUTF8( aPath.string().c_str() ) ) )
    {
        aError = "schematic SVG artifact is not well-formed XML";
        return false;
    }

    wxXmlNode* root = document.GetRoot();

    if( !root || root->GetName() != wxS( "svg" )
        || root->GetAttribute( wxS( "version" ) ) != wxS( "1.1" )
        || root->GetAttribute( wxS( "xmlns" ) ) != wxS( "http://www.w3.org/2000/svg" )
        || root->GetAttribute( wxS( "width" ) ).empty()
        || root->GetAttribute( wxS( "height" ) ).empty()
        || root->GetAttribute( wxS( "viewBox" ) ).empty() )
    {
        aError = "schematic SVG artifact has the wrong native root schema";
        return false;
    }

    std::vector<std::pair<wxXmlNode*, size_t>> pending = { { root, 0 } };
    size_t nodes = 0;
    size_t graphics = 0;
    bool nativeDescription = false;
    bool matchingTitle = false;
    const wxString expectedFilename = wxString::FromUTF8(
            aPath.filename().string().c_str() );

    while( !pending.empty() )
    {
        const auto [node, depth] = pending.back();
        pending.pop_back();

        if( depth > MAX_SCHEMATIC_SVG_DEPTH || ++nodes > MAX_SCHEMATIC_SVG_NODES )
        {
            aError = "schematic SVG artifact exceeds structural limits";
            return false;
        }

        if( node->GetType() == wxXML_ELEMENT_NODE )
        {
            const wxString name = node->GetName().Lower();

            if( name == wxS( "script" ) || name == wxS( "foreignobject" )
                || name == wxS( "iframe" ) )
            {
                aError = "schematic SVG artifact contains active content";
                return false;
            }

            if( name == wxS( "path" ) || name == wxS( "line" )
                || name == wxS( "polyline" ) || name == wxS( "polygon" )
                || name == wxS( "circle" ) || name == wxS( "rect" )
                || name == wxS( "text" ) )
            {
                ++graphics;
            }

            if( name == wxS( "desc" )
                && node->GetNodeContent().Contains( wxS( "Eeschema-SVG" ) ) )
            {
                nativeDescription = true;
            }

            if( name == wxS( "title" )
                && node->GetNodeContent().Contains( expectedFilename ) )
            {
                matchingTitle = true;
            }

            for( wxXmlAttribute* attribute = node->GetAttributes(); attribute;
                 attribute = attribute->GetNext() )
            {
                const wxString attributeName = attribute->GetName().Lower();
                const wxString value = attribute->GetValue().Lower();

                if( attributeName.StartsWith( wxS( "on" ) )
                    || ( ( attributeName == wxS( "href" )
                           || attributeName == wxS( "xlink:href" ) )
                         && !value.StartsWith( wxS( "#" ) )
                         && !value.StartsWith( wxS( "data:image/" ) ) ) )
                {
                    aError = "schematic SVG artifact contains an unsafe reference";
                    return false;
                }
            }
        }

        for( wxXmlNode* child = node->GetChildren(); child; child = child->GetNext() )
            pending.emplace_back( child, depth + 1 );
    }

    if( graphics == 0 || !nativeDescription || !matchingTitle )
    {
        aError = "schematic SVG artifact contains no identified native drawing";
        return false;
    }

    return true;
}
