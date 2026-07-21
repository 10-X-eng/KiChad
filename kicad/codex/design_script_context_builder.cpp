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

#include "design_script_context_builder.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string_view>

#include <picosha2.h>


namespace
{

using JSON = nlohmann::json;

constexpr size_t MAX_CONTEXT_STRING_BYTES = 1024;
constexpr size_t MAX_CONTEXT_ARRAY_ITEMS = 64;
constexpr size_t MAX_CONTEXT_OBJECT_FIELDS = 128;
constexpr size_t MAX_CONTEXT_DEPTH = 12;
constexpr size_t MAX_CONTEXT_ITEM_BYTES = 24 * 1024;
constexpr size_t MAX_CONTEXT_PAGE_BYTES = 160 * 1024;


std::string lower( std::string aValue )
{
    std::transform( aValue.begin(), aValue.end(), aValue.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );
    return aValue;
}


bool payloadKey( std::string_view aKey )
{
    static const std::set<std::string_view> PAYLOAD_KEYS = {
        "dataBase64", "cacheSource", "source", "rawSource", "encodedData"
    };
    return PAYLOAD_KEYS.contains( aKey );
}


JSON sanitize( const JSON& aValue, size_t aDepth = 0 )
{
    if( aDepth >= MAX_CONTEXT_DEPTH )
        return { { "omitted", "maximum semantic context depth" } };

    if( aValue.is_string() )
    {
        const std::string& value = aValue.get_ref<const std::string&>();

        if( value.size() <= MAX_CONTEXT_STRING_BYTES )
            return value;

        std::string digest;
        picosha2::hash256_hex_string( value, digest );
        return { { "textOmitted", true }, { "bytes", value.size() }, { "sha256", digest } };
    }

    if( aValue.is_array() )
    {
        JSON result = JSON::array();
        const size_t included = std::min( aValue.size(), MAX_CONTEXT_ARRAY_ITEMS );

        for( size_t index = 0; index < included; ++index )
            result.push_back( sanitize( aValue[index], aDepth + 1 ) );

        if( included < aValue.size() )
            result.push_back( { { "omittedItems", aValue.size() - included } } );

        return result;
    }

    if( aValue.is_object() )
    {
        JSON result = JSON::object();
        size_t fields = 0;

        for( auto field = aValue.begin(); field != aValue.end(); ++field )
        {
            if( fields >= MAX_CONTEXT_OBJECT_FIELDS )
            {
                result["omittedFields"] = aValue.size() - fields;
                break;
            }

            if( payloadKey( field.key() ) )
            {
                const size_t bytes = field->is_string() ? field->get_ref<const std::string&>().size()
                                                        : field->dump().size();
                result[field.key() + "Omitted"] = { { "encodedBytes", bytes } };
            }
            else
            {
                result[field.key()] = sanitize( *field, aDepth + 1 );
            }

            ++fields;
        }

        return result;
    }

    return aValue;
}


std::string itemIdentity( const JSON& aItem, size_t aFallback )
{
    for( const char* key : { "id", "reference", "name", "component", "kind" } )
    {
        if( aItem.contains( key ) && aItem[key].is_string()
            && !aItem[key].get_ref<const std::string&>().empty() )
        {
            return aItem[key].get<std::string>();
        }
    }

    return std::to_string( aFallback );
}


JSON compactItem( const std::string& aDomain, const std::string& aKind,
                  const std::string& aId, const JSON& aData )
{
    JSON item = { { "domain", aDomain }, { "kind", aKind }, { "id", aId },
                  { "data", sanitize( aData ) } };
    const std::string serialized = item.dump();

    if( serialized.size() <= MAX_CONTEXT_ITEM_BYTES )
        return item;

    std::string digest;
    picosha2::hash256_hex_string( serialized, digest );
    return { { "domain", aDomain }, { "kind", aKind }, { "id", aId },
             { "detailOmitted", true }, { "semanticBytes", serialized.size() },
             { "semanticSha256", digest } };
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_CONTEXT_BUILDER::JSON DESIGN_SCRIPT_CONTEXT_BUILDER::Build(
        const JSON& aIr, const JSON& aPlan, const std::string& aDomain,
        const std::string& aQuery, size_t aOffset, size_t aLimit )
{
    JSON items = JSON::array();
    const std::string query = lower( aQuery );
    size_t matchingItems = 0;
    size_t pageBytes = 0;
    bool byteLimited = false;

    const auto add = [&]( const std::string& aItemDomain, const std::string& aKind,
                          const std::string& aId, const JSON& aData )
    {
        if( aDomain != "all" && aDomain != aItemDomain )
            return;

        JSON item = compactItem( aItemDomain, aKind, aId, aData );
        const std::string searchable = lower( item.dump() );

        if( !query.empty() && searchable.find( query ) == std::string::npos )
            return;

        const size_t index = matchingItems++;

        if( index < aOffset || items.size() >= aLimit || byteLimited )
            return;

        const size_t bytes = item.dump().size();

        if( !items.empty() && pageBytes > MAX_CONTEXT_PAGE_BYTES - bytes )
        {
            byteLimited = true;
            return;
        }

        pageBytes += bytes;
        items.push_back( std::move( item ) );
    };

    add( "project", "project", aIr.value( "project", JSON::object() ).value( "name", "project" ),
         { { "project", aIr.value( "project", JSON::object() ) },
           { "units", aIr.value( "units", "mm" ) },
           { "compilerPlan", aPlan } } );

    const auto addArray = [&]( const char* aIrKey, const char* aDomainName,
                               const char* aKind )
    {
        if( !aIr.contains( aIrKey ) || !aIr[aIrKey].is_array() )
            return;

        size_t fallback = 0;

        for( const JSON& entry : aIr[aIrKey] )
            add( aDomainName, aKind, itemIdentity( entry, fallback++ ), entry );
    };

    addArray( "libraries", "libraries", "library" );
    addArray( "authoredSymbols", "libraries", "authored_symbol" );
    addArray( "authoredFootprints", "libraries", "authored_footprint" );

    if( aIr.contains( "schematic" ) && aIr["schematic"].is_object() )
    {
        const JSON& schematic = aIr["schematic"];

        for( const auto& [key, kind] : {
                     std::pair<const char*, const char*>( "sheets", "sheet" ),
                     { "components", "component" }, { "nets", "net" },
                     { "noConnects", "no_connect" }, { "drawings", "drawing" },
                     { "busAliases", "bus_alias" }, { "groups", "group" } } )
        {
            if( !schematic.contains( key ) || !schematic[key].is_array() )
                continue;

            size_t fallback = 0;

            for( const JSON& entry : schematic[key] )
                add( "schematic", kind, itemIdentity( entry, fallback++ ), entry );
        }
    }

    if( aIr.contains( "pcb" ) && aIr["pcb"].is_array() )
    {
        size_t fallback = 0;

        for( const JSON& statement : aIr["pcb"] )
            add( "pcb", statement.value( "kind", "statement" ),
                 itemIdentity( statement, fallback++ ), statement );
    }

    for( const auto& [key, kind] : {
                 std::pair<const char*, const char*>( "rules", "rules" ),
                 { "netClasses", "net_classes" }, { "customRules", "custom_rules" } } )
    {
        if( aIr.contains( key ) && !aIr[key].is_null() )
            add( "pcb", kind, kind, aIr[key] );
    }

    addArray( "sourcing", "manufacturing", "source" );
    addArray( "checks", "manufacturing", "check" );
    addArray( "outputs", "manufacturing", "output" );

    if( aIr.contains( "production" ) && aIr["production"].is_object() )
        add( "manufacturing", "production", "production", aIr["production"] );

    const size_t returned = items.size();
    const size_t nextOffset = aOffset + returned;
    JSON result = { { "schema", "kichad.design-context.v1" },
                    { "domain", aDomain },
                    { "query", aQuery },
                    { "offset", aOffset },
                    { "limit", aLimit },
                    { "total", matchingItems },
                    { "returned", returned },
                    { "byteLimited", byteLimited },
                    { "items", std::move( items ) } };

    if( nextOffset < matchingItems )
        result["nextOffset"] = nextOffset;
    else
        result["nextOffset"] = nullptr;

    return result;
}

} // namespace KICHAD
