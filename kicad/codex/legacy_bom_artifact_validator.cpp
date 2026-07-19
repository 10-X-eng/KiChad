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

#include "legacy_bom_artifact_validator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/string.h>
#include <wx/xml/xml.h>


namespace
{

using JSON = nlohmann::json;

constexpr uintmax_t MAX_LEGACY_BOM_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_LEGACY_BOM_NODES = 2'000'000;
constexpr size_t MAX_LEGACY_BOM_DEPTH = 256;
constexpr size_t MAX_LEGACY_BOM_COMPONENTS = 100'000;

struct COMPONENT
{
    std::string value;
    std::string footprint;

    auto operator<=>( const COMPONENT& ) const = default;
};


wxXmlNode* directChild( wxXmlNode* aParent, const wxString& aName )
{
    wxXmlNode* match = nullptr;

    for( wxXmlNode* child = aParent ? aParent->GetChildren() : nullptr;
         child; child = child->GetNext() )
    {
        if( child->GetType() != wxXML_ELEMENT_NODE || child->GetName() != aName )
            continue;

        if( match )
            return nullptr;

        match = child;
    }

    return match;
}


bool expectedComponents( const JSON& aPlan,
                         std::map<std::string, COMPONENT>& aExpected,
                         std::string& aError )
{
    if( !aPlan.is_object() || !aPlan.contains( "expectedSchematicBomRows" )
        || !aPlan["expectedSchematicBomRows"].is_array()
        || aPlan["expectedSchematicBomRows"].size() > MAX_LEGACY_BOM_COMPONENTS )
    {
        aError = "fabrication plan has an invalid legacy BOM contract";
        return false;
    }

    for( const JSON& item : aPlan["expectedSchematicBomRows"] )
    {
        if( !item.is_object() || !item.contains( "reference" )
            || !item["reference"].is_string() || !item.contains( "value" )
            || !item["value"].is_string() || !item.contains( "footprint" )
            || !item["footprint"].is_string() )
        {
            aError = "fabrication plan has a malformed legacy BOM component";
            return false;
        }

        const std::string reference = item["reference"].get<std::string>();
        COMPONENT component = { item["value"].get<std::string>(),
                                item["footprint"].get<std::string>() };

        if( reference.empty()
            || !aExpected.emplace( reference, std::move( component ) ).second )
        {
            aError = "fabrication plan has a duplicate legacy BOM reference";
            return false;
        }
    }

    return true;
}

} // namespace


bool KICHAD::LEGACY_BOM_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const nlohmann::json& aPlan,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_LEGACY_BOM_BYTES )
    {
        aError = "legacy BOM artifact has an invalid size";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string source{ std::istreambuf_iterator<char>( input ),
                        std::istreambuf_iterator<char>() };
    std::string lowered = source;
    std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );

    if( input.bad() || source.size() != bytes
        || source.find( '\0' ) != std::string::npos
        || lowered.find( "<!doctype" ) != std::string::npos
        || lowered.find( "<!entity" ) != std::string::npos )
    {
        aError = "legacy BOM artifact contains unsafe XML content";
        return false;
    }

    wxXmlDocument document;

    if( !document.Load( wxString::FromUTF8( aPath.string().c_str() ) ) )
    {
        aError = "legacy BOM artifact is not well-formed XML";
        return false;
    }

    wxXmlNode* root = document.GetRoot();

    if( !root || root->GetName() != wxS( "export" )
        || root->GetAttribute( wxS( "version" ) ) != wxS( "E" ) )
    {
        aError = "legacy BOM artifact has the wrong native root schema";
        return false;
    }

    static const std::set<wxString> REQUIRED_COLLECTIONS = {
        wxS( "design" ), wxS( "components" ), wxS( "libparts" ),
        wxS( "libraries" ), wxS( "nets" )
    };
    std::map<wxString, wxXmlNode*> collections;

    for( wxXmlNode* child = root->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() != wxXML_ELEMENT_NODE )
            continue;

        if( !REQUIRED_COLLECTIONS.contains( child->GetName() )
            || !collections.emplace( child->GetName(), child ).second )
        {
            aError = "legacy BOM artifact has an unexpected or duplicate root collection";
            return false;
        }
    }

    if( collections.size() != REQUIRED_COLLECTIONS.size() )
    {
        aError = "legacy BOM artifact is missing a required native collection";
        return false;
    }

    std::vector<std::pair<wxXmlNode*, size_t>> pending = { { root, 0 } };
    size_t nodes = 0;

    while( !pending.empty() )
    {
        const auto [node, depth] = pending.back();
        pending.pop_back();

        if( depth > MAX_LEGACY_BOM_DEPTH || ++nodes > MAX_LEGACY_BOM_NODES )
        {
            aError = "legacy BOM artifact exceeds structural limits";
            return false;
        }

        for( wxXmlNode* child = node->GetChildren(); child; child = child->GetNext() )
            pending.emplace_back( child, depth + 1 );
    }

    wxXmlNode* design = collections.at( wxS( "design" ) );
    wxXmlNode* sourceNode = directChild( design, wxS( "source" ) );
    wxXmlNode* toolNode = directChild( design, wxS( "tool" ) );
    const std::string fileStem = aPlan.value( "fileStem", "" );
    bool rootSheet = false;

    for( wxXmlNode* child = design->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() == wxXML_ELEMENT_NODE && child->GetName() == wxS( "sheet" )
            && child->GetAttribute( wxS( "name" ) ) == wxS( "/" ) )
        {
            rootSheet = true;
        }
    }

    if( fileStem.empty() || !sourceNode || !toolNode || !rootSheet
        || std::filesystem::path( sourceNode->GetNodeContent().ToStdString() )
                   .filename().string() != fileStem + ".kicad_sch"
        || !toolNode->GetNodeContent().StartsWith( wxS( "Eeschema 10.0.4" ) ) )
    {
        aError = "legacy BOM artifact has the wrong schematic or native producer identity";
        return false;
    }

    std::map<std::string, COMPONENT> expected;

    if( !expectedComponents( aPlan, expected, aError ) )
        return false;

    std::map<std::string, COMPONENT> actual;
    wxXmlNode* components = collections.at( wxS( "components" ) );

    for( wxXmlNode* child = components->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() != wxXML_ELEMENT_NODE )
            continue;

        if( child->GetName() != wxS( "comp" )
            || actual.size() >= MAX_LEGACY_BOM_COMPONENTS )
        {
            aError = "legacy BOM component collection has an invalid entry";
            return false;
        }

        const std::string reference = child->GetAttribute( wxS( "ref" ) ).ToStdString();
        wxXmlNode* valueNode = directChild( child, wxS( "value" ) );
        wxXmlNode* footprintNode = directChild( child, wxS( "footprint" ) );
        COMPONENT component = {
            valueNode ? valueNode->GetNodeContent().ToStdString() : std::string(),
            footprintNode ? footprintNode->GetNodeContent().ToStdString() : std::string()
        };

        if( reference.empty() || !valueNode
            || !actual.emplace( reference, std::move( component ) ).second )
        {
            aError = "legacy BOM artifact has an unvalued or duplicate component";
            return false;
        }
    }

    if( actual != expected )
    {
        aError = "legacy BOM components differ from the exact compiled KDS components";
        return false;
    }

    return true;
}
