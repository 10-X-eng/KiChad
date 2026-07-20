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

#include "design_script_footprint_custom_pad_generator.h"

#include <cstdint>
#include <set>
#include <string>


namespace
{

using JSON = nlohmann::json;


std::string millimetres( int64_t aNanometers )
{
    if( aNanometers == 0 )
        return "0";

    const bool negative = aNanometers < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aNanometers + 1 ) ) + 1
                                       : static_cast<uint64_t>( aNanometers );
    const uint64_t whole = magnitude / 1'000'000;
    const uint64_t fraction = magnitude % 1'000'000;
    std::string result = negative ? "-" : "";
    result += std::to_string( whole );

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction );
        result += "." + std::string( 6 - digits.size(), '0' ) + digits;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


bool validPoint( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" )
           && aPoint["xNm"].is_number_integer() && aPoint.contains( "yNm" )
           && aPoint["yNm"].is_number_integer();
}


std::string nativePoint( const JSON& aPoint )
{
    return millimetres( aPoint["xNm"].get<int64_t>() ) + " "
           + millimetres( aPoint["yNm"].get<int64_t>() );
}


bool renderPrimitive( const JSON& aPrimitive, std::string& aSource )
{
    static const std::set<std::string> kinds = {
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
    };

    if( !aPrimitive.is_object() || !aPrimitive.contains( "kind" )
        || !aPrimitive["kind"].is_string()
        || !kinds.contains( aPrimitive["kind"].get<std::string>() )
        || !aPrimitive.contains( "widthNm" ) || !aPrimitive["widthNm"].is_number_integer()
        || aPrimitive["widthNm"].get<int64_t>() < 0
        || !aPrimitive.contains( "fill" ) || !aPrimitive["fill"].is_boolean() )
    {
        return false;
    }

    const std::string kind = aPrimitive["kind"].get<std::string>();
    const bool fillable = kind == "rectangle" || kind == "circle" || kind == "polygon";
    const std::string nativeKind = kind == "rectangle" ? "rect"
                                   : kind == "bezier" ? "curve"
                                   : kind == "polygon" ? "poly" : kind;
    aSource += "\t\t\t(gr_" + nativeKind + "\n";

    auto emitPoint = [&]( const char* aToken, const char* aField )
    {
        if( !aPrimitive.contains( aField ) || !validPoint( aPrimitive[aField] ) )
            return false;

        aSource += "\t\t\t\t(" + std::string( aToken ) + " "
                   + nativePoint( aPrimitive[aField] ) + ")\n";
        return true;
    };

    if( kind == "line" || kind == "rectangle" )
    {
        if( !emitPoint( "start", "start" ) || !emitPoint( "end", "end" ) )
            return false;

        if( kind == "rectangle" )
        {
            if( !aPrimitive.contains( "radiusNm" )
                || !aPrimitive["radiusNm"].is_number_integer()
                || aPrimitive["radiusNm"].get<int64_t>() < 0 )
            {
                return false;
            }

            if( aPrimitive["radiusNm"].get<int64_t>() > 0 )
                aSource += "\t\t\t\t(radius "
                           + millimetres( aPrimitive["radiusNm"].get<int64_t>() ) + ")\n";
        }
    }
    else if( kind == "arc" )
    {
        if( !emitPoint( "start", "start" ) || !emitPoint( "mid", "mid" )
            || !emitPoint( "end", "end" ) )
        {
            return false;
        }
    }
    else if( kind == "circle" )
    {
        if( !aPrimitive.contains( "center" ) || !validPoint( aPrimitive["center"] )
            || !aPrimitive.contains( "radiusNm" )
            || !aPrimitive["radiusNm"].is_number_integer()
            || aPrimitive["radiusNm"].get<int64_t>() <= 0 )
        {
            return false;
        }

        JSON endpoint = aPrimitive["center"];
        endpoint["xNm"] = endpoint["xNm"].get<int64_t>()
                           + aPrimitive["radiusNm"].get<int64_t>();
        aSource += "\t\t\t\t(center " + nativePoint( aPrimitive["center"] ) + ")\n"
                   "\t\t\t\t(end " + nativePoint( endpoint ) + ")\n";
    }
    else if( kind == "polygon" )
    {
        if( !aPrimitive.contains( "points" ) || !aPrimitive["points"].is_array()
            || aPrimitive["points"].size() < 3 )
        {
            return false;
        }

        aSource += "\t\t\t\t(pts\n";

        for( const JSON& point : aPrimitive["points"] )
        {
            if( !validPoint( point ) )
                return false;

            aSource += "\t\t\t\t\t(xy " + nativePoint( point ) + ")\n";
        }

        aSource += "\t\t\t\t)\n";
    }
    else if( kind == "bezier" )
    {
        const char* fields[] = { "start", "control1", "control2", "end" };

        for( const char* field : fields )
        {
            if( !aPrimitive.contains( field ) || !validPoint( aPrimitive[field] ) )
                return false;
        }

        aSource += "\t\t\t\t(pts\n"
                   "\t\t\t\t\t(xy " + nativePoint( aPrimitive["start"] ) + ")\n"
                   "\t\t\t\t\t(xy " + nativePoint( aPrimitive["control1"] ) + ")\n"
                   "\t\t\t\t\t(xy " + nativePoint( aPrimitive["control2"] ) + ")\n"
                   "\t\t\t\t\t(xy " + nativePoint( aPrimitive["end"] ) + ")\n"
                   "\t\t\t\t)\n";
    }

    aSource += "\t\t\t\t(width "
               + millimetres( aPrimitive["widthNm"].get<int64_t>() ) + ")\n";

    if( fillable )
        aSource += "\t\t\t\t(fill "
                   + std::string( aPrimitive["fill"].get<bool>() ? "yes" : "no" ) + ")\n";
    else if( aPrimitive["fill"].get<bool>() )
        return false;

    aSource += "\t\t\t)\n";
    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_GENERATOR::Render(
        const JSON& aCustom, std::string& aSource )
{
    if( !aCustom.is_object() || !aCustom.contains( "anchor" )
        || !aCustom["anchor"].is_string()
        || ( aCustom["anchor"] != "circle" && aCustom["anchor"] != "rect" )
        || !aCustom.contains( "clearance" ) || !aCustom["clearance"].is_string()
        || ( aCustom["clearance"] != "outline" && aCustom["clearance"] != "convex_hull" )
        || !aCustom.contains( "primitives" ) || !aCustom["primitives"].is_array()
        || aCustom["primitives"].empty() )
    {
        return false;
    }

    aSource += "\t\t(options\n"
               "\t\t\t(clearance "
               + std::string( aCustom["clearance"] == "convex_hull" ? "convexhull" : "outline" )
               + ")\n"
               "\t\t\t(anchor " + aCustom["anchor"].get<std::string>() + ")\n"
               "\t\t)\n"
               "\t\t(primitives\n";

    for( const JSON& primitive : aCustom["primitives"] )
    {
        if( !renderPrimitive( primitive, aSource ) )
            return false;
    }

    aSource += "\t\t)\n";
    return true;
}

} // namespace KICHAD
