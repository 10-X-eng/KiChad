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

#include "design_script_footprint_component_classes_generator.h"

#include <algorithm>
#include <set>
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
        if( character == '\\' )
            result += "\\\\";
        else if( character == '"' )
            result += "\\\"";
        else
            result.push_back( static_cast<char>( character ) );
    }

    result.push_back( '"' );
    return result;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_COMPONENT_CLASSES_GENERATOR::Render(
        const JSON& aClasses, std::string& aSource )
{
    if( !aClasses.is_array() )
        return false;

    if( aClasses.empty() )
        return true;

    if( aClasses.size() > 256 )
        return false;

    std::vector<std::string> names;
    std::set<std::string> unique;

    for( const JSON& entry : aClasses )
    {
        if( !entry.is_string() )
            return false;

        const std::string& name = entry.get_ref<const std::string&>();

        if( name.empty() || name.size() > 256 || name.find( '\0' ) != std::string::npos
            || name.find_first_of( "\r\n" ) != std::string::npos
            || !unique.emplace( name ).second )
        {
            return false;
        }

        names.push_back( name );
    }

    std::sort( names.begin(), names.end() );
    aSource += "\t(component_classes";

    for( const std::string& name : names )
        aSource += " (class " + quoteText( name ) + ")";

    aSource += ")\n";
    return true;
}

} // namespace KICHAD
