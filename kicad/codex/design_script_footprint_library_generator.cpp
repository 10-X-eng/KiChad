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

#include "design_script_footprint_library_generator.h"

#include "design_script_footprint_graphic_generator.h"
#include "design_script_footprint_pad_generator.h"
#include "design_script_pcb_planner.h"
#include "design_script_footprint_text_generator.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT;
using GRAPHIC_GENERATOR = KICHAD::DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_GENERATOR;
using PAD_GENERATOR = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PAD_GENERATOR;
using TEXT_GENERATOR = KICHAD::DESIGN_SCRIPT_FOOTPRINT_TEXT_GENERATOR;

constexpr size_t MAX_NATIVE_FOOTPRINT_BYTES = 16 * 1024 * 1024;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


std::string quoteText( const std::string& aText )
{
    std::string result = "\"";
    result.reserve( aText.size() + 2 );

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


std::string decimalPpm( int64_t aPartsPerMillion )
{
    const bool negative = aPartsPerMillion < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aPartsPerMillion + 1 ) ) + 1
                                       : static_cast<uint64_t>( aPartsPerMillion );
    std::string fraction = std::to_string( magnitude % 1'000'000 );
    std::string result = ( negative ? "-" : "" )
                         + std::to_string( magnitude / 1'000'000 );

    if( magnitude % 1'000'000 != 0 )
    {
        result += "." + std::string( 6 - fraction.size(), '0' ) + fraction;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


std::string degrees( int64_t aTenths )
{
    const bool negative = aTenths < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aTenths + 1 ) ) + 1
                                       : static_cast<uint64_t>( aTenths );
    std::string result = negative ? "-" : "";
    result += std::to_string( magnitude / 10 );

    if( magnitude % 10 != 0 )
        result += "." + std::to_string( magnitude % 10 );

    return result;
}


std::string property( const std::string& aName, const std::string& aValue,
                      const std::string& aLayer, bool aHidden, int64_t aY,
                      const std::string& aUuid )
{
    std::string source = "\t(property " + quoteText( aName ) + " " + quoteText( aValue ) + "\n"
                         "\t\t(at 0 " + millimetres( aY ) + " 0)\n"
                         "\t\t(layer " + quoteText( aLayer ) + ")\n";

    if( aHidden )
        source += "\t\t(hide yes)\n";

    source += "\t\t(uuid " + quoteText( aUuid ) + ")\n"
              "\t\t(effects\n"
              "\t\t\t(font\n"
              "\t\t\t\t(size 1.27 1.27)\n"
              "\t\t\t)\n"
              "\t\t)\n"
              "\t)\n";
    return source;
}


bool renderModel( const JSON& aModel, std::string& aSource )
{
    const char* transforms[] = { "offsetNm", "scalePpm", "rotationTenths" };

    if( !aModel.is_object() || !aModel.contains( "path" ) || !aModel["path"].is_string()
        || !aModel.contains( "visible" ) || !aModel["visible"].is_boolean()
        || !aModel.contains( "opacityPpm" ) || !aModel["opacityPpm"].is_number_integer() )
    {
        return false;
    }

    for( const char* transform : transforms )
    {
        if( !aModel.contains( transform ) || !aModel[transform].is_object()
            || !aModel[transform].contains( "x" ) || !aModel[transform]["x"].is_number_integer()
            || !aModel[transform].contains( "y" ) || !aModel[transform]["y"].is_number_integer()
            || !aModel[transform].contains( "z" ) || !aModel[transform]["z"].is_number_integer() )
        {
            return false;
        }
    }

    aSource += "\t(model " + quoteText( aModel["path"].get<std::string>() ) + "\n";

    if( !aModel["visible"].get<bool>() )
        aSource += "\t\t(hide yes)\n";

    if( aModel["opacityPpm"].get<int64_t>() != 1'000'000 )
        aSource += "\t\t(opacity "
                   + decimalPpm( aModel["opacityPpm"].get<int64_t>() ) + ")\n";

    const JSON& offset = aModel["offsetNm"];
    const JSON& scale = aModel["scalePpm"];
    const JSON& rotation = aModel["rotationTenths"];
    aSource += "\t\t(offset (xyz " + millimetres( offset["x"].get<int64_t>() ) + " "
               + millimetres( offset["y"].get<int64_t>() ) + " "
               + millimetres( offset["z"].get<int64_t>() ) + "))\n"
               "\t\t(scale (xyz " + decimalPpm( scale["x"].get<int64_t>() ) + " "
               + decimalPpm( scale["y"].get<int64_t>() ) + " "
               + decimalPpm( scale["z"].get<int64_t>() ) + "))\n"
               "\t\t(rotate (xyz " + degrees( rotation["x"].get<int64_t>() ) + " "
               + degrees( rotation["y"].get<int64_t>() ) + " "
               + degrees( rotation["z"].get<int64_t>() ) + "))\n"
               "\t)\n";
    return true;
}


bool renderFootprint( const JSON& aFootprint, const std::string& aProject,
                      std::string& aSource, RESULT& aResult )
{
    static const std::vector<std::pair<const char*, JSON::value_t>> required = {
        { "id", JSON::value_t::string }, { "name", JSON::value_t::string },
        { "reference", JSON::value_t::string }, { "value", JSON::value_t::string },
        { "datasheet", JSON::value_t::string }, { "description", JSON::value_t::string },
        { "keywords", JSON::value_t::string }, { "attributes", JSON::value_t::object },
        { "duplicatePadNumbersAreJumpers", JSON::value_t::boolean },
        { "jumperGroups", JSON::value_t::array }, { "netTieGroups", JSON::value_t::array },
        { "pads", JSON::value_t::array }, { "graphics", JSON::value_t::array },
        { "texts", JSON::value_t::array }, { "textBoxes", JSON::value_t::array },
        { "models", JSON::value_t::array }
    };

    if( !aFootprint.is_object() )
        return false;

    for( const auto& [field, type] : required )
    {
        if( !aFootprint.contains( field ) || aFootprint[field].type() != type )
            return false;
    }

    const std::string id = aFootprint["id"].get<std::string>();
    const std::string name = aFootprint["name"].get<std::string>();
    aSource = "(footprint " + quoteText( name ) + "\n"
              "\t(version 20260206)\n"
              "\t(generator \"kichad_kds\")\n"
              "\t(generator_version \"10.0\")\n"
              "\t(layer \"F.Cu\")\n";

    if( !aFootprint["description"].get_ref<const std::string&>().empty() )
        aSource += "\t(descr " + quoteText( aFootprint["description"].get<std::string>() ) + ")\n";

    if( !aFootprint["keywords"].get_ref<const std::string&>().empty() )
        aSource += "\t(tags " + quoteText( aFootprint["keywords"].get<std::string>() ) + ")\n";

    auto fieldUuid = [&]( const std::string& aName )
    {
        return KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                aProject, "footprint-field", id + ":" + aName );
    };
    aSource += property( "Reference", aFootprint["reference"].get<std::string>(),
                         "F.SilkS", false, -2'540'000, fieldUuid( "Reference" ) );
    aSource += property( "Value", aFootprint["value"].get<std::string>(),
                         "F.Fab", false, 2'540'000, fieldUuid( "Value" ) );
    aSource += property( "Datasheet", aFootprint["datasheet"].get<std::string>(),
                         "F.Fab", true, 0, fieldUuid( "Datasheet" ) );
    aSource += property( "Description", aFootprint["description"].get<std::string>(),
                         "F.Fab", true, 0, fieldUuid( "Description" ) );

    const JSON& attributes = aFootprint["attributes"];
    const std::pair<const char*, const char*> attributeTokens[] = {
        { "smd", "smd" }, { "throughHole", "through_hole" },
        { "boardOnly", "board_only" }, { "excludeFromPosition", "exclude_from_pos_files" },
        { "excludeFromBom", "exclude_from_bom" },
        { "allowMissingCourtyard", "allow_missing_courtyard" }, { "dnp", "dnp" },
        { "allowSoldermaskBridges", "allow_soldermask_bridges" }
    };
    std::string attr;

    for( const auto& [field, token] : attributeTokens )
    {
        if( !attributes.contains( field ) || !attributes[field].is_boolean() )
            return false;

        if( attributes[field].get<bool>() )
            attr += " " + std::string( token );
    }

    if( !attr.empty() )
        aSource += "\t(attr" + attr + ")\n";

    if( !aFootprint["netTieGroups"].empty() )
    {
        aSource += "\t(net_tie_pad_groups";

        for( const JSON& group : aFootprint["netTieGroups"] )
        {
            if( !group.is_array() || group.size() < 2 )
                return false;

            std::string nativeGroup;

            for( const JSON& number : group )
            {
                if( !number.is_string() )
                    return false;

                if( !nativeGroup.empty() )
                    nativeGroup += ", ";

                nativeGroup += number.get<std::string>();
            }

            aSource += " " + quoteText( nativeGroup );
        }

        aSource += ")\n";
    }

    aSource += "\t(duplicate_pad_numbers_are_jumpers "
               + std::string( aFootprint["duplicatePadNumbersAreJumpers"].get<bool>()
                                      ? "yes" : "no" )
               + ")\n";

    if( !aFootprint["jumperGroups"].empty() )
    {
        aSource += "\t(jumper_pad_groups\n";

        for( const JSON& group : aFootprint["jumperGroups"] )
        {
            if( !group.is_array() || group.size() < 2 )
                return false;

            aSource += "\t\t(";

            for( const JSON& number : group )
            {
                if( !number.is_string() )
                    return false;

                aSource += quoteText( number.get<std::string>() ) + " ";
            }

            aSource += ")\n";
        }

        aSource += "\t)\n";
    }

    for( const JSON& graphic : aFootprint["graphics"] )
    {
        const std::string logicalId = graphic.value( "id", "" );
        const std::string uuid = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                aProject, "footprint-graphic", id + ":" + logicalId );

        if( logicalId.empty() || !GRAPHIC_GENERATOR::Render( graphic, uuid, aSource ) )
            return false;

        ++aResult.counts["graphics"].get_ref<int64_t&>();
    }

    for( const JSON& text : aFootprint["texts"] )
    {
        const std::string logicalId = text.value( "id", "" );
        const std::string uuid = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                aProject, "footprint-text", id + ":" + logicalId );

        if( logicalId.empty() || !TEXT_GENERATOR::Render( text, uuid, aSource ) )
            return false;

        ++aResult.counts["texts"].get_ref<int64_t&>();
    }

    for( const JSON& textBox : aFootprint["textBoxes"] )
    {
        const std::string logicalId = textBox.value( "id", "" );
        const std::string uuid = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                aProject, "footprint-text-box", id + ":" + logicalId );

        if( logicalId.empty() || !TEXT_GENERATOR::Render( textBox, uuid, aSource ) )
            return false;

        ++aResult.counts["textBoxes"].get_ref<int64_t&>();
    }

    for( const JSON& pad : aFootprint["pads"] )
    {
        const std::string logicalId = pad.value( "id", "" );
        const std::string uuid = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                aProject, "footprint-pad", id + ":" + logicalId );

        if( logicalId.empty() || !PAD_GENERATOR::Render( pad, uuid, aSource ) )
            return false;

        ++aResult.counts["pads"].get_ref<int64_t&>();
    }

    aSource += "\t(embedded_fonts no)\n";

    for( const JSON& model : aFootprint["models"] )
    {
        if( !renderModel( model, aSource ) )
            return false;

        ++aResult.counts["models"].get_ref<int64_t&>();
    }

    aSource += ")\n";
    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT
DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( const JSON& aCompilerIr )
{
    RESULT result;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "project" )
        || !aCompilerIr["project"].is_object() || !aCompilerIr["project"].contains( "name" )
        || !aCompilerIr["project"]["name"].is_string()
        || !aCompilerIr.contains( "libraries" ) || !aCompilerIr["libraries"].is_array()
        || !aCompilerIr.contains( "authoredFootprints" )
        || !aCompilerIr["authoredFootprints"].is_array() )
    {
        diagnostic( result, "invalid_authored_footprint_ir",
                    "compiled KDS footprint-library IR is malformed" );
        return result;
    }

    const std::string project = aCompilerIr["project"]["name"].get<std::string>();
    std::map<std::string, std::vector<const JSON*>> footprintsByLibrary;

    for( const JSON& footprint : aCompilerIr["authoredFootprints"] )
    {
        if( !footprint.is_object() || !footprint.contains( "library" )
            || !footprint["library"].is_string() )
        {
            diagnostic( result, "invalid_authored_footprint_ir",
                        "compiled KDS contains a malformed authored footprint" );
            continue;
        }

        footprintsByLibrary[footprint["library"].get<std::string>()].push_back( &footprint );
    }

    std::set<std::string> generatedLibraries;

    for( const JSON& library : aCompilerIr["libraries"] )
    {
        if( !library.is_object() || library.value( "kind", "" ) != "footprint"
            || library.value( "table", "" ) != "project"
            || !library.value( "managed", false ) )
        {
            continue;
        }

        const std::string nickname = library.value( "id", "" );

        if( nickname.empty() || !generatedLibraries.emplace( nickname ).second
            || !footprintsByLibrary.contains( nickname )
            || footprintsByLibrary[nickname].empty() )
        {
            diagnostic( result, "invalid_managed_footprint_library",
                        "each managed project footprint library requires one unique declaration "
                        "and at least one authored footprint" );
            continue;
        }

        std::vector<const JSON*>& footprints = footprintsByLibrary[nickname];
        std::sort( footprints.begin(), footprints.end(), []( const JSON* aLeft, const JSON* aRight )
        {
            return aLeft->value( "name", "" ) < aRight->value( "name", "" );
        } );
        JSON names = JSON::array();

        for( const JSON* footprint : footprints )
        {
            const std::string name = footprint->value( "name", "" );
            std::string source;

            if( name.empty() || !renderFootprint( *footprint, project, source, result )
                || source.size() > MAX_NATIVE_FOOTPRINT_BYTES )
            {
                diagnostic( result, "invalid_authored_footprint_ir",
                            "authored footprint backend rejected "
                                    + footprint->value( "id", "unknown" ) );
                continue;
            }

            std::string parseError;
            std::unique_ptr<DOCUMENT> parsed = DOCUMENT::Parse( source, &parseError );

            if( !parsed || parsed->Roots().size() != 1
                || parsed->ListHead( parsed->Roots().front() ) != "footprint" )
            {
                diagnostic( result, "invalid_generated_footprint",
                            "generated footprint " + nickname + ":" + name
                                    + " is not structural KiCad data: " + parseError );
                continue;
            }

            result.sources[nickname + ":" + name] = std::move( source );
            names.push_back( name );
            ++result.counts["footprints"].get_ref<int64_t&>();
        }

        result.libraries[nickname] = std::move( names );
        ++result.counts["libraries"].get_ref<int64_t&>();
    }

    for( const auto& [nickname, footprints] : footprintsByLibrary )
    {
        if( !generatedLibraries.contains( nickname ) )
            diagnostic( result, "unresolved_managed_footprint_library",
                        "authored footprints target undeclared managed project library "
                                + nickname );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
