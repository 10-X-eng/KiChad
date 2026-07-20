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

#include "design_script_footprint_settings_generator.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
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


std::string fixedDecimal( int64_t aValue, uint64_t aScale )
{
    const bool negative = aValue < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aValue + 1 ) ) + 1
                                       : static_cast<uint64_t>( aValue );
    std::string result = negative ? "-" : "";
    result += std::to_string( magnitude / aScale );
    const uint64_t remainder = magnitude % aScale;

    if( remainder != 0 )
    {
        std::string fraction = std::to_string( remainder );
        const size_t digits = std::to_string( aScale ).size() - 1;
        result += "." + std::string( digits - fraction.size(), '0' ) + fraction;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


int layerRank( const std::string& aLayer )
{
    static const std::map<std::string, int> fixed = {
        { "F.Cu", 0 }, { "F.Mask", 1 }, { "B.Cu", 2 }, { "B.Mask", 3 },
        { "F.SilkS", 5 }, { "B.SilkS", 7 }, { "F.Adhes", 9 }, { "B.Adhes", 11 },
        { "F.Paste", 13 }, { "B.Paste", 15 }, { "Dwgs.User", 17 },
        { "Cmts.User", 19 }, { "Eco1.User", 21 }, { "Eco2.User", 23 },
        { "Edge.Cuts", 25 }, { "Margin", 27 }, { "B.CrtYd", 29 },
        { "F.CrtYd", 31 }, { "B.Fab", 33 }, { "F.Fab", 35 }
    };

    if( fixed.contains( aLayer ) )
        return fixed.at( aLayer );

    const bool inner = aLayer.starts_with( "In" ) && aLayer.ends_with( ".Cu" );
    const bool user = aLayer.starts_with( "User." );
    const size_t offset = inner ? 2 : user ? 5 : aLayer.size();
    const size_t length = inner ? aLayer.size() - 5 : aLayer.size() - offset;
    const std::string_view number( aLayer.data() + offset, length );
    int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );

    if( parsed.ec != std::errc() || parsed.ptr != number.data() + number.size() )
        return 1000;

    return inner ? 4 + ( index - 1 ) * 2 : 39 + ( index - 1 ) * 2;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_SETTINGS_GENERATOR::RenderRules(
        const JSON& aRules, std::string& aSource )
{
    if( !aRules.is_object() || !aRules.contains( "clearanceNm" )
        || !aRules.contains( "solderMaskMarginNm" )
        || !aRules.contains( "solderPasteMarginNm" )
        || !aRules.contains( "solderPasteMarginPpm" )
        || !aRules.contains( "zoneConnection" ) || !aRules["zoneConnection"].is_string() )
    {
        return false;
    }

    const std::pair<const char*, const char*> distances[] = {
        { "solderMaskMarginNm", "solder_mask_margin" },
        { "solderPasteMarginNm", "solder_paste_margin" },
        { "clearanceNm", "clearance" }
    };

    for( const auto& [field, token] : distances )
    {
        if( aRules[field].is_null() )
            continue;

        if( !aRules[field].is_number_integer() )
            return false;

        aSource += "\t(" + std::string( token ) + " "
                   + fixedDecimal( aRules[field].get<int64_t>(), 1'000'000 ) + ")\n";
    }

    if( !aRules["solderPasteMarginPpm"].is_null() )
    {
        if( !aRules["solderPasteMarginPpm"].is_number_integer() )
            return false;

        aSource += "\t(solder_paste_margin_ratio "
                   + fixedDecimal( aRules["solderPasteMarginPpm"].get<int64_t>(), 1'000'000 )
                   + ")\n";
    }

    const std::string connection = aRules["zoneConnection"].get<std::string>();
    static const std::map<std::string, int> connectionValues = {
        { "none", 0 }, { "thermal", 1 }, { "solid", 2 }, { "pth_thermal", 3 }
    };

    if( connection != "inherit" )
    {
        if( !connectionValues.contains( connection ) )
            return false;

        aSource += "\t(zone_connect " + std::to_string( connectionValues.at( connection ) ) + ")\n";
    }

    return true;
}


bool DESIGN_SCRIPT_FOOTPRINT_SETTINGS_GENERATOR::RenderStackup(
        const JSON& aStackup, std::string& aSource )
{
    if( aStackup.is_null() )
        return true;

    if( !aStackup.is_array() || aStackup.size() < 2 )
        return false;

    for( const JSON& layer : aStackup )
    {
        if( !layer.is_string() )
            return false;
    }

    aSource += "\t(stackup (layer " + quoteText( aStackup.front().get<std::string>() ) + ")"
               " (layer " + quoteText( aStackup.back().get<std::string>() ) + ")";

    for( size_t index = 1; index + 1 < aStackup.size(); ++index )
        aSource += " (layer " + quoteText( aStackup[index].get<std::string>() ) + ")";

    aSource += ")\n";
    return true;
}


bool DESIGN_SCRIPT_FOOTPRINT_SETTINGS_GENERATOR::RenderPrivateLayers(
        const JSON& aLayers, std::string& aSource )
{
    if( !aLayers.is_array() )
        return false;

    if( aLayers.empty() )
        return true;

    std::vector<std::string> layers;

    for( const JSON& layer : aLayers )
    {
        if( !layer.is_string() )
            return false;

        layers.push_back( layer.get<std::string>() );
    }

    std::sort( layers.begin(), layers.end(), []( const std::string& aLeft,
                                                 const std::string& aRight )
    {
        return layerRank( aLeft ) < layerRank( aRight );
    } );
    aSource += "\t(private_layers";

    for( const std::string& layer : layers )
        aSource += " " + quoteText( layer );

    aSource += ")\n";
    return true;
}

} // namespace KICHAD
