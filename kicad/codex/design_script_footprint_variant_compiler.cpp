/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "design_script_footprint_variant_compiler.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_VARIANT_COMPILER::RESULT;

constexpr size_t MAX_VARIANT_FIELDS = 256;
constexpr size_t MAX_FIELD_TEXT_BYTES = 4096;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool scalar( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( aNode >= aDocument.Nodes().size()
        || aDocument.Nodes()[aNode].kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool identifier( const std::string& aValue )
{
    if( aValue.empty() || aValue.size() > 128 )
        return false;

    return std::all_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
    {
        return std::isalnum( aCharacter ) || aCharacter == '_' || aCharacter == '-'
               || aCharacter == '.' || aCharacter == '+';
    } );
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


std::string caseFold( std::string aValue )
{
    std::transform( aValue.begin(), aValue.end(), aValue.begin(), []( unsigned char aCharacter )
    {
        return static_cast<char>( std::tolower( aCharacter ) );
    } );
    return aValue;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_VARIANT_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_VARIANT_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( aDocument.ListHead( aNode ) != "variant" || node.children.size() < 2
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_footprint_variant_id",
                    "footprint variant requires one unique bounded name" );
        return result;
    }

    result.variant = { { "id", id }, { "dnp", false }, { "excludeFromBom", false },
                       { "excludeFromPosition", false }, { "fields", JSON::array() } };
    std::set<std::string> flags;
    std::set<std::string> fieldNames;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "field" )
        {
            const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
            std::string name;
            std::string value;

            if( result.variant["fields"].size() >= MAX_VARIANT_FIELDS )
            {
                diagnostic( result, "too_many_authored_footprint_variant_fields",
                            "a footprint variant may override at most 256 fields" );
                continue;
            }

            if( field.children.size() != 3 || !scalar( aDocument, field.children[1], name )
                || name.empty() || name.size() > 256 || name.find( '\0' ) != std::string::npos
                || caseFold( name ) == "reference"
                || !scalar( aDocument, field.children[2], value )
                || value.size() > MAX_FIELD_TEXT_BYTES
                || value.find( '\0' ) != std::string::npos )
            {
                diagnostic( result, "invalid_authored_footprint_variant_field",
                            "variant field requires a bounded non-Reference name and bounded value" );
                continue;
            }

            if( !fieldNames.emplace( caseFold( name ) ).second )
            {
                diagnostic( result, "duplicate_authored_footprint_variant_field",
                            "variant field " + name + " occurs more than once" );
                continue;
            }

            result.variant["fields"].push_back( { { "name", name }, { "value", value } } );
            continue;
        }

        if( head != "dnp" && head != "exclude_from_bom" && head != "exclude_from_position" )
        {
            diagnostic( result, "unknown_authored_footprint_variant_field",
                        "variant supports dnp, exclude_from_bom, exclude_from_position, and field" );
            continue;
        }

        std::string value;

        if( !flags.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_variant_flag",
                        "variant flag " + head + " occurs more than once" );
        }
        else if( !oneValue( aDocument, child, value )
                 || ( value != "true" && value != "false" ) )
        {
            diagnostic( result, "invalid_authored_footprint_variant_flag",
                        "variant flag " + head + " must be true or false" );
        }
        else
        {
            const char* key = head == "exclude_from_bom" ? "excludeFromBom"
                              : head == "exclude_from_position" ? "excludeFromPosition" : "dnp";
            result.variant[key] = value == "true";
        }
    }

    for( const char* required : { "dnp", "exclude_from_bom", "exclude_from_position" } )
    {
        if( !flags.contains( required ) )
            diagnostic( result, "missing_authored_footprint_variant_flag",
                        std::string( "variant requires explicit " ) + required );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
