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

#include "design_script_footprint_group_compiler.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_GROUP_COMPILER::RESULT;

constexpr size_t MAX_GROUP_MEMBERS = 4096;


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

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_GROUP_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_GROUP_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( aDocument.ListHead( aNode ) != "group" || node.children.size() < 2
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_footprint_group_id",
                    "footprint group requires one unique bounded logical ID" );
        return result;
    }

    result.group = { { "id", id }, { "name", id }, { "locked", false },
                     { "members", JSON::array() } };
    std::set<std::string> singletonFields;
    std::set<std::pair<std::string, std::string>> uniqueMembers;
    static const std::set<std::string> memberTypes = {
        "pad", "graphic", "text", "text_box", "zone", "group"
    };

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "member" )
        {
            const DOCUMENT::NODE& member = aDocument.Nodes().at( child );
            std::string type;
            std::string logicalId;

            if( result.group["members"].size() >= MAX_GROUP_MEMBERS )
            {
                diagnostic( result, "too_many_authored_footprint_group_members",
                            "a footprint group may contain at most 4096 members" );
                continue;
            }

            if( member.children.size() != 3
                || !scalar( aDocument, member.children[1], type )
                || !memberTypes.contains( type )
                || !scalar( aDocument, member.children[2], logicalId )
                || !identifier( logicalId ) )
            {
                diagnostic( result, "invalid_authored_footprint_group_member",
                            "group member requires type pad, graphic, text, text_box, zone, or group and one logical ID" );
                continue;
            }

            if( !uniqueMembers.emplace( type, logicalId ).second )
            {
                diagnostic( result, "duplicate_authored_footprint_group_member",
                            "group member " + type + ":" + logicalId + " occurs more than once" );
                continue;
            }

            result.group["members"].push_back( { { "type", type }, { "id", logicalId } } );
            continue;
        }

        if( head != "name" && head != "locked" )
        {
            diagnostic( result, "unknown_authored_footprint_group_field",
                        "footprint group supports name, locked, and member" );
            continue;
        }

        if( !singletonFields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_group_field",
                        "footprint group field " + head + " occurs more than once" );
            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_footprint_group_field",
                        "footprint group " + head + " requires one value" );
        }
        else if( head == "name" )
        {
            if( value.empty() || value.size() > 256 || value.find( '\0' ) != std::string::npos )
                diagnostic( result, "invalid_authored_footprint_group_name",
                            "group name must contain 1 through 256 bounded UTF-8 bytes" );
            else
                result.group["name"] = value;
        }
        else if( value != "true" && value != "false" )
        {
            diagnostic( result, "invalid_authored_footprint_group_locked",
                        "group locked must be true or false" );
        }
        else
        {
            result.group["locked"] = value == "true";
        }
    }

    if( result.group["members"].empty() )
        diagnostic( result, "empty_authored_footprint_group",
                    "footprint group requires at least one member" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
