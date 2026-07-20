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

#include "design_script_board_table_proto_generator.h"

#include "design_script_pcb_planner.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>


namespace
{

using JSON = nlohmann::json;


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


JSON vectorProto( int64_t aX, int64_t aY )
{
    return { { "xNm", std::to_string( aX ) }, { "yNm", std::to_string( aY ) } };
}


bool validColor( const JSON& aColor )
{
    if( aColor.is_null() )
        return true;

    if( !aColor.is_object() )
        return false;

    for( const char* channel : { "r", "g", "b", "a" } )
    {
        if( !aColor.contains( channel ) || !aColor[channel].is_number_integer()
            || aColor[channel].get<int>() < 0 || aColor[channel].get<int>() > 255 )
        {
            return false;
        }
    }

    return true;
}


bool renderStroke( const JSON& aStroke, JSON& aOutput )
{
    static const std::map<std::string, std::string> styles = {
        { "default", "SLS_DEFAULT" }, { "solid", "SLS_SOLID" },
        { "dash", "SLS_DASH" }, { "dot", "SLS_DOT" },
        { "dash_dot", "SLS_DASHDOT" }, { "dash_dot_dot", "SLS_DASHDOTDOT" }
    };

    if( !aStroke.is_object() || !aStroke.contains( "widthNm" )
        || !aStroke["widthNm"].is_number_integer()
        || !aStroke.contains( "style" ) || !aStroke["style"].is_string()
        || !styles.contains( aStroke["style"].get<std::string>() )
        || !aStroke.contains( "color" ) || !validColor( aStroke["color"] ) )
    {
        return false;
    }

    aOutput = {
        { "width", { { "valueNm", std::to_string( aStroke["widthNm"].get<int64_t>() ) } } },
        { "style", styles.at( aStroke["style"].get<std::string>() ) }
    };

    if( aStroke["color"].is_object() )
    {
        aOutput["color"] = {
            { "r", aStroke["color"]["r"].get<double>() / 255.0 },
            { "g", aStroke["color"]["g"].get<double>() / 255.0 },
            { "b", aStroke["color"]["b"].get<double>() / 255.0 },
            { "a", aStroke["color"]["a"].get<double>() / 255.0 }
        };
    }

    return true;
}


bool validCell( const JSON& aCell )
{
    if( !aCell.is_object() || !aCell.contains( "row" ) || !aCell["row"].is_number_unsigned()
        || !aCell.contains( "column" ) || !aCell["column"].is_number_unsigned()
        || !aCell.contains( "content" ) || !aCell["content"].is_string()
        || !aCell.contains( "margins" ) || !aCell["margins"].is_object()
        || !aCell.contains( "size" ) || !aCell["size"].is_object()
        || !aCell["size"].contains( "xNm" ) || !aCell["size"]["xNm"].is_number_integer()
        || !aCell["size"].contains( "yNm" ) || !aCell["size"]["yNm"].is_number_integer()
        || !aCell.contains( "font" ) || !aCell["font"].is_string()
        || !aCell.contains( "lineSpacing" ) || !aCell["lineSpacing"].is_number()
        || !aCell.contains( "thicknessNm" ) || !aCell["thicknessNm"].is_number_integer()
        || !aCell.contains( "justify" ) || !aCell["justify"].is_object()
        || !aCell.contains( "mirror" ) || !aCell["mirror"].is_boolean()
        || !aCell.contains( "bold" ) || !aCell["bold"].is_boolean()
        || !aCell.contains( "italic" ) || !aCell["italic"].is_boolean()
        || !aCell.contains( "hyperlink" ) || !aCell["hyperlink"].is_string()
        || !aCell.contains( "locked" ) || !aCell["locked"].is_boolean() )
    {
        return false;
    }

    static const std::set<std::string> horizontal = { "left", "center", "right" };
    static const std::set<std::string> vertical = { "top", "center", "bottom" };
    const JSON& justify = aCell["justify"];

    if( !justify.contains( "horizontal" ) || !justify["horizontal"].is_string()
        || !horizontal.contains( justify["horizontal"].get<std::string>() )
        || !justify.contains( "vertical" ) || !justify["vertical"].is_string()
        || !vertical.contains( justify["vertical"].get<std::string>() ) )
    {
        return false;
    }

    for( const char* field : { "leftNm", "topNm", "rightNm", "bottomNm" } )
    {
        if( !aCell["margins"].contains( field )
            || !aCell["margins"][field].is_number_integer()
            || aCell["margins"][field].get<int64_t>() < 0 )
        {
            return false;
        }
    }

    return aCell["size"]["xNm"].get<int64_t>() > 0
           && aCell["size"]["yNm"].get<int64_t>() > 0;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_BOARD_TABLE_PROTO_GENERATOR::Render(
        const JSON& aTable, const std::string& aProject, JSON& aItem )
{
    if( !aTable.is_object() || aProject.empty()
        || !aTable.contains( "id" ) || !aTable["id"].is_string()
        || aTable["id"].get_ref<const std::string&>().empty()
        || !aTable.contains( "position" ) || !aTable["position"].is_object()
        || !aTable["position"].contains( "xNm" )
        || !aTable["position"]["xNm"].is_number_integer()
        || !aTable["position"].contains( "yNm" )
        || !aTable["position"]["yNm"].is_number_integer()
        || !aTable.contains( "rotationDegrees" )
        || !aTable["rotationDegrees"].is_number_integer()
        || ( aTable["rotationDegrees"] != 0 && aTable["rotationDegrees"] != 90 )
        || !aTable.contains( "layer" ) || !aTable["layer"].is_string()
        || !aTable.contains( "locked" ) || !aTable["locked"].is_boolean()
        || !aTable.contains( "columnWidthsNm" ) || !aTable["columnWidthsNm"].is_array()
        || aTable["columnWidthsNm"].empty() || aTable["columnWidthsNm"].size() > 256
        || !aTable.contains( "rowHeightsNm" ) || !aTable["rowHeightsNm"].is_array()
        || aTable["rowHeightsNm"].empty() || aTable["rowHeightsNm"].size() > 256
        || !aTable.contains( "border" ) || !aTable["border"].is_object()
        || !aTable.contains( "separators" ) || !aTable["separators"].is_object()
        || !aTable.contains( "cells" ) || !aTable["cells"].is_array()
        || !aTable.contains( "merges" ) || !aTable["merges"].is_array() )
    {
        return false;
    }

    const size_t columnCount = aTable["columnWidthsNm"].size();
    const size_t rowCount = aTable["rowHeightsNm"].size();

    if( aTable["cells"].size() != rowCount * columnCount )
        return false;

    std::vector<int64_t> columnOffsets( columnCount, 0 );
    std::vector<int64_t> rowOffsets( rowCount, 0 );
    JSON columnWidths = JSON::array();
    JSON rowHeights = JSON::array();

    for( size_t column = 0; column < columnCount; ++column )
    {
        const JSON& width = aTable["columnWidthsNm"][column];

        if( !width.is_number_integer() || width.get<int64_t>() <= 0 )
            return false;

        if( column > 0 )
            columnOffsets[column] = columnOffsets[column - 1]
                                    + aTable["columnWidthsNm"][column - 1].get<int64_t>();

        columnWidths.push_back( { { "valueNm", std::to_string( width.get<int64_t>() ) } } );
    }

    for( size_t row = 0; row < rowCount; ++row )
    {
        const JSON& height = aTable["rowHeightsNm"][row];

        if( !height.is_number_integer() || height.get<int64_t>() <= 0 )
            return false;

        if( row > 0 )
            rowOffsets[row] = rowOffsets[row - 1]
                              + aTable["rowHeightsNm"][row - 1].get<int64_t>();

        rowHeights.push_back( { { "valueNm", std::to_string( height.get<int64_t>() ) } } );
    }

    const JSON& border = aTable["border"];
    const JSON& separators = aTable["separators"];
    JSON borderStroke;
    JSON separatorsStroke;

    if( !border.contains( "external" ) || !border["external"].is_boolean()
        || !border.contains( "header" ) || !border["header"].is_boolean()
        || !border.contains( "stroke" ) || !renderStroke( border["stroke"], borderStroke )
        || !separators.contains( "rows" ) || !separators["rows"].is_boolean()
        || !separators.contains( "columns" ) || !separators["columns"].is_boolean()
        || !separators.contains( "stroke" )
        || !renderStroke( separators["stroke"], separatorsStroke ) )
    {
        return false;
    }

    std::vector<std::pair<uint32_t, uint32_t>> spans( rowCount * columnCount, { 1, 1 } );

    for( const JSON& merge : aTable["merges"] )
    {
        if( !merge.is_object() || !merge.contains( "firstRow" )
            || !merge["firstRow"].is_number_unsigned() || !merge.contains( "firstColumn" )
            || !merge["firstColumn"].is_number_unsigned() || !merge.contains( "lastRow" )
            || !merge["lastRow"].is_number_unsigned() || !merge.contains( "lastColumn" )
            || !merge["lastColumn"].is_number_unsigned() )
        {
            return false;
        }

        const size_t firstRow = merge["firstRow"].get<size_t>();
        const size_t firstColumn = merge["firstColumn"].get<size_t>();
        const size_t lastRow = merge["lastRow"].get<size_t>();
        const size_t lastColumn = merge["lastColumn"].get<size_t>();

        if( firstRow > lastRow || firstColumn > lastColumn
            || lastRow >= rowCount || lastColumn >= columnCount )
        {
            return false;
        }

        spans[firstRow * columnCount + firstColumn] = {
            static_cast<uint32_t>( lastColumn - firstColumn + 1 ),
            static_cast<uint32_t>( lastRow - firstRow + 1 )
        };

        for( size_t row = firstRow; row <= lastRow; ++row )
        {
            for( size_t column = firstColumn; column <= lastColumn; ++column )
            {
                if( row != firstRow || column != firstColumn )
                    spans[row * columnCount + column] = { 0, 0 };
            }
        }
    }

    static const std::map<std::string, std::string> horizontalEnums = {
        { "left", "HA_LEFT" }, { "center", "HA_CENTER" }, { "right", "HA_RIGHT" }
    };
    static const std::map<std::string, std::string> verticalEnums = {
        { "top", "VA_TOP" }, { "center", "VA_CENTER" }, { "bottom", "VA_BOTTOM" }
    };
    JSON cells = JSON::array();
    const int rotation = aTable["rotationDegrees"].get<int>();
    const int64_t originX = aTable["position"]["xNm"].get<int64_t>();
    const int64_t originY = aTable["position"]["yNm"].get<int64_t>();
    const std::string tableId = aTable["id"].get<std::string>();

    for( size_t offset = 0; offset < aTable["cells"].size(); ++offset )
    {
        const JSON& cell = aTable["cells"][offset];

        if( !validCell( cell ) )
            return false;

        const size_t row = cell["row"].get<size_t>();
        const size_t column = cell["column"].get<size_t>();

        if( row >= rowCount || column >= columnCount || offset != row * columnCount + column )
            return false;

        const auto [ columnSpan, rowSpan ] = spans[offset];
        int64_t cellWidth = aTable["columnWidthsNm"][column].get<int64_t>();
        int64_t cellHeight = aTable["rowHeightsNm"][row].get<int64_t>();

        if( columnSpan > 1 )
        {
            cellWidth = 0;

            for( size_t index = 0; index < columnSpan; ++index )
                cellWidth += aTable["columnWidthsNm"][column + index].get<int64_t>();
        }

        if( rowSpan > 1 )
        {
            cellHeight = 0;

            for( size_t index = 0; index < rowSpan; ++index )
                cellHeight += aTable["rowHeightsNm"][row + index].get<int64_t>();
        }

        int64_t x = originX + columnOffsets[column];
        int64_t y = originY + rowOffsets[row];
        int64_t endX = x + cellWidth;
        int64_t endY = y + cellHeight;

        if( rotation == 90 )
        {
            x = originX + rowOffsets[row];
            y = originY - columnOffsets[column];
            endX = x + cellHeight;
            endY = y - cellWidth;
        }

        const JSON& margins = cell["margins"];
        const JSON& justify = cell["justify"];
        const std::string cellLogicalId = tableId + "/" + std::to_string( row + 1 )
                                          + "/" + std::to_string( column + 1 );
        cells.push_back( {
            { "id", { { "value", DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                                        aProject, "table_cell", cellLogicalId ) } } },
            { "textbox",
              { { "topLeft", vectorProto( x, y ) },
                { "bottomRight", vectorProto( endX, endY ) },
                { "attributes",
                  { { "fontName", cell["font"] },
                    { "horizontalAlignment",
                      horizontalEnums.at( justify["horizontal"].get<std::string>() ) },
                    { "verticalAlignment",
                      verticalEnums.at( justify["vertical"].get<std::string>() ) },
                    { "angle", { { "valueDegrees", rotation } } },
                    { "lineSpacing", cell["lineSpacing"] },
                    { "strokeWidth",
                      { { "valueNm", std::to_string( cell["thicknessNm"].get<int64_t>() ) } } },
                    { "italic", cell["italic"] }, { "bold", cell["bold"] },
                    { "underlined", false }, { "visible", true },
                    { "mirrored", cell["mirror"] }, { "multiline", true },
                    { "keepUpright", false },
                    { "size", vectorProto( cell["size"]["xNm"].get<int64_t>(),
                                             cell["size"]["yNm"].get<int64_t>() ) } } },
                { "text", cell["content"] }, { "hyperlink", cell["hyperlink"] } } },
            { "columnSpan", columnSpan }, { "rowSpan", rowSpan },
            { "margins",
              { { "left", { { "valueNm", std::to_string( margins["leftNm"].get<int64_t>() ) } } },
                { "top", { { "valueNm", std::to_string( margins["topNm"].get<int64_t>() ) } } },
                { "right", { { "valueNm", std::to_string( margins["rightNm"].get<int64_t>() ) } } },
                { "bottom", { { "valueNm", std::to_string( margins["bottomNm"].get<int64_t>() ) } } } } },
            { "locked", cell["locked"].get<bool>() ? "LS_LOCKED" : "LS_UNLOCKED" }
        } );
    }

    aItem = {
        { "id", { { "value", DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                                    aProject, "table", tableId ) } } },
        { "layer", layerEnum( aTable["layer"].get<std::string>() ) },
        { "locked", aTable["locked"].get<bool>() ? "LS_LOCKED" : "LS_UNLOCKED" },
        { "columnCount", columnCount }, { "columnWidths", std::move( columnWidths ) },
        { "rowHeights", std::move( rowHeights ) },
        { "strokeExternal", border["external"] },
        { "strokeHeaderSeparator", border["header"] },
        { "borderStroke", std::move( borderStroke ) },
        { "strokeRows", separators["rows"] },
        { "strokeColumns", separators["columns"] },
        { "separatorsStroke", std::move( separatorsStroke ) },
        { "cells", std::move( cells ) }
    };
    return true;
}

} // namespace KICHAD
