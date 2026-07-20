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

#include "design_script_board_graphic_proto_generator.h"

#include "design_script_pcb_planner.h"

#include <algorithm>
#include <cstdint>
#include <map>
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

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_BOARD_GRAPHIC_PROTO_GENERATOR::Render(
        const JSON& aGraphic, const std::string& aProject, JSON& aItem )
{
    static const std::set<std::string> kinds = {
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
    };
    static const std::map<std::string, std::string> styles = {
        { "solid", "SLS_SOLID" }, { "dash", "SLS_DASH" }, { "dot", "SLS_DOT" },
        { "dash_dot", "SLS_DASHDOT" }, { "dash_dot_dot", "SLS_DASHDOTDOT" }
    };

    if( !aGraphic.is_object() || aProject.empty()
        || !aGraphic.contains( "id" ) || !aGraphic["id"].is_string()
        || aGraphic["id"].get_ref<const std::string&>().empty()
        || !aGraphic.contains( "kind" ) || !aGraphic["kind"].is_string()
        || !kinds.contains( aGraphic["kind"].get<std::string>() )
        || !aGraphic.contains( "stroke" ) || !aGraphic["stroke"].is_object()
        || !aGraphic["stroke"].contains( "widthNm" )
        || !aGraphic["stroke"]["widthNm"].is_number_integer()
        || aGraphic["stroke"]["widthNm"].get<int64_t>() < 0
        || !aGraphic["stroke"].contains( "style" )
        || !aGraphic["stroke"]["style"].is_string()
        || !styles.contains( aGraphic["stroke"]["style"].get<std::string>() )
        || !aGraphic.contains( "fill" ) || !aGraphic["fill"].is_string()
        || ( aGraphic["fill"] != "none" && aGraphic["fill"] != "solid" )
        || !aGraphic.contains( "layers" ) || !aGraphic["layers"].is_array()
        || aGraphic["layers"].size() != 1 || !aGraphic["layers"][0].is_string()
        || !aGraphic.contains( "locked" ) || !aGraphic["locked"].is_boolean() )
    {
        return false;
    }

    const std::string kind = aGraphic["kind"].get<std::string>();
    JSON shape = {
        { "attributes",
          { { "stroke",
              { { "width",
                  { { "valueNm", std::to_string(
                            aGraphic["stroke"]["widthNm"].get<int64_t>() ) } } },
                { "style", styles.at( aGraphic["stroke"]["style"].get<std::string>() ) } } },
            { "fill",
              { { "fillType", aGraphic["fill"] == "solid"
                                      ? "GFT_FILLED" : "GFT_UNFILLED" } } } } }
    };
    const auto requirePoint = [&]( const char* aField )
    {
        return aGraphic.contains( aField ) && point( aGraphic[aField] );
    };

    if( kind == "line" )
    {
        if( !requirePoint( "start" ) || !requirePoint( "end" ) )
            return false;

        shape["segment"] = { { "start", vectorProto( aGraphic["start"] ) },
                               { "end", vectorProto( aGraphic["end"] ) } };
    }
    else if( kind == "rectangle" )
    {
        if( !requirePoint( "start" ) || !requirePoint( "end" )
            || !aGraphic.contains( "radiusNm" )
            || !aGraphic["radiusNm"].is_number_integer()
            || aGraphic["radiusNm"].get<int64_t>() < 0 )
        {
            return false;
        }

        const int64_t x1 = aGraphic["start"]["xNm"].get<int64_t>();
        const int64_t y1 = aGraphic["start"]["yNm"].get<int64_t>();
        const int64_t x2 = aGraphic["end"]["xNm"].get<int64_t>();
        const int64_t y2 = aGraphic["end"]["yNm"].get<int64_t>();
        shape["rectangle"] = {
            { "topLeft", vectorProto( JSON{ { "xNm", std::min( x1, x2 ) },
                                              { "yNm", std::min( y1, y2 ) } } ) },
            { "bottomRight", vectorProto( JSON{ { "xNm", std::max( x1, x2 ) },
                                                  { "yNm", std::max( y1, y2 ) } } ) },
            { "cornerRadius",
              { { "valueNm", std::to_string( aGraphic["radiusNm"].get<int64_t>() ) } } }
        };
    }
    else if( kind == "arc" )
    {
        if( !requirePoint( "start" ) || !requirePoint( "mid" ) || !requirePoint( "end" ) )
            return false;

        shape["arc"] = { { "start", vectorProto( aGraphic["start"] ) },
                           { "mid", vectorProto( aGraphic["mid"] ) },
                           { "end", vectorProto( aGraphic["end"] ) } };
    }
    else if( kind == "circle" )
    {
        if( !requirePoint( "center" ) || !aGraphic.contains( "radiusNm" )
            || !aGraphic["radiusNm"].is_number_integer()
            || aGraphic["radiusNm"].get<int64_t>() <= 0 )
        {
            return false;
        }

        JSON radiusPoint = aGraphic["center"];
        radiusPoint["xNm"] = radiusPoint["xNm"].get<int64_t>()
                              + aGraphic["radiusNm"].get<int64_t>();
        shape["circle"] = { { "center", vectorProto( aGraphic["center"] ) },
                              { "radiusPoint", vectorProto( radiusPoint ) } };
    }
    else if( kind == "polygon" )
    {
        if( !aGraphic.contains( "points" ) || !aGraphic["points"].is_array()
            || aGraphic["points"].size() < 3 )
        {
            return false;
        }

        JSON nodes = JSON::array();

        for( const JSON& item : aGraphic["points"] )
        {
            if( !point( item ) )
                return false;

            nodes.push_back( { { "point", vectorProto( item ) } } );
        }

        shape["polygon"] = {
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

        shape["bezier"] = {
            { "start", vectorProto( aGraphic["start"] ) },
            { "control1", vectorProto( aGraphic["control1"] ) },
            { "control2", vectorProto( aGraphic["control2"] ) },
            { "end", vectorProto( aGraphic["end"] ) }
        };
    }

    const std::string logicalId = aGraphic["id"].get<std::string>();
    aItem = {
        { "id", { { "value", DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                                    aProject, "shape", logicalId ) } } },
        { "shape", std::move( shape ) },
        { "layer", "BL_Edge_Cuts" },
        { "locked", aGraphic["locked"].get<bool>() ? "LS_LOCKED" : "LS_UNLOCKED" }
    };
    return true;
}

} // namespace KICHAD
