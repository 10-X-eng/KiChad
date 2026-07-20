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

#include "design_script_footprint_group_generator.h"

#include "design_script_pcb_planner.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>


namespace
{

using JSON = nlohmann::json;


std::string quoteText( const std::string& aText )
{
    std::string result = "\"";

    for( unsigned char character : aText )
    {
        switch( character )
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result.push_back( static_cast<char>( character ) ); break;
        }
    }

    result.push_back( '"' );
    return result;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_GROUP_GENERATOR::Render(
        const JSON& aGroup, const std::string& aProject,
        const std::string& aFootprintId, std::string& aSource )
{
    if( !aGroup.is_object() || !aGroup.contains( "id" ) || !aGroup["id"].is_string()
        || !aGroup.contains( "name" ) || !aGroup["name"].is_string()
        || !aGroup.contains( "locked" ) || !aGroup["locked"].is_boolean()
        || !aGroup.contains( "members" ) || !aGroup["members"].is_array()
        || aGroup["members"].empty() )
    {
        return false;
    }

    static const std::map<std::string, std::string> domains = {
        { "pad", "footprint-pad" }, { "graphic", "footprint-graphic" },
        { "text", "footprint-text" }, { "text_box", "footprint-text-box" },
        { "zone", "footprint-zone" }, { "group", "footprint-group" }
    };
    const std::string groupId = aGroup["id"].get<std::string>();
    const std::string uuid = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
            aProject, "footprint-group", aFootprintId + ":" + groupId );
    std::vector<std::string> memberUuids;

    for( const JSON& member : aGroup["members"] )
    {
        if( !member.is_object() || !member.contains( "type" ) || !member["type"].is_string()
            || !member.contains( "id" ) || !member["id"].is_string() )
        {
            return false;
        }

        const std::string type = member["type"].get<std::string>();
        const std::string id = member["id"].get<std::string>();

        if( !domains.contains( type ) )
            return false;

        memberUuids.push_back( KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                aProject, domains.at( type ), aFootprintId + ":" + id ) );
    }

    std::sort( memberUuids.begin(), memberUuids.end() );
    aSource += "\t(group " + quoteText( aGroup["name"].get<std::string>() ) + "\n"
               "\t\t(uuid " + quoteText( uuid ) + ")\n";

    if( aGroup["locked"].get<bool>() )
        aSource += "\t\t(locked yes)\n";

    aSource += "\t\t(members";

    for( const std::string& memberUuid : memberUuids )
        aSource += " " + quoteText( memberUuid );

    aSource += ")\n\t)\n";
    return true;
}

} // namespace KICHAD
