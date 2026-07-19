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

#include "design_script_symbol_resolver.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT;

constexpr size_t MAX_LIBRARY_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_RESOLVED_SYMBOLS = 4096;
constexpr size_t MAX_PINS_PER_UNIT = 1024;
constexpr int64_t MAX_SYMBOL_LIBRARY_VERSION = 20251024;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


std::vector<size_t> directLists( const DOCUMENT& aDocument, size_t aParent,
                                 const std::string& aHead )
{
    std::vector<size_t> result;

    for( size_t child : aDocument.Nodes().at( aParent ).children )
    {
        if( aDocument.Nodes().at( child ).kind == DOCUMENT::NODE_KIND::LIST
            && aDocument.ListHead( child ) == aHead )
        {
            result.push_back( child );
        }
    }

    return result;
}


bool directScalar( const DOCUMENT& aDocument, size_t aParent, const std::string& aHead,
                   std::string& aValue )
{
    const std::vector<size_t> lists = directLists( aDocument, aParent, aHead );

    if( lists.size() != 1 )
        return false;

    const DOCUMENT::NODE& node = aDocument.Nodes().at( lists.front() );

    if( node.children.size() < 2
        || aDocument.Nodes().at( node.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    aValue = aDocument.AtomText( node.children[1] );
    return true;
}


bool optionalNativeBool( const DOCUMENT& aDocument, size_t aParent,
                         const std::string& aHead, bool aDefault, bool& aValue )
{
    const std::vector<size_t> lists = directLists( aDocument, aParent, aHead );

    if( lists.empty() )
    {
        aValue = aDefault;
        return true;
    }

    if( lists.size() != 1 )
        return false;

    const DOCUMENT::NODE& node = aDocument.Nodes().at( lists.front() );

    if( node.children.size() != 2
        || aDocument.Nodes().at( node.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    const std::string value = aDocument.AtomText( node.children[1] );

    if( value != "yes" && value != "no" )
        return false;

    aValue = value == "yes";
    return true;
}


std::string quoted( const std::string& aText )
{
    std::string result = "\"";

    for( char character : aText )
    {
        if( character == '\\' || character == '"' )
            result.push_back( '\\' );

        result.push_back( character );
    }

    result.push_back( '"' );
    return result;
}


bool millimetresToNanometres( const std::string& aText, int64_t& aValue )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    std::from_chars_result converted = std::from_chars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double rounded = std::round( value * 1'000'000.0L );

    if( !std::isfinite( rounded ) || rounded < -2'000'000'000.0L
        || rounded > 2'000'000'000.0L )
    {
        return false;
    }

    aValue = static_cast<int64_t>( rounded );
    return true;
}


bool unitIdentity( const std::string& aName, int64_t& aUnit, int64_t& aBodyStyle )
{
    const size_t bodySeparator = aName.rfind( '_' );

    if( bodySeparator == std::string::npos )
        return false;

    const size_t unitSeparator = aName.rfind( '_', bodySeparator - 1 );

    if( unitSeparator == std::string::npos )
        return false;

    const char* unitBegin = aName.data() + unitSeparator + 1;
    const char* unitEnd = aName.data() + bodySeparator;
    const char* bodyBegin = unitEnd + 1;
    const char* bodyEnd = aName.data() + aName.size();
    std::from_chars_result unitResult = std::from_chars( unitBegin, unitEnd, aUnit );
    std::from_chars_result bodyResult = std::from_chars( bodyBegin, bodyEnd, aBodyStyle );
    return unitResult.ec == std::errc() && unitResult.ptr == unitEnd
           && bodyResult.ec == std::errc() && bodyResult.ptr == bodyEnd
           && aUnit >= 0 && aUnit <= 256 && aBodyStyle >= 0 && aBodyStyle <= 64;
}


bool pinMetadata( const DOCUMENT& aDocument, size_t aPin, JSON& aPinJson )
{
    std::string number;
    const std::vector<size_t> at = directLists( aDocument, aPin, "at" );

    if( !directScalar( aDocument, aPin, "number", number ) || number.empty()
        || number.size() > 64 || at.size() != 1 )
    {
        return false;
    }

    const DOCUMENT::NODE& position = aDocument.Nodes().at( at.front() );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( position.children.size() < 3
        || aDocument.Nodes().at( position.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
        || aDocument.Nodes().at( position.children[2] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    xText = aDocument.AtomText( position.children[1] );
    yText = aDocument.AtomText( position.children[2] );

    if( !millimetresToNanometres( xText, x ) || !millimetresToNanometres( yText, y ) )
        return false;

    aPinJson = { { "number", number }, { "xNm", x }, { "yNm", y } };
    return true;
}


bool splitLibraryId( const std::string& aLibraryId, std::string& aNickname,
                     std::string& aItem )
{
    const size_t separator = aLibraryId.find( ':' );

    if( separator == std::string::npos || separator == 0 || separator + 1 >= aLibraryId.size()
        || aLibraryId.find( ':', separator + 1 ) != std::string::npos )
    {
        return false;
    }

    aNickname = aLibraryId.substr( 0, separator );
    aItem = aLibraryId.substr( separator + 1 );
    return !aNickname.empty() && !aItem.empty();
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT
DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve( const JSON& aCompilerIr, const JSON& aLibrarySources )
{
    RESULT result;
    result.counts = { { "libraries", 0 }, { "symbols", 0 }, { "pins", 0 } };

    if( !aCompilerIr.is_object() || aCompilerIr.value( "language", "" ) != "kichad-design"
        || aCompilerIr.value( "version", 0 ) != 1 || !aCompilerIr.contains( "libraries" )
        || !aCompilerIr["libraries"].is_array() || !aCompilerIr.contains( "schematic" )
        || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array()
        || !aLibrarySources.is_object() )
    {
        diagnostic( result, "invalid_compiler_ir",
                    "symbol resolution requires valid KiChad Design Script version 1 IR" );
        return result;
    }

    std::map<std::string, JSON> declarations;

    for( const JSON& library : aCompilerIr["libraries"] )
    {
        if( library.is_object() && library.value( "kind", "" ) == "symbol"
            && library.contains( "id" ) && library["id"].is_string() )
        {
            declarations[library["id"].get<std::string>()] = library;
        }
    }

    std::map<std::string, std::set<int64_t>> requested;

    for( const JSON& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "symbol" )
            || !component["symbol"].is_string() || !component.contains( "units" )
            || !component["units"].is_array() )
        {
            diagnostic( result, "invalid_component_ir", "component symbol IR is malformed" );
            continue;
        }

        const std::string libraryId = component["symbol"].get<std::string>();

        for( const JSON& unit : component["units"] )
        {
            if( unit.is_object() && unit.contains( "number" )
                && unit["number"].is_number_integer() )
            {
                requested[libraryId].insert( unit["number"].get<int64_t>() );
            }
        }
    }

    if( requested.size() > MAX_RESOLVED_SYMBOLS )
    {
        diagnostic( result, "too_many_symbols", "at most 4096 distinct symbols may be resolved" );
        return result;
    }

    std::map<std::string, std::unique_ptr<DOCUMENT>> parsedLibraries;

    for( const auto& [ libraryId, requestedUnits ] : requested )
    {
        std::string nickname;
        std::string itemName;

        if( !splitLibraryId( libraryId, nickname, itemName ) || !declarations.contains( nickname ) )
        {
            diagnostic( result, "unresolved_symbol_library",
                        "symbol " + libraryId + " has no declared library" );
            continue;
        }

        const JSON& declaration = declarations.at( nickname );

        if( declaration.value( "table", "" ) != "project" )
        {
            diagnostic( result, "global_symbol_resolution_not_supported",
                        "executable schematic symbols require a confined project library; "
                        + libraryId + " uses a global library" );
            continue;
        }

        if( !aLibrarySources.contains( nickname ) || !aLibrarySources[nickname].is_string() )
        {
            diagnostic( result, "missing_symbol_library_source",
                        "project symbol library " + nickname + " was not inventoried" );
            continue;
        }

        if( !parsedLibraries.contains( nickname ) )
        {
            const std::string source = aLibrarySources[nickname].get<std::string>();
            std::string parseError;

            if( source.empty() || source.size() > MAX_LIBRARY_BYTES )
            {
                diagnostic( result, "invalid_symbol_library",
                            "project symbol library " + nickname
                                    + " must contain 1 byte to 16 MiB" );
                continue;
            }

            std::unique_ptr<DOCUMENT> document = DOCUMENT::Parse( source, &parseError );

            if( !document || document->Roots().size() != 1
                || document->ListHead( document->Roots().front() ) != "kicad_symbol_lib" )
            {
                diagnostic( result, "invalid_symbol_library",
                            nickname + ": "
                                    + ( parseError.empty() ? "expected one kicad_symbol_lib root"
                                                           : parseError ) );
                continue;
            }

            std::string versionText;
            int64_t version = 0;
            const size_t root = document->Roots().front();

            if( !directScalar( *document, root, "version", versionText ) )
            {
                diagnostic( result, "invalid_symbol_library",
                            nickname + ": symbol library has no unique version" );
                continue;
            }

            const char* versionBegin = versionText.data();
            const char* versionEnd = versionBegin + versionText.size();
            const std::from_chars_result converted =
                    std::from_chars( versionBegin, versionEnd, version );

            if( converted.ec != std::errc() || converted.ptr != versionEnd
                || version < 20200126 || version > MAX_SYMBOL_LIBRARY_VERSION )
            {
                diagnostic( result, "unsupported_symbol_library_version",
                            nickname + ": symbol library version is not supported by KiCad 10" );
                continue;
            }

            parsedLibraries[nickname] = std::move( document );
            ++result.counts["libraries"].get_ref<int64_t&>();
        }

        if( !parsedLibraries.contains( nickname ) )
            continue;

        const DOCUMENT& library = *parsedLibraries.at( nickname );
        const size_t root = library.Roots().front();
        std::vector<size_t> matches;

        for( size_t symbol : directLists( library, root, "symbol" ) )
        {
            const DOCUMENT::NODE& node = library.Nodes().at( symbol );

            if( node.children.size() >= 2
                && library.Nodes().at( node.children[1] ).kind != DOCUMENT::NODE_KIND::LIST
                && library.AtomText( node.children[1] ) == itemName )
            {
                matches.push_back( symbol );
            }
        }

        if( matches.size() != 1 )
        {
            diagnostic( result, "unresolved_symbol",
                        "project library " + nickname + " must contain exactly one symbol "
                                + itemName );
            continue;
        }

        const size_t symbol = matches.front();

        if( !directLists( library, symbol, "extends" ).empty() )
        {
            diagnostic( result, "derived_symbol_not_supported",
                        "symbol " + libraryId
                                + " uses inheritance, which is not yet safely cache-resolved" );
            continue;
        }

        bool excludeFromSim = false;
        bool inBom = true;
        bool onBoard = true;
        bool inPosFiles = true;

        if( !optionalNativeBool( library, symbol, "exclude_from_sim", false,
                                 excludeFromSim )
            || !optionalNativeBool( library, symbol, "in_bom", true, inBom )
            || !optionalNativeBool( library, symbol, "on_board", true, onBoard )
            || !optionalNativeBool( library, symbol, "in_pos_files", true, inPosFiles ) )
        {
            diagnostic( result, "invalid_symbol_flags",
                        "symbol " + libraryId
                                + " has duplicate or malformed native inclusion flags" );
            continue;
        }

        JSON flags = { { "excludeFromSim", excludeFromSim },
                       { "inBom", inBom },
                       { "onBoard", onBoard },
                       { "inPosFiles", inPosFiles } };

        JSON properties = JSON::object();

        for( size_t property : directLists( library, symbol, "property" ) )
        {
            const DOCUMENT::NODE& node = library.Nodes().at( property );

            if( node.children.size() >= 3
                && library.Nodes().at( node.children[1] ).kind != DOCUMENT::NODE_KIND::LIST
                && library.Nodes().at( node.children[2] ).kind != DOCUMENT::NODE_KIND::LIST )
            {
                properties[library.AtomText( node.children[1] )] =
                        library.AtomText( node.children[2] );
            }
        }

        std::map<int64_t, JSON> pinsByUnit;
        std::set<int64_t> presentUnits;

        for( size_t unitNode : directLists( library, symbol, "symbol" ) )
        {
            const DOCUMENT::NODE& node = library.Nodes().at( unitNode );
            int64_t unit = 0;
            int64_t bodyStyle = 0;

            if( node.children.size() < 2
                || library.Nodes().at( node.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
                || !unitIdentity( library.AtomText( node.children[1] ), unit, bodyStyle ) )
            {
                diagnostic( result, "invalid_symbol_unit",
                            "symbol " + libraryId + " contains a malformed unit name" );
                continue;
            }

            if( bodyStyle != 0 && bodyStyle != 1 )
                continue;

            presentUnits.emplace( unit );

            for( size_t pin : directLists( library, unitNode, "pin" ) )
            {
                JSON metadata;

                if( !pinMetadata( library, pin, metadata ) )
                {
                    diagnostic( result, "invalid_symbol_pin",
                                "symbol " + libraryId + " contains a malformed pin" );
                    continue;
                }

                pinsByUnit[unit].push_back( std::move( metadata ) );
            }
        }

        JSON units = JSON::object();

        for( int64_t unit : requestedUnits )
        {
            if( !presentUnits.contains( unit ) )
            {
                diagnostic( result, "unresolved_symbol_unit",
                            "symbol " + libraryId + " has no unit " + std::to_string( unit ) );
                continue;
            }

            JSON pins = JSON::array();

            if( pinsByUnit.contains( 0 ) )
            {
                for( const JSON& pin : pinsByUnit.at( 0 ) )
                    pins.push_back( pin );
            }

            if( pinsByUnit.contains( unit ) )
            {
                for( const JSON& pin : pinsByUnit.at( unit ) )
                    pins.push_back( pin );
            }

            if( pins.empty() || pins.size() > MAX_PINS_PER_UNIT )
            {
                diagnostic( result, "invalid_symbol_pin_count",
                            "symbol " + libraryId + " unit " + std::to_string( unit )
                                    + " must expose 1 through 1024 pins" );
                continue;
            }

            std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
                       {
                           if( aLeft["number"] != aRight["number"] )
                               return aLeft["number"].get<std::string>()
                                      < aRight["number"].get<std::string>();

                           if( aLeft["xNm"] != aRight["xNm"] )
                               return aLeft["xNm"].get<int64_t>()
                                      < aRight["xNm"].get<int64_t>();

                           return aLeft["yNm"].get<int64_t>()
                                  < aRight["yNm"].get<int64_t>();
                       } );
            result.counts["pins"] = result.counts["pins"].get<int64_t>() + pins.size();
            units[std::to_string( unit )] = std::move( pins );
        }

        std::string cacheSource = library.RawText( symbol );
        std::string editError;
        std::unique_ptr<DOCUMENT> cacheDocument = DOCUMENT::Parse( cacheSource, &editError );

        if( !cacheDocument || cacheDocument->Roots().size() != 1 )
        {
            diagnostic( result, "invalid_symbol_cache_source",
                        libraryId + ": " + editError );
            continue;
        }

        const size_t cacheRoot = cacheDocument->Roots().front();
        const DOCUMENT::NODE& cacheNode = cacheDocument->Nodes().at( cacheRoot );

        if( cacheNode.children.size() < 2
            || !cacheDocument->ReplaceNode( cacheNode.children[1], quoted( libraryId ),
                                            &editError )
            || !cacheDocument->Render( cacheSource, &editError ) )
        {
            diagnostic( result, "invalid_symbol_cache_source",
                        libraryId + ": " + editError );
            continue;
        }

        result.symbols[libraryId] = { { "libraryId", libraryId },
                                      { "cacheSource", cacheSource },
                                      { "flags", std::move( flags ) },
                                      { "properties", std::move( properties ) },
                                      { "units", std::move( units ) } };
    }

    result.counts["symbols"] = result.symbols.size();
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
