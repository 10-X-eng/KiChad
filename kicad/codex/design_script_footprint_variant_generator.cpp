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

#include "design_script_footprint_variant_generator.h"

#include <string>


namespace
{

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

bool DESIGN_SCRIPT_FOOTPRINT_VARIANT_GENERATOR::Render(
        const JSON& aVariant, std::string& aSource )
{
    if( !aVariant.is_object() || !aVariant.contains( "id" ) || !aVariant["id"].is_string()
        || !aVariant.contains( "dnp" ) || !aVariant["dnp"].is_boolean()
        || !aVariant.contains( "excludeFromBom" ) || !aVariant["excludeFromBom"].is_boolean()
        || !aVariant.contains( "excludeFromPosition" )
        || !aVariant["excludeFromPosition"].is_boolean()
        || !aVariant.contains( "fields" ) || !aVariant["fields"].is_array() )
    {
        return false;
    }

    aSource += "\t(variant\n"
               "\t\t(name " + quoteText( aVariant["id"].get<std::string>() ) + ")\n"
               "\t\t(dnp " + std::string( aVariant["dnp"].get<bool>() ? "yes" : "no" ) + ")\n"
               "\t\t(exclude_from_bom "
               + std::string( aVariant["excludeFromBom"].get<bool>() ? "yes" : "no" ) + ")\n"
               "\t\t(exclude_from_pos_files "
               + std::string( aVariant["excludeFromPosition"].get<bool>() ? "yes" : "no" ) + ")\n";

    for( const JSON& field : aVariant["fields"] )
    {
        if( !field.is_object() || !field.contains( "name" ) || !field["name"].is_string()
            || !field.contains( "value" ) || !field["value"].is_string() )
        {
            return false;
        }

        aSource += "\t\t(field (name " + quoteText( field["name"].get<std::string>() )
                   + ") (value " + quoteText( field["value"].get<std::string>() ) + "))\n";
    }

    aSource += "\t)\n";
    return true;
}

} // namespace KICHAD
