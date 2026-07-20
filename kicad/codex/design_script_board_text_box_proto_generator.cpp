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

#include "design_script_board_text_box_proto_generator.h"

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


JSON vectorProto( int64_t aX, int64_t aY )
{
    return { { "xNm", std::to_string( aX ) }, { "yNm", std::to_string( aY ) } };
}


JSON vectorProto( const JSON& aPoint )
{
    return vectorProto( aPoint["xNm"].get<int64_t>(), aPoint["yNm"].get<int64_t>() );
}


std::string layerEnum( const std::string& aLayer )
{
    std::string result = "BL_" + aLayer;

    for( char& character : result )
    {
        if( character == '.' )
            character = '_';
    }

    return result;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_BOARD_TEXT_BOX_PROTO_GENERATOR::Render(
        const JSON& aTextBox, const std::string& aProject, JSON& aItem )
{
    static const std::map<std::string, std::string> styles = {
        { "solid", "SLS_SOLID" }, { "dash", "SLS_DASH" }, { "dot", "SLS_DOT" },
        { "dash_dot", "SLS_DASHDOT" }, { "dash_dot_dot", "SLS_DASHDOTDOT" }
    };
    static const std::map<std::string, std::string> horizontalEnums = {
        { "left", "HA_LEFT" }, { "center", "HA_CENTER" }, { "right", "HA_RIGHT" }
    };
    static const std::map<std::string, std::string> verticalEnums = {
        { "top", "VA_TOP" }, { "center", "VA_CENTER" }, { "bottom", "VA_BOTTOM" }
    };

    if( !aTextBox.is_object() || aProject.empty()
        || aTextBox.value( "kind", "" ) != "text_box"
        || !aTextBox.contains( "id" ) || !aTextBox["id"].is_string()
        || aTextBox["id"].get_ref<const std::string&>().empty()
        || !aTextBox.contains( "content" ) || !aTextBox["content"].is_string()
        || !aTextBox.contains( "outline" ) || !aTextBox["outline"].is_string()
        || !aTextBox.contains( "rotationTenths" )
        || !aTextBox["rotationTenths"].is_number_integer()
        || !aTextBox.contains( "layer" ) || !aTextBox["layer"].is_string()
        || aTextBox["layer"].get_ref<const std::string&>().empty()
        || !aTextBox.contains( "locked" ) || !aTextBox["locked"].is_boolean()
        || !aTextBox.contains( "knockout" ) || !aTextBox["knockout"].is_boolean()
        || !aTextBox.contains( "border" ) || !aTextBox["border"].is_boolean()
        || !aTextBox.contains( "font" ) || !aTextBox["font"].is_object()
        || !aTextBox.contains( "justify" ) || !aTextBox["justify"].is_object()
        || !aTextBox.contains( "stroke" ) || !aTextBox["stroke"].is_object()
        || !aTextBox.contains( "margins" ) || !aTextBox["margins"].is_object()
        || !aTextBox.contains( "hyperlink" ) || !aTextBox["hyperlink"].is_string() )
    {
        return false;
    }

    const JSON& font = aTextBox["font"];
    const JSON& justify = aTextBox["justify"];
    const JSON& stroke = aTextBox["stroke"];
    const JSON& margins = aTextBox["margins"];
    const char* marginFields[] = { "leftNm", "topNm", "rightNm", "bottomNm" };

    if( !font.contains( "face" ) || !font["face"].is_string()
        || !font.contains( "size" ) || !font["size"].is_object()
        || !font["size"].contains( "heightNm" )
        || !font["size"]["heightNm"].is_number_integer()
        || !font["size"].contains( "widthNm" )
        || !font["size"]["widthNm"].is_number_integer()
        || !font.contains( "lineSpacingPpm" ) || !font["lineSpacingPpm"].is_number_integer()
        || !font.contains( "thicknessNm" )
        || !( font["thicknessNm"].is_null() || font["thicknessNm"].is_number_integer() )
        || !font.contains( "bold" ) || !font["bold"].is_boolean()
        || !font.contains( "italic" ) || !font["italic"].is_boolean()
        || !justify.contains( "horizontal" ) || !justify["horizontal"].is_string()
        || !horizontalEnums.contains( justify["horizontal"].get<std::string>() )
        || !justify.contains( "vertical" ) || !justify["vertical"].is_string()
        || !verticalEnums.contains( justify["vertical"].get<std::string>() )
        || !justify.contains( "mirrored" ) || !justify["mirrored"].is_boolean()
        || !stroke.contains( "widthNm" ) || !stroke["widthNm"].is_number_integer()
        || stroke["widthNm"].get<int64_t>() < 0
        || !stroke.contains( "style" ) || !stroke["style"].is_string()
        || !styles.contains( stroke["style"].get<std::string>() ) )
    {
        return false;
    }

    for( const char* field : marginFields )
    {
        if( !margins.contains( field ) || !margins[field].is_number_integer()
            || margins[field].get<int64_t>() < 0 )
        {
            return false;
        }
    }

    const int64_t height = font["size"]["heightNm"].get<int64_t>();
    const int64_t width = font["size"]["widthNm"].get<int64_t>();
    int64_t left = 0;
    int64_t top = 0;
    int64_t right = 0;
    int64_t bottom = 0;
    JSON shape = {
        { "attributes",
          { { "stroke",
              { { "width", { { "valueNm", std::to_string(
                                           stroke["widthNm"].get<int64_t>() ) } } },
                { "style", styles.at( stroke["style"].get<std::string>() ) } } },
            { "fill", { { "fillType", "GFT_UNFILLED" } } } } }
    };

    if( height <= 0 || width <= 0 )
        return false;

    if( aTextBox["outline"] == "rectangle" )
    {
        if( !aTextBox.contains( "start" ) || !point( aTextBox["start"] )
            || !aTextBox.contains( "end" ) || !point( aTextBox["end"] ) )
        {
            return false;
        }

        left = std::min( aTextBox["start"]["xNm"].get<int64_t>(),
                         aTextBox["end"]["xNm"].get<int64_t>() );
        top = std::min( aTextBox["start"]["yNm"].get<int64_t>(),
                        aTextBox["end"]["yNm"].get<int64_t>() );
        right = std::max( aTextBox["start"]["xNm"].get<int64_t>(),
                          aTextBox["end"]["xNm"].get<int64_t>() );
        bottom = std::max( aTextBox["start"]["yNm"].get<int64_t>(),
                           aTextBox["end"]["yNm"].get<int64_t>() );
        shape["rectangle"] = {
            { "topLeft", vectorProto( left, top ) },
            { "bottomRight", vectorProto( right, bottom ) },
            { "cornerRadius", { { "valueNm", "0" } } }
        };
    }
    else if( aTextBox["outline"] == "polygon" )
    {
        if( !aTextBox.contains( "points" ) || !aTextBox["points"].is_array()
            || aTextBox["points"].size() < 3 )
        {
            return false;
        }

        JSON nodes = JSON::array();
        bool first = true;

        for( const JSON& item : aTextBox["points"] )
        {
            if( !point( item ) )
                return false;

            const int64_t x = item["xNm"].get<int64_t>();
            const int64_t y = item["yNm"].get<int64_t>();

            if( first )
            {
                left = right = x;
                top = bottom = y;
                first = false;
            }
            else
            {
                left = std::min( left, x );
                top = std::min( top, y );
                right = std::max( right, x );
                bottom = std::max( bottom, y );
            }

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
        return false;
    }

    const int64_t textStroke = font["thicknessNm"].is_number_integer()
                                     ? font["thicknessNm"].get<int64_t>() : 0;
    const std::string face = font["face"].get<std::string>();
    const std::string logicalId = aTextBox["id"].get<std::string>();

    aItem = {
        { "id", { { "value", DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                                    aProject, "textbox", logicalId ) } } },
        { "textbox",
          { { "topLeft", vectorProto( left, top ) },
            { "bottomRight", vectorProto( right, bottom ) },
            { "attributes",
              { { "fontName", face == "default" ? "" : face },
                { "horizontalAlignment",
                  horizontalEnums.at( justify["horizontal"].get<std::string>() ) },
                { "verticalAlignment",
                  verticalEnums.at( justify["vertical"].get<std::string>() ) },
                { "angle",
                  { { "valueDegrees",
                      static_cast<double>( aTextBox["rotationTenths"].get<int64_t>() ) / 10.0 } } },
                { "lineSpacing",
                  static_cast<double>( font["lineSpacingPpm"].get<int64_t>() ) / 1'000'000.0 },
                { "strokeWidth", { { "valueNm", std::to_string( textStroke ) } } },
                { "italic", font["italic"] }, { "bold", font["bold"] },
                { "underlined", false }, { "visible", true },
                { "mirrored", justify["mirrored"] }, { "multiline", true },
                { "keepUpright", false },
                { "size", vectorProto( width, height ) } } },
            { "text", aTextBox["content"] },
            { "hyperlink", aTextBox["hyperlink"] } } },
        { "layer", layerEnum( aTextBox["layer"].get<std::string>() ) },
        { "locked", aTextBox["locked"].get<bool>() ? "LS_LOCKED" : "LS_UNLOCKED" },
        { "outline", std::move( shape ) },
        { "margins",
          { { "left", { { "valueNm", std::to_string( margins["leftNm"].get<int64_t>() ) } } },
            { "top", { { "valueNm", std::to_string( margins["topNm"].get<int64_t>() ) } } },
            { "right", { { "valueNm", std::to_string( margins["rightNm"].get<int64_t>() ) } } },
            { "bottom", { { "valueNm", std::to_string( margins["bottomNm"].get<int64_t>() ) } } } } },
        { "borderEnabled", aTextBox["border"] },
        { "knockout", aTextBox["knockout"] }
    };
    return true;
}

} // namespace KICHAD
