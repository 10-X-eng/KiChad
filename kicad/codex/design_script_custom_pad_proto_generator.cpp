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

#include "design_script_custom_pad_proto_generator.h"

#include "design_script_pcb_planner.h"

#include <cstdint>
#include <set>
#include <string>


namespace
{

using JSON = nlohmann::json;


bool point( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" )
           && aPoint["xNm"].is_number_integer() && aPoint.contains( "yNm" )
           && aPoint["yNm"].is_number_integer();
}


JSON vectorProto( const JSON& aPoint )
{
    return { { "xNm", std::to_string( aPoint["xNm"].get<int64_t>() ) },
             { "yNm", std::to_string( aPoint["yNm"].get<int64_t>() ) } };
}


bool renderPrimitive( const JSON& aPrimitive, const std::string& aProject,
                      const std::string& aOwnerId, const std::string& aLayer,
                      JSON& aShape )
{
    static const std::set<std::string> kinds = {
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
    };

    if( !aPrimitive.is_object() || !aPrimitive.contains( "id" )
        || !aPrimitive["id"].is_string() || aPrimitive["id"].get_ref<const std::string&>().empty()
        || !aPrimitive.contains( "kind" ) || !aPrimitive["kind"].is_string()
        || !kinds.contains( aPrimitive["kind"].get<std::string>() )
        || !aPrimitive.contains( "widthNm" ) || !aPrimitive["widthNm"].is_number_integer()
        || aPrimitive["widthNm"].get<int64_t>() < 0
        || !aPrimitive.contains( "fill" ) || !aPrimitive["fill"].is_boolean() )
    {
        return false;
    }

    const std::string kind = aPrimitive["kind"].get<std::string>();
    const bool fillable = kind == "rectangle" || kind == "circle" || kind == "polygon";

    if( !fillable && aPrimitive["fill"].get<bool>() )
        return false;

    JSON nativeShape = {
        { "attributes",
          { { "stroke",
              { { "width",
                  { { "valueNm", std::to_string( aPrimitive["widthNm"].get<int64_t>() ) } } },
                { "style", "SLS_SOLID" } } },
            { "fill",
              { { "fillType", aPrimitive["fill"].get<bool>()
                                      ? "GFT_FILLED" : "GFT_UNFILLED" } } } } }
    };

    const auto requirePoint = [&]( const char* aName )
    {
        return aPrimitive.contains( aName ) && point( aPrimitive[aName] );
    };

    if( kind == "line" )
    {
        if( !requirePoint( "start" ) || !requirePoint( "end" ) )
            return false;

        nativeShape["segment"] = { { "start", vectorProto( aPrimitive["start"] ) },
                                     { "end", vectorProto( aPrimitive["end"] ) } };
    }
    else if( kind == "rectangle" )
    {
        if( !requirePoint( "start" ) || !requirePoint( "end" )
            || !aPrimitive.contains( "radiusNm" )
            || !aPrimitive["radiusNm"].is_number_integer()
            || aPrimitive["radiusNm"].get<int64_t>() < 0 )
        {
            return false;
        }

        nativeShape["rectangle"] = {
            { "topLeft", vectorProto( aPrimitive["start"] ) },
            { "bottomRight", vectorProto( aPrimitive["end"] ) },
            { "cornerRadius",
              { { "valueNm", std::to_string( aPrimitive["radiusNm"].get<int64_t>() ) } } }
        };
    }
    else if( kind == "arc" )
    {
        if( !requirePoint( "start" ) || !requirePoint( "mid" ) || !requirePoint( "end" ) )
            return false;

        nativeShape["arc"] = { { "start", vectorProto( aPrimitive["start"] ) },
                                 { "mid", vectorProto( aPrimitive["mid"] ) },
                                 { "end", vectorProto( aPrimitive["end"] ) } };
    }
    else if( kind == "circle" )
    {
        if( !requirePoint( "center" ) || !aPrimitive.contains( "radiusNm" )
            || !aPrimitive["radiusNm"].is_number_integer()
            || aPrimitive["radiusNm"].get<int64_t>() <= 0 )
        {
            return false;
        }

        JSON radiusPoint = aPrimitive["center"];
        radiusPoint["xNm"] = radiusPoint["xNm"].get<int64_t>()
                              + aPrimitive["radiusNm"].get<int64_t>();
        nativeShape["circle"] = { { "center", vectorProto( aPrimitive["center"] ) },
                                    { "radiusPoint", vectorProto( radiusPoint ) } };
    }
    else if( kind == "polygon" )
    {
        if( !aPrimitive.contains( "points" ) || !aPrimitive["points"].is_array()
            || aPrimitive["points"].size() < 3 )
        {
            return false;
        }

        JSON nodes = JSON::array();

        for( const JSON& item : aPrimitive["points"] )
        {
            if( !point( item ) )
                return false;

            nodes.push_back( { { "point", vectorProto( item ) } } );
        }

        nativeShape["polygon"] = {
            { "polygons",
              JSON::array( { { { "outline", { { "nodes", std::move( nodes ) },
                                                 { "closed", true } } },
                              { "holes", JSON::array() } } } ) }
        };
    }
    else
    {
        if( !requirePoint( "start" ) || !requirePoint( "control1" )
            || !requirePoint( "control2" ) || !requirePoint( "end" ) )
        {
            return false;
        }

        nativeShape["bezier"] = {
            { "start", vectorProto( aPrimitive["start"] ) },
            { "control1", vectorProto( aPrimitive["control1"] ) },
            { "control2", vectorProto( aPrimitive["control2"] ) },
            { "end", vectorProto( aPrimitive["end"] ) }
        };
    }

    const std::string primitiveId = aPrimitive["id"].get<std::string>();
    aShape = {
        { "shape", std::move( nativeShape ) },
        { "layer", aLayer },
        { "id",
          { { "value", KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                               aProject, "via_custom_primitive",
                               aOwnerId + ":" + aLayer + ":" + primitiveId ) } } },
        { "locked", "LS_UNLOCKED" }
    };
    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_CUSTOM_PAD_PROTO_GENERATOR::Render(
        const JSON& aCustom, const std::string& aProject, const std::string& aOwnerId,
        const std::string& aLayer, JSON& aShapes )
{
    if( !aCustom.is_object() || !aCustom.contains( "anchor" )
        || !aCustom["anchor"].is_string()
        || ( aCustom["anchor"] != "circle" && aCustom["anchor"] != "rect" )
        || !aCustom.contains( "clearance" ) || !aCustom["clearance"].is_null()
        || !aCustom.contains( "primitives" ) || !aCustom["primitives"].is_array()
        || aCustom["primitives"].empty() || aProject.empty() || aOwnerId.empty()
        || aLayer.empty() )
    {
        return false;
    }

    JSON shapes = JSON::array();

    for( const JSON& primitive : aCustom["primitives"] )
    {
        JSON shape;

        if( !renderPrimitive( primitive, aProject, aOwnerId, aLayer, shape ) )
            return false;

        shapes.push_back( std::move( shape ) );
    }

    aShapes = std::move( shapes );
    return true;
}

} // namespace KICHAD
