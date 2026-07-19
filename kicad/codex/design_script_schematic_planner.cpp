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

#include "design_script_schematic_planner.h"

#include "design_script_pcb_planner.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>


namespace
{

using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT;

constexpr int SCHEMATIC_FILE_VERSION = 20260306;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


std::string quoted( const std::string& aText )
{
    std::string result = "\"";

    for( char character : aText )
    {
        switch( character )
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result.push_back( character ); break;
        }
    }

    result.push_back( '"' );
    return result;
}


std::string millimetres( int64_t aNanometres )
{
    const bool negative = aNanometres < 0;
    uint64_t magnitude = negative ? static_cast<uint64_t>( -( aNanometres + 1 ) ) + 1
                                  : static_cast<uint64_t>( aNanometres );
    const uint64_t whole = magnitude / 1'000'000;
    uint64_t fraction = magnitude % 1'000'000;
    std::ostringstream output;

    if( negative )
        output << '-';

    output << whole;

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction + 1'000'000 ).substr( 1 );

        while( !digits.empty() && digits.back() == '0' )
            digits.pop_back();

        output << '.' << digits;
    }

    return output.str();
}


std::string stableUuid( const std::string& aProject, const std::string& aKind,
                        const std::string& aLogicalId )
{
    return KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, aKind, aLogicalId );
}


std::string effects( const std::string& aJustify )
{
    std::string result =
            "    (effects\n"
            "      (font (size 1.27 1.27))";

    if( !aJustify.empty() )
        result += "\n      (justify " + aJustify + ")";

    return result + "\n    )\n";
}


int sideAngle( const std::string& aSide )
{
    if( aSide == "left" )
        return 180;

    if( aSide == "top" )
        return 90;

    if( aSide == "bottom" )
        return 270;

    return 0;
}


std::string sheetPin( const JSON& aPin, const std::string& aUuid )
{
    std::ostringstream output;
    output << "    (pin " << quoted( aPin.at( "name" ).get<std::string>() ) << ' '
           << aPin.at( "direction" ).get<std::string>() << "\n"
           << "      (at " << millimetres( aPin.at( "position" ).at( "xNm" ).get<int64_t>() )
           << ' ' << millimetres( aPin.at( "position" ).at( "yNm" ).get<int64_t>() )
           << ' ' << sideAngle( aPin.at( "side" ).get<std::string>() ) << ")\n"
           << "      (uuid " << quoted( aUuid ) << ")\n"
           << effects( "left" )
           << "    )\n";
    return output.str();
}


std::string sheetExpression( const JSON& aSheet, const std::string& aProject,
                             const std::string& aParentPath, int aPage )
{
    const std::string id = aSheet.at( "id" ).get<std::string>();
    const int64_t x = aSheet.at( "position" ).at( "xNm" ).get<int64_t>();
    const int64_t y = aSheet.at( "position" ).at( "yNm" ).get<int64_t>();
    const int64_t width = aSheet.at( "size" ).at( "xNm" ).get<int64_t>();
    const int64_t height = aSheet.at( "size" ).at( "yNm" ).get<int64_t>();
    const std::string uuid = stableUuid( aProject, "schematic_sheet", id );
    std::ostringstream output;
    output << "(sheet\n"
           << "    (at " << millimetres( x ) << ' ' << millimetres( y ) << ")\n"
           << "    (size " << millimetres( width ) << ' ' << millimetres( height ) << ")\n"
           << "    (exclude_from_sim no)\n"
           << "    (in_bom yes)\n"
           << "    (on_board yes)\n"
           << "    (dnp no)\n"
           << "    (stroke (width 0.1524) (type solid))\n"
           << "    (fill (color 0 0 0 0))\n"
           << "    (uuid " << quoted( uuid ) << ")\n"
           << "    (property \"Sheetname\" "
           << quoted( aSheet.at( "title" ).get<std::string>() ) << "\n"
           << "      (at " << millimetres( x ) << ' ' << millimetres( y - 711'600 )
           << " 0)\n"
           << "      (show_name no)\n"
           << "      (do_not_autoplace no)\n"
           << effects( "left bottom" )
           << "    )\n"
           << "    (property \"Sheetfile\" "
           << quoted( aSheet.at( "file" ).get<std::string>() ) << "\n"
           << "      (at " << millimetres( x ) << ' '
           << millimetres( y + height + 1'384'600 ) << " 0)\n"
           << "      (show_name no)\n"
           << "      (do_not_autoplace no)\n"
           << effects( "left top" )
           << "    )\n";

    std::vector<JSON> pins = aSheet.at( "pins" ).get<std::vector<JSON>>();
    std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
               {
                   return aLeft.at( "name" ).get<std::string>()
                          < aRight.at( "name" ).get<std::string>();
               } );

    for( const JSON& pin : pins )
    {
        output << sheetPin( pin, stableUuid( aProject, "schematic_sheet_pin",
                                             id + "/" + pin.at( "name" ).get<std::string>() ) );
    }

    output << "    (instances\n"
           << "      (project " << quoted( aProject ) << "\n"
           << "        (path " << quoted( aParentPath ) << " (page "
           << quoted( std::to_string( aPage ) ) << "))\n"
           << "      )\n"
           << "    )\n"
           << "  )";
    return output.str();
}


std::string hierarchicalLabel( const JSON& aSheet, const JSON& aPin,
                               const std::string& aProject, size_t aIndex )
{
    const std::string id = aSheet.at( "id" ).get<std::string>();
    const int64_t y = 20'000'000 + static_cast<int64_t>( aIndex ) * 2'540'000;
    std::ostringstream output;
    output << "(hierarchical_label " << quoted( aPin.at( "name" ).get<std::string>() )
           << "\n"
           << "    (shape " << aPin.at( "direction" ).get<std::string>() << ")\n"
           << "    (at 20 " << millimetres( y ) << " 180)\n"
           << effects( "right" )
           << "    (uuid "
           << quoted( stableUuid( aProject, "schematic_hier_label",
                                  id + "/" + aPin.at( "name" ).get<std::string>() ) )
           << ")\n"
           << "  )";
    return output.str();
}


std::pair<int64_t, int64_t> transformPoint( int64_t aX, int64_t aY, int aRotation,
                                            const std::string& aMirror )
{
    // Library coordinates use an upward Y axis; schematic coordinates use a downward Y axis.
    aY = -aY;
    int x1 = 1;
    int y1 = 0;
    int x2 = 0;
    int y2 = 1;

    if( aRotation == 90 )
    {
        x1 = 0;
        y1 = 1;
        x2 = -1;
        y2 = 0;
    }
    else if( aRotation == 180 )
    {
        x1 = -1;
        y1 = 0;
        x2 = 0;
        y2 = -1;
    }
    else if( aRotation == 270 )
    {
        x1 = 0;
        y1 = -1;
        x2 = 1;
        y2 = 0;
    }

    const auto compose = [&]( int aTempX1, int aTempY1, int aTempX2, int aTempY2 )
    {
        const int newX1 = x1 * aTempX1 + x2 * aTempY1;
        const int newY1 = y1 * aTempX1 + y2 * aTempY1;
        const int newX2 = x1 * aTempX2 + x2 * aTempY2;
        const int newY2 = y1 * aTempX2 + y2 * aTempY2;
        x1 = newX1;
        y1 = newY1;
        x2 = newX2;
        y2 = newY2;
    };

    if( aMirror.find( 'x' ) != std::string::npos )
        compose( 1, 0, 0, -1 );

    if( aMirror.find( 'y' ) != std::string::npos )
        compose( -1, 0, 0, 1 );

    return { x1 * aX + y1 * aY, x2 * aX + y2 * aY };
}


std::string propertyExpression( const std::string& aName, const std::string& aValue,
                                int64_t aX, int64_t aY, int aAngle, bool aHidden,
                                const std::string& aJustify )
{
    std::ostringstream output;
    output << "    (property " << quoted( aName ) << ' ' << quoted( aValue ) << "\n"
           << "      (at " << millimetres( aX ) << ' ' << millimetres( aY ) << ' '
           << aAngle << ")\n"
           << "      (show_name no)\n"
           << "      (do_not_autoplace no)\n";

    if( aHidden )
        output << "      (hide yes)\n";

    output << effects( aJustify ) << "    )\n";
    return output.str();
}


std::string componentExpression( const JSON& aComponent, const JSON& aUnit,
                                 const JSON& aResolved, const std::string& aProject,
                                 const std::string& aInstancePath )
{
    const std::string reference = aComponent.at( "reference" ).get<std::string>();
    const std::string libraryId = aComponent.at( "symbol" ).get<std::string>();
    const int unit = aUnit.at( "number" ).get<int>();
    const int rotation = aUnit.at( "rotationDegrees" ).get<int>();
    const std::string mirror = aUnit.at( "mirror" ).get<std::string>();
    const int nativeRotation = mirror == "xy" ? ( rotation + 180 ) % 360 : rotation;
    const std::string nativeMirror = mirror == "xy" ? "none" : mirror;
    const int64_t x = aUnit.at( "position" ).at( "xNm" ).get<int64_t>();
    const int64_t y = aUnit.at( "position" ).at( "yNm" ).get<int64_t>();
    const std::string logicalId = reference + "/" + std::to_string( unit );
    const std::string uuid = stableUuid( aProject, "schematic_symbol", logicalId );
    const std::string unitKey = std::to_string( unit );
    const JSON& pins = aResolved.at( "units" ).at( unitKey );
    const JSON& nativeProperties = aResolved.at( "properties" );
    std::ostringstream output;
    output << "(symbol\n"
           << "    (lib_id " << quoted( libraryId ) << ")\n"
           << "    (at " << millimetres( x ) << ' ' << millimetres( y ) << ' '
           << nativeRotation << ")\n";

    if( nativeMirror != "none" )
    {
        output << "    (mirror";

        if( nativeMirror == "x" )
            output << " x";

        if( nativeMirror == "y" )
            output << " y";

        output << ")\n";
    }

    output << "    (unit " << unit << ")\n"
           << "    (body_style 1)\n"
           << "    (exclude_from_sim no)\n"
           << "    (in_bom yes)\n"
           << "    (on_board yes)\n"
           << "    (in_pos_files yes)\n"
           << "    (dnp " << ( aComponent.value( "dnp", false ) ? "yes" : "no" ) << ")\n"
           << "    (fields_autoplaced yes)\n"
           << "    (uuid " << quoted( uuid ) << ")\n"
           << propertyExpression( "Reference", reference, x + 2'540'000, y - 1'270'000,
                                  nativeRotation, false, "left" )
           << propertyExpression( "Value", aComponent.at( "value" ).get<std::string>(),
                                  x + 2'540'000, y + 1'270'000, nativeRotation, false, "left" )
           << propertyExpression( "Footprint",
                                  aComponent.at( "footprint" ).get<std::string>(),
                                  x - 1'778'000, y, ( nativeRotation + 90 ) % 360, true, "" )
           << propertyExpression( "Datasheet", nativeProperties.value( "Datasheet", "" ),
                                  x, y, nativeRotation, true, "" )
           << propertyExpression( "Description", nativeProperties.value( "Description", "" ),
                                  x, y, nativeRotation, true, "" );

    std::vector<std::pair<std::string, std::string>> customProperties;

    for( auto property = aComponent.at( "properties" ).begin();
         property != aComponent.at( "properties" ).end(); ++property )
    {
        customProperties.emplace_back( property.key(), property.value().get<std::string>() );
    }

    std::sort( customProperties.begin(), customProperties.end() );

    for( const auto& [ name, value ] : customProperties )
        output << propertyExpression( name, value, x, y, nativeRotation, true, "" );

    for( size_t pinIndex = 0; pinIndex < pins.size(); ++pinIndex )
    {
        const std::string number = pins[pinIndex].at( "number" ).get<std::string>();
        output << "    (pin " << quoted( number ) << "\n"
               << "      (uuid "
               << quoted( stableUuid( aProject, "schematic_symbol_pin",
                                      logicalId + "/" + number + "/"
                                              + std::to_string( pinIndex ) ) )
               << ")\n"
               << "    )\n";
    }

    output << "    (instances\n"
           << "      (project " << quoted( aProject ) << "\n"
           << "        (path " << quoted( aInstancePath ) << "\n"
           << "          (reference " << quoted( reference ) << ")\n"
           << "          (unit " << unit << ")\n"
           << "        )\n"
           << "      )\n"
           << "    )\n"
           << "  )";
    return output.str();
}


std::string globalLabelExpression( const std::string& aName, int64_t aX, int64_t aY,
                                   const std::string& aUuid )
{
    std::ostringstream output;
    output << "(global_label " << quoted( aName ) << "\n"
           << "    (shape passive)\n"
           << "    (at " << millimetres( aX ) << ' ' << millimetres( aY ) << " 0)\n"
           << "    (fields_autoplaced yes)\n"
           << effects( "left" )
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string noConnectExpression( int64_t aX, int64_t aY, const std::string& aUuid )
{
    std::ostringstream output;
    output << "(no_connect\n"
           << "    (at " << millimetres( aX ) << ' ' << millimetres( aY ) << ")\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string strokeExpression( const JSON& aStroke )
{
    std::ostringstream output;
    output << "    (stroke\n"
           << "      (width "
           << millimetres( aStroke.at( "widthNm" ).get<int64_t>() ) << ")\n"
           << "      (type " << aStroke.at( "lineStyle" ).get<std::string>() << ")\n"
           << "    )\n";
    return output.str();
}


std::string schematicLineExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& from = aDrawing.at( "from" );
    const JSON& to = aDrawing.at( "to" );
    const std::string kind = aDrawing.at( "kind" ).get<std::string>();
    std::ostringstream output;

    if( kind == "bus_entry" )
    {
        const int64_t dx = to.at( "xNm" ).get<int64_t>()
                           - from.at( "xNm" ).get<int64_t>();
        const int64_t dy = to.at( "yNm" ).get<int64_t>()
                           - from.at( "yNm" ).get<int64_t>();
        output << "(bus_entry\n"
               << "    (at " << millimetres( from.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( from.at( "yNm" ).get<int64_t>() ) << ")\n"
               << "    (size " << millimetres( dx ) << ' ' << millimetres( dy ) << ")\n";
    }
    else
    {
        output << '(' << kind << "\n"
               << "    (pts\n"
               << "      (xy " << millimetres( from.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( from.at( "yNm" ).get<int64_t>() ) << ")\n"
               << "      (xy " << millimetres( to.at( "xNm" ).get<int64_t>() ) << ' '
               << millimetres( to.at( "yNm" ).get<int64_t>() ) << ")\n"
               << "    )\n";
    }

    output << strokeExpression( aDrawing.at( "stroke" ) )
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


std::string alphaChannel( int aAlpha )
{
    if( aAlpha == 0 )
        return "0";

    if( aAlpha == 255 )
        return "1";

    std::ostringstream output;
    output << std::fixed << std::setprecision( 8 )
           << static_cast<double>( aAlpha ) / 255.0;
    std::string result = output.str();

    while( !result.empty() && result.back() == '0' )
        result.pop_back();

    return result;
}


std::string junctionExpression( const JSON& aDrawing, const std::string& aUuid )
{
    const JSON& position = aDrawing.at( "position" );
    int red = 0;
    int green = 0;
    int blue = 0;
    int alpha = 0;

    if( !aDrawing.at( "color" ).is_null() )
    {
        red = aDrawing.at( "color" ).at( "r" ).get<int>();
        green = aDrawing.at( "color" ).at( "g" ).get<int>();
        blue = aDrawing.at( "color" ).at( "b" ).get<int>();
        alpha = aDrawing.at( "color" ).at( "a" ).get<int>();
    }

    std::ostringstream output;
    output << "(junction\n"
           << "    (at " << millimetres( position.at( "xNm" ).get<int64_t>() ) << ' '
           << millimetres( position.at( "yNm" ).get<int64_t>() ) << ")\n"
           << "    (diameter " << millimetres( aDrawing.at( "diameterNm" ).get<int64_t>() )
           << ")\n"
           << "    (color " << red << ' ' << green << ' ' << blue << ' '
           << alphaChannel( alpha ) << ")\n"
           << "    (uuid " << quoted( aUuid ) << ")\n"
           << "  )";
    return output.str();
}


bool validSheetShape( const JSON& aSheet )
{
    if( !aSheet.is_object() || !aSheet.contains( "id" ) || !aSheet["id"].is_string()
        || !aSheet.contains( "parent" )
        || !( aSheet["parent"].is_null() || aSheet["parent"].is_string() )
        || !aSheet.contains( "file" ) || !aSheet["file"].is_string()
        || !aSheet.contains( "title" ) || !aSheet["title"].is_string()
        || !aSheet.contains( "pins" ) || !aSheet["pins"].is_array() )
    {
        return false;
    }

    if( !aSheet["parent"].is_null()
        && ( !aSheet.contains( "position" ) || !aSheet["position"].is_object()
             || !aSheet["position"].contains( "xNm" )
             || !aSheet["position"]["xNm"].is_number_integer()
             || !aSheet["position"].contains( "yNm" )
             || !aSheet["position"]["yNm"].is_number_integer()
             || !aSheet.contains( "size" ) || !aSheet["size"].is_object()
             || !aSheet["size"].contains( "xNm" )
             || !aSheet["size"]["xNm"].is_number_integer()
             || !aSheet["size"].contains( "yNm" )
             || !aSheet["size"]["yNm"].is_number_integer() ) )
    {
        return false;
    }

    for( const JSON& pin : aSheet["pins"] )
    {
        if( !pin.is_object() || !pin.contains( "name" ) || !pin["name"].is_string()
            || !pin.contains( "direction" ) || !pin["direction"].is_string()
            || !pin.contains( "side" ) || !pin["side"].is_string()
            || !pin.contains( "position" ) || !pin["position"].is_object()
            || !pin["position"].contains( "xNm" )
            || !pin["position"]["xNm"].is_number_integer()
            || !pin["position"].contains( "yNm" )
            || !pin["position"]["yNm"].is_number_integer() )
        {
            return false;
        }
    }

    return true;
}


bool validDrawingShape( const JSON& aDrawing )
{
    if( !aDrawing.is_object() || !aDrawing.contains( "kind" )
        || !aDrawing["kind"].is_string() || !aDrawing.contains( "id" )
        || !aDrawing["id"].is_string() || aDrawing["id"].get<std::string>().empty()
        || !aDrawing.contains( "sheet" ) || !aDrawing["sheet"].is_string() )
    {
        return false;
    }

    const std::string kind = aDrawing["kind"].get<std::string>();

    if( kind == "junction" )
    {
        if( !aDrawing.contains( "position" ) || !aDrawing["position"].is_object()
            || !aDrawing["position"].contains( "xNm" )
            || !aDrawing["position"]["xNm"].is_number_integer()
            || !aDrawing["position"].contains( "yNm" )
            || !aDrawing["position"]["yNm"].is_number_integer()
            || !aDrawing.contains( "diameterNm" )
            || !aDrawing["diameterNm"].is_number_integer()
            || !aDrawing.contains( "color" )
            || !( aDrawing["color"].is_null() || aDrawing["color"].is_object() ) )
        {
            return false;
        }

        if( aDrawing["color"].is_object() )
        {
            for( const char* channel : { "r", "g", "b", "a" } )
            {
                if( !aDrawing["color"].contains( channel )
                    || !aDrawing["color"][channel].is_number_integer() )
                {
                    return false;
                }
            }
        }

        return true;
    }

    if( kind != "wire" && kind != "bus" && kind != "bus_entry" )
        return false;

    for( const char* point : { "from", "to" } )
    {
        if( !aDrawing.contains( point ) || !aDrawing[point].is_object()
            || !aDrawing[point].contains( "xNm" )
            || !aDrawing[point]["xNm"].is_number_integer()
            || !aDrawing[point].contains( "yNm" )
            || !aDrawing[point]["yNm"].is_number_integer() )
        {
            return false;
        }
    }

    return aDrawing.contains( "stroke" ) && aDrawing["stroke"].is_object()
           && aDrawing["stroke"].contains( "widthNm" )
           && aDrawing["stroke"]["widthNm"].is_number_integer()
           && aDrawing["stroke"].contains( "lineStyle" )
           && aDrawing["stroke"]["lineStyle"].is_string();
}


bool validComponentShape( const JSON& aComponent )
{
    if( !aComponent.is_object() || !aComponent.contains( "reference" )
        || !aComponent["reference"].is_string() || !aComponent.contains( "symbol" )
        || !aComponent["symbol"].is_string() || !aComponent.contains( "value" )
        || !aComponent["value"].is_string() || !aComponent.contains( "footprint" )
        || !aComponent["footprint"].is_string() || !aComponent.contains( "properties" )
        || !aComponent["properties"].is_object() || !aComponent.contains( "units" )
        || !aComponent["units"].is_array() || aComponent["units"].empty() )
    {
        return false;
    }

    for( const JSON& unit : aComponent["units"] )
    {
        if( !unit.is_object() || !unit.contains( "number" )
            || !unit["number"].is_number_integer() || !unit.contains( "sheet" )
            || !unit["sheet"].is_string() || !unit.contains( "position" )
            || !unit["position"].is_object() || !unit["position"].contains( "xNm" )
            || !unit["position"]["xNm"].is_number_integer()
            || !unit["position"].contains( "yNm" )
            || !unit["position"]["yNm"].is_number_integer()
            || !unit.contains( "rotationDegrees" )
            || !unit["rotationDegrees"].is_number_integer() || !unit.contains( "mirror" )
            || !unit["mirror"].is_string() )
        {
            return false;
        }
    }

    return true;
}


bool validUuid( const std::string& aUuid )
{
    if( aUuid.size() != 36 )
        return false;

    for( size_t index = 0; index < aUuid.size(); ++index )
    {
        if( index == 8 || index == 13 || index == 18 || index == 23 )
        {
            if( aUuid[index] != '-' )
                return false;
        }
        else if( !std::isxdigit( static_cast<unsigned char>( aUuid[index] ) ) )
        {
            return false;
        }
    }

    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT
DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( const JSON& aCompilerIr,
                                      const JSON& aExistingScreenUuids,
                                      const JSON& aResolvedSymbols )
{
    RESULT result;
    result.counts = { { "files", 0 }, { "sheets", 0 }, { "pins", 0 },
                      { "components", 0 }, { "netEndpoints", 0 },
                      { "noConnects", 0 }, { "drawings", 0 }, { "librarySymbols", 0 },
                      { "managedItems", 0 } };

    if( !aCompilerIr.is_object() || aCompilerIr.value( "language", "" ) != "kichad-design"
        || aCompilerIr.value( "version", 0 ) != 1 || !aCompilerIr.contains( "project" )
        || !aCompilerIr["project"].is_object()
        || !aCompilerIr["project"].contains( "name" )
        || !aCompilerIr["project"]["name"].is_string()
        || !aCompilerIr.contains( "schematic" ) || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "sheets" )
        || !aCompilerIr["schematic"]["sheets"].is_array()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array()
        || !aCompilerIr["schematic"].contains( "nets" )
        || !aCompilerIr["schematic"]["nets"].is_array()
        || !aCompilerIr["schematic"].contains( "noConnects" )
        || !aCompilerIr["schematic"]["noConnects"].is_array()
        || !aCompilerIr["schematic"].contains( "drawings" )
        || !aCompilerIr["schematic"]["drawings"].is_array()
        || !aExistingScreenUuids.is_object() || !aResolvedSymbols.is_object() )
    {
        diagnostic( result, "invalid_compiler_ir",
                    "schematic planning requires valid KiChad Design Script version 1 IR" );
        return result;
    }

    const std::string project = aCompilerIr["project"]["name"].get<std::string>();
    const JSON& sourceSheets = aCompilerIr["schematic"]["sheets"];
    const JSON& sourceComponents = aCompilerIr["schematic"]["components"];
    const JSON& sourceNets = aCompilerIr["schematic"]["nets"];
    const JSON& sourceNoConnects = aCompilerIr["schematic"]["noConnects"];
    const JSON& sourceDrawings = aCompilerIr["schematic"]["drawings"];

    if( sourceSheets.empty() )
    {
        if( !sourceDrawings.empty() )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic drawings require a declared hierarchy" );
            return result;
        }

        result.fullyLowered = true;
        return result;
    }

    std::map<std::string, JSON> sheets;
    std::map<std::string, std::string> fileOwners;
    std::string rootId;

    for( const JSON& sheet : sourceSheets )
    {
        if( !validSheetShape( sheet ) )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic IR contains a malformed sheet or pin" );
            continue;
        }

        const std::string id = sheet["id"].get<std::string>();
        const std::string file = sheet["file"].get<std::string>();

        if( id.empty() || !sheets.emplace( id, sheet ).second )
            diagnostic( result, "invalid_schematic_ir", "schematic sheet IDs must be unique" );

        auto [fileEntry, fileInserted] = fileOwners.emplace( file, id );

        if( !fileInserted )
        {
            diagnostic( result, "shared_sheet_file_not_supported",
                        "sheet file " + file + " is referenced by both " + fileEntry->second
                                + " and " + id
                                + "; shared-screen instance lowering is not yet supported" );
        }

        if( sheet["parent"].is_null() )
        {
            if( !rootId.empty() )
                diagnostic( result, "invalid_schematic_ir", "schematic IR has multiple roots" );

            rootId = id;
        }
    }

    if( rootId.empty() )
        diagnostic( result, "invalid_schematic_ir", "schematic IR has no root sheet" );

    std::map<std::string, std::vector<std::string>> children;

    for( const auto& [ id, sheet ] : sheets )
    {
        if( sheet["parent"].is_null() )
            continue;

        const std::string parent = sheet["parent"].get<std::string>();

        if( !sheets.contains( parent ) )
            diagnostic( result, "invalid_schematic_ir", "sheet " + id + " has no parent " + parent );
        else
            children[parent].push_back( id );
    }

    for( auto& [ parent, childIds ] : children )
        std::sort( childIds.begin(), childIds.end() );

    std::set<std::string> visiting;
    std::set<std::string> visited;
    std::vector<std::string> orderedIds;
    std::function<void( const std::string& )> visit = [&]( const std::string& aId )
    {
        if( visiting.contains( aId ) )
        {
            diagnostic( result, "invalid_schematic_ir", "schematic IR contains a parent cycle" );
            return;
        }

        if( visited.contains( aId ) )
            return;

        visiting.insert( aId );
        visited.insert( aId );
        orderedIds.push_back( aId );

        for( const std::string& child : children[aId] )
            visit( child );

        visiting.erase( aId );
    };

    if( !rootId.empty() )
        visit( rootId );

    if( visited.size() != sheets.size() )
        diagnostic( result, "invalid_schematic_ir", "schematic IR contains unreachable sheets" );

    if( !result.diagnostics.empty() )
        return result;

    std::map<std::string, int> pages;

    for( size_t index = 0; index < orderedIds.size(); ++index )
        pages[orderedIds[index]] = static_cast<int>( index + 1 );

    std::map<std::string, std::string> screenUuids;

    for( const auto& [ id, sheet ] : sheets )
    {
        const std::string file = sheet["file"].get<std::string>();
        std::string screenUuid = stableUuid( project, "schematic_file", file );

        if( aExistingScreenUuids.contains( file ) )
        {
            if( !aExistingScreenUuids[file].is_string()
                || !validUuid( aExistingScreenUuids[file].get<std::string>() ) )
            {
                diagnostic( result, "invalid_screen_identity",
                            "existing screen UUID for " + file + " is invalid" );
                return result;
            }

            screenUuid = aExistingScreenUuids[file].get<std::string>();
        }

        screenUuids.emplace( file, std::move( screenUuid ) );
    }

    const std::string rootScreenUuid =
            screenUuids.at( sheets.at( rootId )["file"].get<std::string>() );
    std::map<std::string, std::string> instancePaths;

    for( const std::string& id : orderedIds )
    {
        std::vector<std::string> ancestors;
        std::string cursor = id;

        while( cursor != rootId )
        {
            ancestors.push_back( cursor );
            cursor = sheets.at( cursor )["parent"].get<std::string>();
        }

        std::reverse( ancestors.begin(), ancestors.end() );
        std::string path = "/" + rootScreenUuid;

        for( const std::string& ancestor : ancestors )
            path += "/" + stableUuid( project, "schematic_sheet", ancestor );

        instancePaths[id] = std::move( path );
    }

    struct PIN_POINT
    {
        std::string sheet;
        int64_t     x;
        int64_t     y;
    };

    std::map<std::string, std::vector<JSON>> placementsBySheet;
    std::map<std::string, std::map<std::string, JSON>> librariesBySheet;
    std::map<std::string, std::vector<PIN_POINT>> endpointPositions;
    std::set<std::string> componentReferences;

    try
    {
        for( const JSON& component : sourceComponents )
        {
            if( !validComponentShape( component ) )
            {
                diagnostic( result, "invalid_schematic_ir",
                            "schematic IR contains a malformed component placement" );
                continue;
            }

            const std::string reference = component["reference"].get<std::string>();
            const std::string libraryId = component["symbol"].get<std::string>();

            if( !componentReferences.emplace( reference ).second )
            {
                diagnostic( result, "invalid_schematic_ir",
                            "schematic component references must be unique" );
                continue;
            }

            if( !aResolvedSymbols.contains( libraryId )
                || !aResolvedSymbols[libraryId].is_object()
                || !aResolvedSymbols[libraryId].contains( "cacheSource" )
                || !aResolvedSymbols[libraryId]["cacheSource"].is_string()
                || !aResolvedSymbols[libraryId].contains( "properties" )
                || !aResolvedSymbols[libraryId]["properties"].is_object()
                || !aResolvedSymbols[libraryId].contains( "units" )
                || !aResolvedSymbols[libraryId]["units"].is_object() )
            {
                diagnostic( result, "unresolved_schematic_symbol",
                            "component " + reference + " symbol " + libraryId
                                    + " has no exact resolved project-library source" );
                continue;
            }

            const JSON& resolved = aResolvedSymbols[libraryId];

            for( const JSON& unit : component["units"] )
            {
                const int unitNumber = unit["number"].get<int>();
                const std::string unitKey = std::to_string( unitNumber );
                const std::string sheetId = unit["sheet"].get<std::string>();

                if( !sheets.contains( sheetId ) || !resolved["units"].contains( unitKey )
                    || !resolved["units"][unitKey].is_array() )
                {
                    diagnostic( result, "unresolved_schematic_symbol_unit",
                                "component " + reference + " unit " + unitKey
                                        + " has no resolved sheet or symbol unit" );
                    continue;
                }

                placementsBySheet[sheetId].push_back(
                        { { "component", component }, { "unit", unit },
                          { "resolved", resolved } } );
                librariesBySheet[sheetId][libraryId] = resolved;
                const int64_t originX = unit["position"]["xNm"].get<int64_t>();
                const int64_t originY = unit["position"]["yNm"].get<int64_t>();
                const int rotation = unit["rotationDegrees"].get<int>();
                const std::string mirror = unit["mirror"].get<std::string>();

                for( const JSON& pin : resolved["units"][unitKey] )
                {
                    if( !pin.is_object() || !pin.contains( "number" )
                        || !pin["number"].is_string() || !pin.contains( "xNm" )
                        || !pin["xNm"].is_number_integer() || !pin.contains( "yNm" )
                        || !pin["yNm"].is_number_integer() )
                    {
                        diagnostic( result, "invalid_resolved_symbol_pin",
                                    "resolved symbol " + libraryId + " has malformed pin metadata" );
                        continue;
                    }

                    const auto [ pinX, pinY ] = transformPoint(
                            pin["xNm"].get<int64_t>(), pin["yNm"].get<int64_t>(),
                            rotation, mirror );
                    const std::string endpoint = reference + "/" + unitKey + "/"
                                                 + pin["number"].get<std::string>();
                    endpointPositions[endpoint].push_back(
                            { sheetId, originX + pinX, originY + pinY } );
                }
            }
        }
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "invalid_schematic_ir", error.what() );
    }

    std::map<std::string, std::vector<JSON>> connectivityBySheet;
    std::map<std::string, std::vector<JSON>> drawingsBySheet;

    for( const JSON& drawing : sourceDrawings )
    {
        if( !validDrawingShape( drawing )
            || !sheets.contains( drawing.value( "sheet", "" ) ) )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic IR contains a malformed drawing or sheet reference" );
            continue;
        }

        const std::string kind = drawing["kind"].get<std::string>();
        const std::string id = drawing["id"].get<std::string>();
        const std::string uuid = stableUuid( project, "schematic_" + kind, id );
        JSON planned = { { "kind", kind },
                         { "logicalId", id },
                         { "uuid", uuid },
                         { "source", kind == "junction"
                                               ? junctionExpression( drawing, uuid )
                                               : schematicLineExpression( drawing, uuid ) } };
        drawingsBySheet[drawing["sheet"].get<std::string>()].push_back(
                std::move( planned ) );
    }

    const auto addEndpoint = [&]( const JSON& aEndpoint, const std::string& aNetName,
                                  bool aNoConnect )
    {
        if( !aEndpoint.is_object() || !aEndpoint.contains( "component" )
            || !aEndpoint["component"].is_string() || !aEndpoint.contains( "unit" )
            || !aEndpoint["unit"].is_number_integer() || !aEndpoint.contains( "number" )
            || !aEndpoint["number"].is_string() )
        {
            diagnostic( result, "invalid_schematic_ir", "schematic endpoint IR is malformed" );
            return;
        }

        const std::string reference = aEndpoint["component"].get<std::string>();
        const std::string unit = std::to_string( aEndpoint["unit"].get<int>() );
        const std::string number = aEndpoint["number"].get<std::string>();
        const std::string endpoint = reference + "/" + unit + "/" + number;
        auto positions = endpointPositions.find( endpoint );

        if( positions == endpointPositions.end() || positions->second.empty() )
        {
            diagnostic( result, "unresolved_schematic_pin",
                        "schematic endpoint " + reference + ":" + unit + ":" + number
                                + " does not resolve to a native symbol pin" );
            return;
        }

        for( size_t index = 0; index < positions->second.size(); ++index )
        {
            const PIN_POINT& point = positions->second[index];
            const std::string prefix = aNoConnect ? "no_connect" : "net";
            const std::string logicalId = prefix + "/" + aNetName + "/" + endpoint + "/"
                                          + std::to_string( index );
            const std::string kind = aNoConnect ? "no_connect" : "global_label";
            const std::string uuid = stableUuid( project, "schematic_" + kind, logicalId );
            const std::string source = aNoConnect
                                               ? noConnectExpression( point.x, point.y, uuid )
                                               : globalLabelExpression( aNetName, point.x, point.y,
                                                                        uuid );
            connectivityBySheet[point.sheet].push_back(
                    { { "kind", kind }, { "logicalId", logicalId }, { "uuid", uuid },
                      { "source", source } } );
        }
    };

    try
    {
        for( const JSON& net : sourceNets )
        {
            if( !net.is_object() || !net.contains( "name" ) || !net["name"].is_string()
                || !net.contains( "pins" ) || !net["pins"].is_array() )
            {
                diagnostic( result, "invalid_schematic_ir", "schematic net IR is malformed" );
                continue;
            }

            const std::string name = net["name"].get<std::string>();

            for( const JSON& endpoint : net["pins"] )
                addEndpoint( endpoint, name, false );
        }

        for( const JSON& endpoint : sourceNoConnects )
            addEndpoint( endpoint, "", true );
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "invalid_schematic_ir", error.what() );
    }

    if( !result.diagnostics.empty() )
        return result;

    JSON files = JSON::array();
    JSON managedItems = JSON::array();

    try
    {
        for( const std::string& id : orderedIds )
        {
            const JSON& sheet = sheets.at( id );
            const std::string file = sheet["file"].get<std::string>();
            const std::string screenUuid = screenUuids.at( file );
            JSON items = JSON::array();
            JSON libSymbols = JSON::array();

            for( const auto& [ libraryId, resolved ] : librariesBySheet[id] )
            {
                const std::string ownershipUuid =
                        stableUuid( project, "schematic_lib_symbol", id + "/" + libraryId );
                libSymbols.push_back( { { "kind", "lib_symbol" },
                                        { "file", file },
                                        { "logicalId", id + "/" + libraryId },
                                        { "libraryId", libraryId },
                                        { "uuid", ownershipUuid },
                                        { "source", resolved["cacheSource"] } } );
                managedItems.push_back( { { "file", file },
                                          { "kind", "lib_symbol" },
                                          { "logicalId", id + "/" + libraryId },
                                          { "libraryId", libraryId },
                                          { "uuid", ownershipUuid } } );
            }

            for( const JSON& placement : placementsBySheet[id] )
            {
                const JSON& component = placement["component"];
                const JSON& unit = placement["unit"];
                const std::string reference = component["reference"].get<std::string>();
                const std::string logicalId = reference + "/"
                                              + std::to_string( unit["number"].get<int>() );
                const std::string uuid = stableUuid( project, "schematic_symbol", logicalId );
                const std::string source = componentExpression(
                        component, unit, placement["resolved"], project, instancePaths.at( id ) );
                items.push_back( { { "kind", "symbol" },
                                   { "file", file },
                                   { "logicalId", logicalId },
                                   { "uuid", uuid },
                                   { "source", source } } );
                managedItems.push_back( { { "file", file },
                                          { "kind", "symbol" },
                                          { "logicalId", logicalId },
                                          { "uuid", uuid } } );
            }

            for( JSON connectivity : connectivityBySheet[id] )
            {
                connectivity["file"] = file;
                items.push_back( connectivity );
                JSON ownership = connectivity;
                ownership.erase( "source" );
                managedItems.push_back( std::move( ownership ) );
            }

            for( JSON drawing : drawingsBySheet[id] )
            {
                drawing["file"] = file;
                items.push_back( drawing );
                JSON ownership = std::move( drawing );
                ownership.erase( "source" );
                managedItems.push_back( std::move( ownership ) );
            }

            if( id != rootId )
            {
                std::vector<JSON> pins = sheet["pins"].get<std::vector<JSON>>();
                std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
                           {
                               return aLeft.at( "name" ).get<std::string>()
                                      < aRight.at( "name" ).get<std::string>();
                           } );

                for( size_t pinIndex = 0; pinIndex < pins.size(); ++pinIndex )
                {
                    const JSON& pin = pins[pinIndex];
                    const std::string logicalId = id + "/" + pin["name"].get<std::string>();
                    const std::string uuid =
                            stableUuid( project, "schematic_hier_label", logicalId );
                    const std::string source =
                            hierarchicalLabel( sheet, pin, project, pinIndex );
                    items.push_back( { { "kind", "hierarchical_label" },
                                       { "file", file },
                                       { "logicalId", logicalId },
                                       { "uuid", uuid },
                                       { "source", source } } );
                    managedItems.push_back( { { "file", file },
                                              { "kind", "hierarchical_label" },
                                              { "logicalId", logicalId },
                                              { "uuid", uuid } } );
                }
            }

            for( const std::string& childId : children[id] )
            {
                const JSON& child = sheets.at( childId );
                const std::string uuid = stableUuid( project, "schematic_sheet", childId );
                const std::string source =
                        sheetExpression( child, project, instancePaths.at( id ),
                                         pages.at( childId ) );
                items.push_back( { { "kind", "sheet" },
                                   { "file", file },
                                   { "logicalId", childId },
                                   { "uuid", uuid },
                                   { "source", source } } );
                managedItems.push_back( { { "file", file },
                                          { "kind", "sheet" },
                                          { "logicalId", childId },
                                          { "uuid", uuid } } );
            }

            std::ostringstream document;
            document << "(kicad_sch\n"
                     << "  (version " << SCHEMATIC_FILE_VERSION << ")\n"
                     << "  (generator \"eeschema\")\n"
                     << "  (generator_version \"10.0\")\n"
                     << "  (uuid " << quoted( screenUuid ) << ")\n"
                     << "  (paper \"A4\")\n";

            if( id == rootId )
            {
                document << "  (title_block\n"
                         << "    (title " << quoted( sheet["title"].get<std::string>() ) << ")\n"
                         << "  )\n";
            }

            document << "  (lib_symbols\n";

            for( const JSON& libSymbol : libSymbols )
                document << "    " << libSymbol["source"].get<std::string>() << "\n";

            document << "  )\n";

            for( const JSON& item : items )
                document << "  " << item["source"].get<std::string>() << "\n";

            if( id == rootId )
            {
                document << "  (sheet_instances\n"
                         << "    (path \"/\" (page \"1\"))\n"
                         << "  )\n"
                         << "  (embedded_fonts no)\n";
            }

            document << ")\n";
            std::string parseError;

            if( !LOSSLESS_SEXPR_DOCUMENT::Parse( document.str(), &parseError ) )
            {
                diagnostic( result, "invalid_generated_schematic",
                            "generated " + file + " is not a valid s-expression: " + parseError );
                return result;
            }

            files.push_back( { { "path", file },
                               { "sheetId", id },
                               { "screenUuid", screenUuid },
                               { "page", pages.at( id ) },
                               { "root", id == rootId },
                               { "title", sheet["title"] },
                               { "rootTitleSource",
                                 id == rootId
                                         ? JSON( "(title "
                                                 + quoted( sheet["title"].get<std::string>() )
                                                 + ")" )
                                         : JSON( nullptr ) },
                               { "rootInstancesSource",
                                 id == rootId
                                         ? JSON( "(sheet_instances\n"
                                                 "    (path \"/\" (page \"1\"))\n"
                                                 "  )" )
                                         : JSON( nullptr ) },
                               { "libSymbols", std::move( libSymbols ) },
                               { "items", std::move( items ) },
                               { "newDocumentSource", document.str() } } );
            result.counts["pins"] = result.counts["pins"].get<size_t>()
                                    + sheet["pins"].size();
        }
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "invalid_schematic_ir", error.what() );
        return result;
    }

    result.operations.push_back( { { "action", "reconcile_schematic_hierarchy" },
                                   { "project", project },
                                   { "rootFile", sheets.at( rootId )["file"] },
                                   { "files", std::move( files ) },
                                   { "managedItems", std::move( managedItems ) } } );
    result.counts["files"] = orderedIds.size();
    result.counts["sheets"] = orderedIds.size();
    result.counts["components"] = sourceComponents.size();
    result.counts["netEndpoints"] = 0;

    for( const JSON& net : sourceNets )
        result.counts["netEndpoints"] = result.counts["netEndpoints"].get<size_t>()
                                        + net.value( "pins", JSON::array() ).size();

    result.counts["noConnects"] = sourceNoConnects.size();
    result.counts["drawings"] = sourceDrawings.size();

    for( const auto& [ sheet, symbols ] : librariesBySheet )
        result.counts["librarySymbols"] = result.counts["librarySymbols"].get<size_t>()
                                          + symbols.size();

    result.counts["managedItems"] = result.operations[0]["managedItems"].size();
    result.fullyLowered = true;
    return result;
}

} // namespace KICHAD
