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

#include "design_script_board_table_compiler.h"
#include "kichad_from_chars.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_BOARD_TABLE_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr int64_t MIN_AXIS_SIZE_NM = 100'000LL;
constexpr int64_t MAX_TEXT_SIZE_NM = 50'000'000LL;
constexpr size_t MAX_AXIS_COUNT = 256;
constexpr size_t MAX_CELL_COUNT = 65'536;
constexpr size_t MAX_TEXT_BYTES = 64 * 1024;
constexpr size_t MAX_HYPERLINK_BYTES = 2048;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode }, { "message", aMessage } } );
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


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool identifier( const std::string& aValue )
{
    return !aValue.empty() && aValue.size() <= 128
           && std::all_of( aValue.begin(), aValue.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isalnum( aCharacter ) || aCharacter == '_'
                                      || aCharacter == '-' || aCharacter == '.'
                                      || aCharacter == '+';
                           } );
}


bool boolean( const std::string& aText, bool& aValue )
{
    if( aText == "true" )
    {
        aValue = true;
        return true;
    }

    if( aText == "false" )
    {
        aValue = false;
        return true;
    }

    return false;
}


bool distance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result parsed = KICHAD::FromChars( begin, end, value );

    if( parsed.ec != std::errc() || parsed.ptr == begin || !std::isfinite( value ) )
        return false;

    const std::string_view unit( parsed.ptr, static_cast<size_t>( end - parsed.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm" )
        scale = 1'000'000.0L;
    else if( unit == "mil" )
        scale = 25'400.0L;
    else if( unit == "um" )
        scale = 1'000.0L;
    else if( unit == "nm" )
        scale = 1.0L;
    else if( unit == "in" )
        scale = 25'400'000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( rounded < -MAX_COORDINATE_NM || rounded > MAX_COORDINATE_NM )
        return false;

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool decimal( const std::string& aText, double& aValue )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result parsed = KICHAD::FromChars( begin, end, aValue );
    return parsed.ec == std::errc() && parsed.ptr == end && std::isfinite( aValue );
}


bool positiveInteger( const std::string& aText, size_t aMaximum, size_t& aValue )
{
    size_t parsed = 0;
    const std::from_chars_result result =
            std::from_chars( aText.data(), aText.data() + aText.size(), parsed );

    if( result.ec != std::errc() || result.ptr != aText.data() + aText.size()
        || parsed == 0 || parsed > aMaximum )
    {
        return false;
    }

    aValue = parsed;
    return true;
}


bool userLayer( const std::string& aLayer )
{
    if( !aLayer.starts_with( "User." ) )
        return false;

    int index = 0;
    const std::string_view number( aLayer.data() + 5, aLayer.size() - 5 );
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 45;
}


bool innerCopperLayer( const std::string& aLayer )
{
    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return false;

    int index = 0;
    const std::string_view number( aLayer.data() + 2, aLayer.size() - 5 );
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 30;
}


bool boardLayer( const std::string& aLayer )
{
    static const std::set<std::string> fixed = {
        "F.Cu", "B.Cu", "F.Adhes", "B.Adhes", "F.Paste", "B.Paste",
        "F.SilkS", "B.SilkS", "F.Mask", "B.Mask", "Dwgs.User", "Cmts.User",
        "Eco1.User", "Eco2.User", "Edge.Cuts", "Margin", "F.CrtYd", "B.CrtYd",
        "F.Fab", "B.Fab"
    };
    return fixed.contains( aLayer ) || innerCopperLayer( aLayer ) || userLayer( aLayer );
}


bool point( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.children.size() != 3 || !scalar( aDocument, node.children[1], xText )
        || !scalar( aDocument, node.children[2], yText )
        || !distance( xText, x ) || !distance( yText, y ) )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool color( const std::string& aText, JSON& aColor )
{
    if( aText == "default" )
    {
        aColor = nullptr;
        return true;
    }

    if( ( aText.size() != 7 && aText.size() != 9 ) || aText.front() != '#' )
        return false;

    const auto hex = []( char aCharacter ) -> int
    {
        if( aCharacter >= '0' && aCharacter <= '9' )
            return aCharacter - '0';
        if( aCharacter >= 'a' && aCharacter <= 'f' )
            return aCharacter - 'a' + 10;
        if( aCharacter >= 'A' && aCharacter <= 'F' )
            return aCharacter - 'A' + 10;
        return -1;
    };
    int channels[4] = { 0, 0, 0, 255 };

    for( size_t index = 0; index < ( aText.size() - 1 ) / 2; ++index )
    {
        const int high = hex( aText[index * 2 + 1] );
        const int low = hex( aText[index * 2 + 2] );

        if( high < 0 || low < 0 )
            return false;

        channels[index] = high * 16 + low;
    }

    aColor = { { "r", channels[0] }, { "g", channels[1] },
               { "b", channels[2] }, { "a", channels[3] } };
    return true;
}


bool stroke( const DOCUMENT& aDocument, size_t aNode, JSON& aStroke )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string widthText;
    std::string style;
    std::string colorText;
    int64_t width = -1;
    JSON parsedColor;
    static const std::set<std::string> styles = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };

    if( node.children.size() != 4 || !scalar( aDocument, node.children[1], widthText )
        || ( widthText != "default"
             && ( !distance( widthText, width ) || width < 10'000 || width > 10'000'000 ) )
        || !scalar( aDocument, node.children[2], style ) || !styles.contains( style )
        || !scalar( aDocument, node.children[3], colorText )
        || !color( colorText, parsedColor ) )
    {
        return false;
    }

    aStroke = { { "widthNm", width }, { "style", style },
                { "color", std::move( parsedColor ) } };
    return true;
}


bool tableLines( const DOCUMENT& aDocument, size_t aNode, bool aBorder,
                 JSON& aLines, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::set<std::string> booleans = aBorder
            ? std::set<std::string>{ "external", "header" }
            : std::set<std::string>{ "rows", "columns" };
    std::set<std::string> fields;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_board_table_line_field",
                        "table line field " + head + " occurs more than once" );
            continue;
        }

        if( booleans.contains( head ) )
        {
            std::string value;
            bool parsed = false;

            if( !oneValue( aDocument, child, value ) || !boolean( value, parsed ) )
                diagnostic( aResult, "invalid_authored_board_table_line_boolean",
                            "table line booleans require true or false" );
            else
                aLines[head] = parsed;
        }
        else if( head == "stroke" )
        {
            JSON parsed;

            if( !stroke( aDocument, child, parsed ) )
                diagnostic( aResult, "invalid_authored_board_table_line_stroke",
                            "table stroke requires WIDTH STYLE default|#RRGGBB[AA]" );
            else
                aLines["stroke"] = std::move( parsed );
        }
        else
        {
            diagnostic( aResult, "unknown_authored_board_table_line_field",
                        "table border supports external/header/stroke; separators supports rows/columns/stroke" );
        }
    }

    for( const std::string& field : booleans )
    {
        if( !aLines.contains( field ) )
            diagnostic( aResult, "missing_authored_board_table_line_field",
                        "table line policy is missing " + field );
    }

    if( !aLines.contains( "stroke" ) )
        diagnostic( aResult, "missing_authored_board_table_line_field",
                    "table line policy is missing stroke" );

    return aLines.contains( "stroke" )
           && std::all_of( booleans.begin(), booleans.end(),
                           [&]( const std::string& aField ) { return aLines.contains( aField ); } );
}


JSON tableCell( const DOCUMENT& aDocument, size_t aNode, size_t aRows, size_t aColumns,
                RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string rowText;
    std::string columnText;
    std::string content;
    size_t row = 0;
    size_t column = 0;

    if( node.children.size() < 4 || !scalar( aDocument, node.children[1], rowText )
        || !scalar( aDocument, node.children[2], columnText )
        || !scalar( aDocument, node.children[3], content )
        || !positiveInteger( rowText, aRows, row )
        || !positiveInteger( columnText, aColumns, column )
        || content.size() > MAX_TEXT_BYTES || content.find( '\0' ) != std::string::npos )
    {
        diagnostic( aResult, "invalid_authored_board_table_cell",
                    "cell requires an in-bounds 1-based row/column and bounded content" );
        return JSON::object();
    }

    JSON cell = { { "row", row - 1 }, { "column", column - 1 }, { "content", content } };
    std::set<std::string> fields;

    for( size_t index = 4; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_board_table_cell_field",
                        "cell field " + head + " occurs more than once" );
            continue;
        }

        if( head == "margins" )
        {
            JSON margins = JSON::object();
            static const char* names[] = { "leftNm", "topNm", "rightNm", "bottomNm" };
            bool valid = field.children.size() == 5;

            for( size_t margin = 0; margin < 4 && valid; ++margin )
            {
                std::string valueText;
                int64_t value = 0;
                valid = scalar( aDocument, field.children[margin + 1], valueText )
                        && distance( valueText, value ) && value >= 0;
                margins[names[margin]] = value;
            }

            if( !valid )
                diagnostic( aResult, "invalid_authored_board_table_cell_margins",
                            "cell margins require four non-negative distances" );
            else
                cell["margins"] = std::move( margins );

            continue;
        }

        if( head == "text_size" )
        {
            JSON size;

            if( !point( aDocument, child, size )
                || size["xNm"].get<int64_t>() < MIN_AXIS_SIZE_NM
                || size["yNm"].get<int64_t>() < MIN_AXIS_SIZE_NM
                || size["xNm"].get<int64_t>() > MAX_TEXT_SIZE_NM
                || size["yNm"].get<int64_t>() > MAX_TEXT_SIZE_NM )
            {
                diagnostic( aResult, "invalid_authored_board_table_cell_text_size",
                            "cell text_size requires width and height from 0.1mm through 50mm" );
            }
            else
                cell["size"] = std::move( size );

            continue;
        }

        std::string value;

        if( head == "justify" )
        {
            std::string horizontal;
            std::string vertical;
            static const std::set<std::string> horizontalValues = { "left", "center", "right" };
            static const std::set<std::string> verticalValues = { "top", "center", "bottom" };

            if( field.children.size() != 3
                || !scalar( aDocument, field.children[1], horizontal )
                || !scalar( aDocument, field.children[2], vertical )
                || !horizontalValues.contains( horizontal )
                || !verticalValues.contains( vertical ) )
            {
                diagnostic( aResult, "invalid_authored_board_table_cell_justify",
                            "cell justify requires horizontal and vertical alignment" );
            }
            else
                cell["justify"] = { { "horizontal", horizontal }, { "vertical", vertical } };

            continue;
        }

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_board_table_cell_field",
                        "cell field " + head + " requires one value" );
            continue;
        }

        if( head == "font" )
        {
            if( value.empty() || value.size() > 256 || value.find_first_of( "\0\r\n" ) != std::string::npos )
                diagnostic( aResult, "invalid_authored_board_table_cell_font",
                            "cell font requires stroke or one bounded local font name" );
            else
                cell["font"] = value == "stroke" ? "" : value;
        }
        else if( head == "line_spacing" )
        {
            double parsed = 0.0;

            if( !decimal( value, parsed ) || parsed < 0.1 || parsed > 10.0 )
                diagnostic( aResult, "invalid_authored_board_table_cell_line_spacing",
                            "cell line_spacing requires 0.1 through 10" );
            else
                cell["lineSpacing"] = parsed;
        }
        else if( head == "thickness" )
        {
            int64_t parsed = 0;

            if( value == "auto" )
                cell["thicknessNm"] = 0;
            else if( !distance( value, parsed ) || parsed < 0 || parsed > 10'000'000 )
                diagnostic( aResult, "invalid_authored_board_table_cell_thickness",
                            "cell thickness requires auto or 0 through 10mm" );
            else
                cell["thicknessNm"] = parsed;
        }
        else if( head == "hyperlink" )
        {
            if( value.size() > MAX_HYPERLINK_BYTES
                || value.find_first_of( "\0\r\n" ) != std::string::npos )
                diagnostic( aResult, "invalid_authored_board_table_cell_hyperlink",
                            "cell hyperlink requires none or one bounded line" );
            else
                cell["hyperlink"] = value == "none" ? "" : value;
        }
        else if( head == "mirror" || head == "bold" || head == "italic" || head == "locked" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( aResult, "invalid_authored_board_table_cell_boolean",
                            "cell style and lock fields require true or false" );
            else
                cell[head] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_board_table_cell_field",
                        "cell supports margins, text_size, font, line_spacing, thickness, justify, mirror, bold, italic, hyperlink, and locked" );
        }
    }

    for( const char* required : { "margins", "size", "font", "lineSpacing", "thicknessNm",
                                  "justify", "mirror", "bold", "italic", "hyperlink", "locked" } )
    {
        if( !cell.contains( required ) )
            diagnostic( aResult, "missing_authored_board_table_cell_field",
                        "cell " + rowText + "," + columnText + " is missing " + required );
    }

    return cell;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_BOARD_TABLE_COMPILER::RESULT
DESIGN_SCRIPT_BOARD_TABLE_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( aDocument.ListHead( aNode ) != "table" || node.children.size() < 2
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_board_table_header",
                    "table requires one bounded stable ID" );
        return result;
    }

    JSON table = { { "id", id }, { "locked", false } };
    std::set<std::string> fields;
    size_t cellsNode = DOCUMENT::NO_NODE;
    size_t mergesNode = DOCUMENT::NO_NODE;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_board_table_field",
                        "table field " + head + " occurs more than once" );
            continue;
        }

        if( head == "at" )
        {
            JSON position;

            if( !point( aDocument, child, position ) )
                diagnostic( result, "invalid_authored_board_table_position",
                            "table at requires two bounded coordinates" );
            else
                table["position"] = std::move( position );
        }
        else if( head == "columns" || head == "rows" )
        {
            JSON dimensions = JSON::array();
            int64_t total = 0;
            bool valid = field.children.size() >= 2
                         && field.children.size() <= MAX_AXIS_COUNT + 1;

            for( size_t dimensionIndex = 1; dimensionIndex < field.children.size(); ++dimensionIndex )
            {
                std::string valueText;
                int64_t value = 0;
                valid = valid && scalar( aDocument, field.children[dimensionIndex], valueText )
                        && distance( valueText, value ) && value >= MIN_AXIS_SIZE_NM;
                total += value;
                dimensions.push_back( value );
            }

            if( !valid || total > MAX_COORDINATE_NM )
                diagnostic( result, "invalid_authored_board_table_" + head,
                            "table axes require 1 through 256 positive bounded dimensions with a total no greater than 2m" );
            else
                table[head == "columns" ? "columnWidthsNm" : "rowHeightsNm"] =
                        std::move( dimensions );
        }
        else if( head == "border" || head == "separators" )
        {
            JSON lines = JSON::object();

            if( tableLines( aDocument, child, head == "border", lines, result ) )
                table[head] = std::move( lines );
        }
        else if( head == "cells" )
            cellsNode = child;
        else if( head == "merges" )
            mergesNode = child;
        else
        {
            std::string value;

            if( !oneValue( aDocument, child, value ) )
                diagnostic( result, "invalid_authored_board_table_field",
                            "table field " + head + " requires one value" );
            else if( head == "rotation" )
            {
                if( value != "0deg" && value != "90deg" )
                    diagnostic( result, "invalid_authored_board_table_rotation",
                                "table rotation requires 0deg or 90deg" );
                else
                    table["rotationDegrees"] = value == "90deg" ? 90 : 0;
            }
            else if( head == "layer" )
            {
                if( !boardLayer( value ) )
                    diagnostic( result, "invalid_authored_board_table_layer",
                                "table layer requires a supported KiCad board layer" );
                else
                    table["layer"] = value;
            }
            else if( head == "locked" )
            {
                bool parsed = false;

                if( !boolean( value, parsed ) )
                    diagnostic( result, "invalid_authored_board_table_locked",
                                "table locked requires true or false" );
                else
                    table["locked"] = parsed;
            }
            else
                diagnostic( result, "unknown_authored_board_table_field",
                            "table supports at, rotation, layer, locked, columns, rows, border, separators, cells, and merges" );
        }
    }

    for( const char* required : { "position", "rotationDegrees", "layer", "columnWidthsNm",
                                  "rowHeightsNm", "border", "separators" } )
    {
        if( !table.contains( required ) )
            diagnostic( result, "missing_authored_board_table_field",
                        "table " + id + " is missing " + required );
    }

    if( cellsNode == DOCUMENT::NO_NODE || mergesNode == DOCUMENT::NO_NODE
        || !table.contains( "columnWidthsNm" ) || !table.contains( "rowHeightsNm" ) )
    {
        if( cellsNode == DOCUMENT::NO_NODE )
            diagnostic( result, "missing_authored_board_table_field", "table is missing cells" );
        if( mergesNode == DOCUMENT::NO_NODE )
            diagnostic( result, "missing_authored_board_table_field", "table is missing merges" );
        result.statement = { { "kind", "board_table" }, { "logicalId", id },
                             { "table", std::move( table ) }, { "typed", true } };
        return result;
    }

    const size_t columns = table["columnWidthsNm"].size();
    const size_t rows = table["rowHeightsNm"].size();

    if( rows > MAX_CELL_COUNT / columns )
    {
        diagnostic( result, "invalid_authored_board_table_cell_count",
                    "table may contain no more than 65536 cells" );
        return result;
    }

    std::map<std::pair<size_t, size_t>, JSON> cells;
    const DOCUMENT::NODE& cellList = aDocument.Nodes().at( cellsNode );

    for( size_t index = 1; index < cellList.children.size(); ++index )
    {
        const size_t child = cellList.children[index];

        if( aDocument.ListHead( child ) != "cell" )
        {
            diagnostic( result, "invalid_authored_board_table_cells",
                        "table cells may contain only cell forms" );
            continue;
        }

        JSON compiled = tableCell( aDocument, child, rows, columns, result );

        if( !compiled.contains( "row" ) || !compiled.contains( "column" ) )
            continue;

        const std::pair<size_t, size_t> key = {
            compiled["row"].get<size_t>(), compiled["column"].get<size_t>()
        };

        if( !cells.emplace( key, std::move( compiled ) ).second )
            diagnostic( result, "duplicate_authored_board_table_cell",
                        "each table cell address must occur exactly once" );
    }

    if( cells.size() != rows * columns )
        diagnostic( result, "incomplete_authored_board_table_cells",
                    "table must define every cell exactly once" );

    table["cells"] = JSON::array();

    for( size_t row = 0; row < rows; ++row )
    {
        for( size_t column = 0; column < columns; ++column )
        {
            auto found = cells.find( { row, column } );

            if( found != cells.end() )
                table["cells"].push_back( std::move( found->second ) );
        }
    }

    table["merges"] = JSON::array();
    std::vector<bool> occupied( rows * columns, false );
    std::vector<bool> covered( rows * columns, false );
    const DOCUMENT::NODE& merges = aDocument.Nodes().at( mergesNode );

    for( size_t index = 1; index < merges.children.size(); ++index )
    {
        const size_t child = merges.children[index];
        const DOCUMENT::NODE& merge = aDocument.Nodes().at( child );
        std::string texts[4];
        size_t values[4] = {};
        bool valid = aDocument.ListHead( child ) == "merge" && merge.children.size() == 5;

        for( size_t value = 0; value < 4 && valid; ++value )
        {
            valid = scalar( aDocument, merge.children[value + 1], texts[value] )
                    && positiveInteger( texts[value], value % 2 == 0 ? rows : columns,
                                        values[value] );
        }

        valid = valid && values[0] <= values[2] && values[1] <= values[3]
                && ( values[0] != values[2] || values[1] != values[3] );

        if( !valid )
        {
            diagnostic( result, "invalid_authored_board_table_merge",
                        "merge requires a nontrivial in-bounds row/column rectangle" );
            continue;
        }

        bool overlaps = false;

        for( size_t row = values[0] - 1; row < values[2]; ++row )
        {
            for( size_t column = values[1] - 1; column < values[3]; ++column )
                overlaps = overlaps || occupied[row * columns + column];
        }

        if( overlaps )
        {
            diagnostic( result, "overlapping_authored_board_table_merge",
                        "table merge rectangles cannot overlap" );
            continue;
        }

        for( size_t row = values[0] - 1; row < values[2]; ++row )
        {
            for( size_t column = values[1] - 1; column < values[3]; ++column )
            {
                occupied[row * columns + column] = true;
                covered[row * columns + column] = row != values[0] - 1
                                                   || column != values[1] - 1;
            }
        }

        table["merges"].push_back( {
            { "firstRow", values[0] - 1 }, { "firstColumn", values[1] - 1 },
            { "lastRow", values[2] - 1 }, { "lastColumn", values[3] - 1 }
        } );
    }

    if( table["cells"].size() == rows * columns )
    {
        for( size_t offset = 0; offset < covered.size(); ++offset )
        {
            if( covered[offset] && !table["cells"][offset]["content"].get<std::string>().empty() )
                diagnostic( result, "covered_authored_board_table_cell_content",
                            "cells covered by a merge must have empty content" );
        }
    }

    const int64_t x = table.value( "position", JSON::object() ).value( "xNm", int64_t( 0 ) );
    const int64_t y = table.value( "position", JSON::object() ).value( "yNm", int64_t( 0 ) );
    int64_t width = 0;
    int64_t height = 0;

    for( const JSON& value : table["columnWidthsNm"] )
        width += value.get<int64_t>();
    for( const JSON& value : table["rowHeightsNm"] )
        height += value.get<int64_t>();

    const bool extentValid = table.value( "rotationDegrees", 0 ) == 0
            ? x + width <= MAX_COORDINATE_NM && y + height <= MAX_COORDINATE_NM
            : x + height <= MAX_COORDINATE_NM && y - width >= -MAX_COORDINATE_NM;

    if( !extentValid )
        diagnostic( result, "invalid_authored_board_table_extent",
                    "table rotated extent exceeds the native board coordinate range" );

    result.statement = { { "kind", "board_table" }, { "logicalId", id },
                         { "table", std::move( table ) }, { "typed", true } };
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
