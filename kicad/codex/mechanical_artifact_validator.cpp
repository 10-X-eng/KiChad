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

#include "mechanical_artifact_validator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/string.h>
#include <wx/xml/xml.h>


namespace
{

using JSON = nlohmann::json;

constexpr uintmax_t MAX_MECHANICAL_ARTIFACT_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_STL_LINES = 8'000'000;
constexpr size_t MAX_STL_LINE_BYTES = 1024 * 1024;
constexpr size_t MAX_STL_FACETS = 1'000'000;
constexpr size_t MAX_XML_NODES = 2'000'000;
constexpr size_t MAX_XML_DEPTH = 256;


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


bool readBounded( const std::filesystem::path& aPath, std::string& aContents,
                  std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_MECHANICAL_ARTIFACT_BYTES )
    {
        aError = "mechanical artifact has an invalid size";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    aContents.assign( std::istreambuf_iterator<char>( input ),
                      std::istreambuf_iterator<char>() );

    if( input.bad() || aContents.size() != bytes )
    {
        aError = "mechanical artifact could not be read completely";
        return false;
    }

    return true;
}


bool validateBrepContents( const std::string& aContents, std::string& aError )
{
    if( aContents.find( '\0' ) != std::string::npos
        || !trim( aContents ).starts_with(
                "CASCADE Topology V1, (c) Matra-Datavision\n" ) )
    {
        aError = "BREP artifact has the wrong Open CASCADE header";
        return false;
    }

    static constexpr std::array<std::string_view, 8> SECTIONS = {
        "Locations ", "Curve2ds ", "Curves ", "Polygon3D ",
        "PolygonOnTriangulations ", "Surfaces ", "Triangulations ", "TShapes "
    };
    size_t position = 0;
    size_t shapeCount = 0;

    for( std::string_view section : SECTIONS )
    {
        const size_t found = aContents.find( "\n" + std::string( section ), position );

        if( found == std::string::npos )
        {
            aError = "BREP artifact is missing an ordered topology section";
            return false;
        }

        const size_t valueBegin = found + 1 + section.size();
        const size_t valueEnd = aContents.find( '\n', valueBegin );
        unsigned long long count = 0;

        if( valueEnd == std::string::npos )
        {
            aError = "BREP artifact has an unterminated topology section";
            return false;
        }

        std::istringstream parser( aContents.substr( valueBegin, valueEnd - valueBegin ) );
        std::string trailing;

        if( !( parser >> count ) || ( parser >> trailing ) || count > MAX_XML_NODES )
        {
            aError = "BREP artifact has an invalid topology section count";
            return false;
        }

        if( section == "TShapes " )
            shapeCount = static_cast<size_t>( count );

        position = valueEnd;
    }

    if( shapeCount == 0 || !trim( aContents ).ends_with( "+1 0" ) )
    {
        aError = "BREP artifact contains no complete top-level shape";
        return false;
    }

    return true;
}


bool parseVector( std::string_view aLine, std::string_view aPrefix )
{
    aLine = trim( aLine );

    if( !aLine.starts_with( aPrefix ) )
        return false;

    std::istringstream parser{ std::string( aLine.substr( aPrefix.size() ) ) };
    std::array<double, 3> coordinates;
    std::string trailing;

    if( !( parser >> coordinates[0] >> coordinates[1] >> coordinates[2] )
        || ( parser >> trailing ) )
    {
        return false;
    }

    return std::all_of( coordinates.begin(), coordinates.end(),
                        []( double aValue ) { return std::isfinite( aValue ); } );
}


uint32_t littleEndian32( const std::string& aBytes, size_t aOffset )
{
    const auto* bytes = reinterpret_cast<const unsigned char*>( aBytes.data() + aOffset );
    return static_cast<uint32_t>( bytes[0] )
           | static_cast<uint32_t>( bytes[1] ) << 8
           | static_cast<uint32_t>( bytes[2] ) << 16
           | static_cast<uint32_t>( bytes[3] ) << 24;
}


bool parseCount( const wxString& aValue, size_t& aCount )
{
    unsigned long long value = 0;

    if( aValue.empty() || !aValue.ToULongLong( &value )
        || value > static_cast<unsigned long long>( MAX_XML_NODES ) )
    {
        return false;
    }

    aCount = static_cast<size_t>( value );
    return true;
}


std::string jsonString( const JSON& aObject, const char* aKey )
{
    if( !aObject.is_object() || !aObject.contains( aKey )
        || !aObject[aKey].is_string() )
    {
        return {};
    }

    return aObject[aKey].get<std::string>();
}


bool jsonIndex( const JSON& aObject, const char* aKey, size_t aLimit,
                size_t& aIndex )
{
    if( !aObject.is_object() || !aObject.contains( aKey )
        || !aObject[aKey].is_number_unsigned() )
    {
        return false;
    }

    const uint64_t value = aObject[aKey].get<uint64_t>();

    if( value >= aLimit )
        return false;

    aIndex = static_cast<size_t>( value );
    return true;
}


bool validGlbDocument( const JSON& aDocument, const std::string& aExpectedStem,
                       size_t aBinaryBytes )
{
    static constexpr size_t MAX_GLTF_COLLECTION_ITEMS = 1'000'000;

    if( !aDocument.is_object() || !aDocument.contains( "asset" )
        || !aDocument["asset"].is_object() )
    {
        return false;
    }

    const JSON& asset = aDocument["asset"];

    if( jsonString( asset, "version" ) != "2.0" || !asset.contains( "extras" )
        || !asset["extras"].is_object()
        || !jsonString( asset["extras"], "generator" ).starts_with( "KiCad 10.0.4" )
        || jsonString( asset["extras"], "pcb_name" ) != aExpectedStem )
    {
        return false;
    }

    for( const char* collection : { "scenes", "nodes", "meshes", "bufferViews",
                                    "accessors" } )
    {
        if( !aDocument.contains( collection ) || !aDocument[collection].is_array()
            || aDocument[collection].empty()
            || aDocument[collection].size() > MAX_GLTF_COLLECTION_ITEMS )
        {
            return false;
        }
    }

    if( !aDocument.contains( "buffers" ) || !aDocument["buffers"].is_array()
        || aDocument["buffers"].size() != 1 || !aDocument["buffers"][0].is_object()
        || aDocument["buffers"][0].contains( "uri" )
        || !aDocument["buffers"][0].contains( "byteLength" )
        || !aDocument["buffers"][0]["byteLength"].is_number_unsigned()
        || aDocument["buffers"][0]["byteLength"].get<uint64_t>() != aBinaryBytes )
    {
        return false;
    }

    size_t defaultScene = 0;

    if( !jsonIndex( aDocument, "scene", aDocument["scenes"].size(), defaultScene ) )
        return false;

    const JSON& scene = aDocument["scenes"][defaultScene];

    if( !scene.is_object() || !scene.contains( "nodes" ) || !scene["nodes"].is_array()
        || scene["nodes"].empty() )
    {
        return false;
    }

    for( const JSON& nodeIndex : scene["nodes"] )
    {
        if( !nodeIndex.is_number_unsigned()
            || nodeIndex.get<uint64_t>() >= aDocument["nodes"].size() )
        {
            return false;
        }
    }

    for( const JSON& view : aDocument["bufferViews"] )
    {
        size_t bufferIndex = 0;

        if( !jsonIndex( view, "buffer", 1, bufferIndex )
            || !view.contains( "byteLength" ) || !view["byteLength"].is_number_unsigned() )
        {
            return false;
        }

        const uint64_t length = view["byteLength"].get<uint64_t>();
        uint64_t offset = 0;

        if( view.contains( "byteOffset" ) )
        {
            if( !view["byteOffset"].is_number_unsigned() )
                return false;

            offset = view["byteOffset"].get<uint64_t>();
        }

        if( length == 0 || offset > aBinaryBytes || length > aBinaryBytes - offset )
            return false;
    }

    static const std::set<std::string> ACCESSOR_TYPES = {
        "SCALAR", "VEC2", "VEC3", "VEC4", "MAT2", "MAT3", "MAT4"
    };
    static const std::set<uint64_t> COMPONENT_TYPES = {
        5120, 5121, 5122, 5123, 5125, 5126
    };

    for( const JSON& accessor : aDocument["accessors"] )
    {
        size_t viewIndex = 0;

        if( !jsonIndex( accessor, "bufferView", aDocument["bufferViews"].size(),
                        viewIndex )
            || !accessor.contains( "componentType" )
            || !accessor["componentType"].is_number_unsigned()
            || !COMPONENT_TYPES.contains(
                    accessor["componentType"].get<uint64_t>() )
            || !accessor.contains( "count" ) || !accessor["count"].is_number_unsigned()
            || accessor["count"].get<uint64_t>() == 0
            || !ACCESSOR_TYPES.contains( jsonString( accessor, "type" ) ) )
        {
            return false;
        }
    }

    for( const JSON& node : aDocument["nodes"] )
    {
        if( !node.is_object() )
            return false;

        if( node.contains( "mesh" ) )
        {
            size_t meshIndex = 0;

            if( !jsonIndex( node, "mesh", aDocument["meshes"].size(), meshIndex ) )
                return false;
        }
    }

    for( const JSON& mesh : aDocument["meshes"] )
    {
        if( !mesh.is_object() || !mesh.contains( "primitives" )
            || !mesh["primitives"].is_array() || mesh["primitives"].empty() )
        {
            return false;
        }

        for( const JSON& primitive : mesh["primitives"] )
        {
            size_t positionAccessor = 0;

            if( !primitive.is_object() || !primitive.contains( "attributes" )
                || !primitive["attributes"].is_object()
                || !jsonIndex( primitive["attributes"], "POSITION",
                               aDocument["accessors"].size(), positionAccessor ) )
            {
                return false;
            }
        }
    }

    if( aDocument.contains( "images" ) )
    {
        if( !aDocument["images"].is_array()
            || aDocument["images"].size() > MAX_GLTF_COLLECTION_ITEMS )
        {
            return false;
        }

        for( const JSON& image : aDocument["images"] )
        {
            if( !image.is_object() || image.contains( "uri" ) )
                return false;
        }
    }

    return true;
}

} // namespace


bool KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateBrep(
        const std::filesystem::path& aPath, std::string& aError )
{
    std::string contents;

    if( !readBounded( aPath, contents, aError ) )
        return false;

    return validateBrepContents( contents, aError );
}


bool KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateGlb(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    std::string contents;

    if( !readBounded( aPath, contents, aError ) )
        return false;

    if( contents.size() < 28 || littleEndian32( contents, 0 ) != 0x46546C67
        || littleEndian32( contents, 4 ) != 2
        || littleEndian32( contents, 8 ) != contents.size() )
    {
        aError = "GLB artifact has an invalid version-2 container header";
        return false;
    }

    size_t offset = 12;
    std::string jsonChunk;
    size_t binaryBytes = 0;
    size_t chunkCount = 0;

    while( offset < contents.size() )
    {
        if( contents.size() - offset < 8 )
        {
            aError = "GLB artifact has a truncated chunk header";
            return false;
        }

        const size_t chunkBytes = littleEndian32( contents, offset );
        const uint32_t chunkType = littleEndian32( contents, offset + 4 );
        offset += 8;

        if( chunkBytes % 4 != 0 || chunkBytes > contents.size() - offset
            || ++chunkCount > 2 )
        {
            aError = "GLB artifact has invalid chunk bounds";
            return false;
        }

        if( chunkCount == 1 && chunkType == 0x4E4F534A )
            jsonChunk.assign( contents.data() + offset, chunkBytes );
        else if( chunkCount == 2 && chunkType == 0x004E4942 )
            binaryBytes = chunkBytes;
        else
        {
            aError = "GLB artifact does not contain the exact JSON/BIN chunk sequence";
            return false;
        }

        offset += chunkBytes;
    }

    JSON document = JSON::parse( jsonChunk, nullptr, false );

    if( offset != contents.size() || chunkCount != 2 || jsonChunk.empty()
        || binaryBytes == 0 || document.is_discarded()
        || !validGlbDocument( document, aExpectedStem, binaryBytes ) )
    {
        aError = "GLB artifact has the wrong native scene structure";
        return false;
    }

    return true;
}


bool KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateStl(
        const std::filesystem::path& aPath, std::string& aError )
{
    std::string contents;

    if( !readBounded( aPath, contents, aError ) )
        return false;

    if( contents.find( '\0' ) != std::string::npos )
    {
        aError = "STL artifact is not the expected bounded ASCII representation";
        return false;
    }

    std::istringstream input( contents );
    std::string line;
    size_t lineCount = 0;
    size_t facets = 0;

    const auto nextLine = [&]()
    {
        if( !std::getline( input, line ) || ++lineCount > MAX_STL_LINES
            || line.size() > MAX_STL_LINE_BYTES )
        {
            return false;
        }

        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        return true;
    };

    if( !nextLine() || !trim( line ).starts_with( "solid" )
        || ( trim( line ).size() > 5
             && !std::isspace( static_cast<unsigned char>( trim( line )[5] ) ) ) )
    {
        aError = "STL artifact has no ASCII solid root";
        return false;
    }

    while( nextLine() )
    {
        if( trim( line ).starts_with( "endsolid" )
            && ( trim( line ).size() == 8
                 || std::isspace( static_cast<unsigned char>( trim( line )[8] ) ) ) )
        {
            while( std::getline( input, line ) )
            {
                if( ++lineCount > MAX_STL_LINES || line.size() > MAX_STL_LINE_BYTES
                    || !trim( line ).empty() )
                {
                    aError = "STL artifact contains trailing content";
                    return false;
                }
            }

            if( facets == 0 )
            {
                aError = "STL artifact contains no facets";
                return false;
            }

            return true;
        }

        if( !parseVector( line, "facet normal" ) || ++facets > MAX_STL_FACETS
            || !nextLine() || trim( line ) != "outer loop" )
        {
            aError = "STL artifact has an invalid facet header";
            return false;
        }

        for( size_t vertex = 0; vertex < 3; ++vertex )
        {
            if( !nextLine() || !parseVector( line, "vertex" ) )
            {
                aError = "STL artifact has an invalid triangular vertex";
                return false;
            }
        }

        if( !nextLine() || trim( line ) != "endloop"
            || !nextLine() || trim( line ) != "endfacet" )
        {
            aError = "STL artifact has an incomplete facet";
            return false;
        }
    }

    aError = "STL artifact has no closing endsolid record";
    return false;
}


bool KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateXao(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    std::string contents;

    if( !readBounded( aPath, contents, aError ) )
        return false;

    std::string lowered = contents;
    std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );

    if( contents.find( '\0' ) != std::string::npos
        || lowered.find( "<!doctype" ) != std::string::npos
        || lowered.find( "<!entity" ) != std::string::npos )
    {
        aError = "XAO artifact contains unsafe XML content";
        return false;
    }

    wxXmlDocument document;

    if( !document.Load( wxString::FromUTF8( aPath.string().c_str() ) ) )
    {
        aError = "XAO artifact is not well-formed XML";
        return false;
    }

    wxXmlNode* root = document.GetRoot();

    if( !root || root->GetName() != wxS( "XAO" )
        || root->GetAttribute( wxS( "version" ) ) != wxS( "1.0" )
        || root->GetAttribute( wxS( "author" ) ) != wxS( "KiCad" ) )
    {
        aError = "XAO artifact has the wrong native root schema";
        return false;
    }

    std::vector<std::pair<wxXmlNode*, size_t>> pending = { { root, 0 } };
    size_t nodes = 0;
    size_t geometries = 0;
    size_t brepShapes = 0;
    size_t topologies = 0;
    bool positiveSolids = false;

    while( !pending.empty() )
    {
        const auto [node, depth] = pending.back();
        pending.pop_back();

        if( depth > MAX_XML_DEPTH || ++nodes > MAX_XML_NODES )
        {
            aError = "XAO artifact exceeds structural limits";
            return false;
        }

        if( node->GetType() == wxXML_ELEMENT_NODE )
        {
            if( node->GetParent() == root
                && node->GetName() != wxS( "geometry" )
                && node->GetName() != wxS( "groups" )
                && node->GetName() != wxS( "fields" ) )
            {
                aError = "XAO root contains an unexpected collection";
                return false;
            }

            if( node->GetName() == wxS( "geometry" ) )
            {
                ++geometries;

                if( node->GetParent() != root
                    || node->GetAttribute( wxS( "name" ) )
                    != wxString::FromUTF8( aExpectedStem ) )
                {
                    aError = "XAO geometry identity differs from the planned board";
                    return false;
                }
            }
            else if( node->GetName() == wxS( "shape" ) )
            {
                std::string brepError;
                const std::string brep = node->GetNodeContent().ToStdString();

                if( !node->GetParent()
                    || node->GetParent()->GetName() != wxS( "geometry" )
                    || node->GetAttribute( wxS( "format" ) ) != wxS( "BREP" )
                    || !validateBrepContents( brep, brepError ) )
                {
                    aError = "XAO shape does not contain complete BREP geometry";
                    return false;
                }

                ++brepShapes;
            }
            else if( node->GetName() == wxS( "topology" ) )
            {
                if( !node->GetParent()
                    || node->GetParent()->GetName() != wxS( "geometry" ) )
                {
                    aError = "XAO topology is outside its geometry";
                    return false;
                }

                ++topologies;
            }
            else if( node->GetName() == wxS( "groups" )
                     || node->GetName() == wxS( "fields" ) )
            {
                size_t declared = 0;
                size_t actual = 0;

                if( node->GetParent() != root
                    || !parseCount( node->GetAttribute( wxS( "count" ) ), declared ) )
                {
                    aError = "XAO contains an invalid top-level collection";
                    return false;
                }

                for( wxXmlNode* child = node->GetChildren(); child; child = child->GetNext() )
                {
                    if( child->GetType() == wxXML_ELEMENT_NODE )
                        ++actual;
                }

                if( actual != declared )
                {
                    aError = "XAO top-level collection count does not match its contents";
                    return false;
                }
            }
            else if( node->GetParent() && node->GetParent()->GetName() == wxS( "topology" ) )
            {
                static const std::set<wxString> TOPOLOGY_COLLECTIONS = {
                    wxS( "vertices" ), wxS( "edges" ), wxS( "wires" ),
                    wxS( "faces" ), wxS( "shells" ), wxS( "solids" ),
                    wxS( "compsolids" ), wxS( "compounds" )
                };
                size_t declared = 0;
                size_t actual = 0;

                if( !TOPOLOGY_COLLECTIONS.contains( node->GetName() )
                    || !parseCount( node->GetAttribute( wxS( "count" ) ), declared ) )
                {
                    aError = "XAO topology contains an invalid collection";
                    return false;
                }

                for( wxXmlNode* child = node->GetChildren(); child; child = child->GetNext() )
                {
                    if( child->GetType() == wxXML_ELEMENT_NODE )
                        ++actual;
                }

                if( actual != declared )
                {
                    aError = "XAO topology collection count does not match its contents";
                    return false;
                }

                if( node->GetName() == wxS( "solids" ) && declared > 0 )
                    positiveSolids = true;
            }

            for( wxXmlNode* child = node->GetChildren(); child; child = child->GetNext() )
                pending.emplace_back( child, depth + 1 );
        }
    }

    if( geometries != 1 || brepShapes != 1 || topologies != 1 || !positiveSolids )
    {
        aError = "XAO artifact contains no complete native board geometry";
        return false;
    }

    return true;
}
