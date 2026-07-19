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

#include "design_script_pcb_reconciler.h"

#include "design_script_pcb_planner.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>


namespace
{

using JSON = nlohmann::json;
using CONTEXT = KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT;
using RESULT = KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT;

constexpr size_t MAX_MANAGED_ITEMS = 10000;
constexpr size_t MAX_LIVE_ITEMS = 20000;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool boundedText( const std::string& aText, size_t aMaximum )
{
    return !aText.empty() && aText.size() <= aMaximum
           && aText.find( '\0' ) == std::string::npos;
}


bool sha256( const std::string& aText )
{
    return aText.size() == 64
           && std::all_of( aText.begin(), aText.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isdigit( aCharacter )
                                      || ( aCharacter >= 'a' && aCharacter <= 'f' );
                           } );
}


bool uuid( const std::string& aText )
{
    if( aText.size() != 36 || aText[8] != '-' || aText[13] != '-'
        || aText[18] != '-' || aText[23] != '-' )
    {
        return false;
    }

    for( size_t i = 0; i < aText.size(); ++i )
    {
        if( i == 8 || i == 13 || i == 18 || i == 23 )
            continue;

        const unsigned char character = static_cast<unsigned char>( aText[i] );

        if( !std::isdigit( character ) && !( character >= 'a' && character <= 'f' ) )
            return false;
    }

    return true;
}


bool managedType( const std::string& aType )
{
    return aType == "shape" || aType == "trace" || aType == "arc" || aType == "via"
           || aType == "zone" || aType == "rule_area";
}


bool componentReference( const std::string& aReference )
{
    return boundedText( aReference, 128 )
           && std::all_of( aReference.begin(), aReference.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isalnum( aCharacter ) || aCharacter == '_'
                                      || aCharacter == '-' || aCharacter == '+'
                                      || aCharacter == '.' || aCharacter == '/'
                                      || aCharacter == '#';
                           } );
}


JSON updateFields( const std::string& aType )
{
    if( aType == "shape" )
        return JSON::array( { "shape", "layer", "locked" } );

    if( aType == "trace" )
        return JSON::array( { "start", "end", "width", "layer", "net", "locked" } );

    if( aType == "arc" )
        return JSON::array( { "start", "mid", "end", "width", "layer", "net", "locked" } );

    if( aType == "zone" )
    {
        return JSON::array( { "type", "layers", "outline", "name", "copper_settings",
                              "priority", "filled", "filled_polygons", "border", "locked",
                              "layer_properties" } );
    }

    if( aType == "rule_area" )
    {
        return JSON::array( { "type", "layers", "outline", "name", "rule_area_settings",
                              "priority", "filled", "filled_polygons", "border", "locked",
                              "layer_properties" } );
    }

    return JSON::array( { "position", "pad_stack", "locked", "net", "type" } );
}


struct MANAGED_ITEM
{
    std::string logicalId;
    std::string itemType;
    std::string itemId;
    JSON        item = JSON::object();
};


struct PLACEMENT
{
    std::string component;
    int64_t     xNm = 0;
    int64_t     yNm = 0;
    double      rotationDegrees = 0.0;
    std::string side;
    bool        locked = false;
};


struct LIVE_FOOTPRINT
{
    std::string itemId;
    bool        schematicLinked = false;
};


bool readPlacement( const JSON& aJson, PLACEMENT& aPlacement, RESULT& aResult )
{
    if( !aJson.is_object() || aJson.value( "action", "" ) != "place_by_reference"
        || !aJson.contains( "component" ) || !aJson["component"].is_string()
        || !aJson.contains( "position" ) || !aJson["position"].is_object()
        || !aJson["position"].contains( "xNm" )
        || !aJson["position"]["xNm"].is_number_integer()
        || !aJson["position"].contains( "yNm" )
        || !aJson["position"]["yNm"].is_number_integer()
        || !aJson.contains( "rotationDegrees" )
        || !aJson["rotationDegrees"].is_number()
        || !aJson.contains( "side" ) || !aJson["side"].is_string()
        || !aJson.contains( "locked" ) || !aJson["locked"].is_boolean() )
    {
        diagnostic( aResult, "invalid_placement", "planned component placement is malformed" );
        return false;
    }

    aPlacement.component = aJson["component"].get<std::string>();
    aPlacement.xNm = aJson["position"]["xNm"].get<int64_t>();
    aPlacement.yNm = aJson["position"]["yNm"].get<int64_t>();
    aPlacement.rotationDegrees = aJson["rotationDegrees"].get<double>();
    aPlacement.side = aJson["side"].get<std::string>();
    aPlacement.locked = aJson["locked"].get<bool>();

    if( !componentReference( aPlacement.component )
        || !std::isfinite( aPlacement.rotationDegrees )
        || std::abs( aPlacement.rotationDegrees ) > 360000.0
        || ( aPlacement.side != "front" && aPlacement.side != "back" ) )
    {
        diagnostic( aResult, "invalid_placement",
                    "planned component placement contains an invalid value" );
        return false;
    }

    return true;
}


bool validateContext( const CONTEXT& aContext, RESULT& aResult )
{
    bool valid = true;

    if( !boundedText( aContext.sourcePath, 4096 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "sourcePath is invalid" );
        valid = false;
    }

    if( !boundedText( aContext.boardPath, 4096 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "boardPath is invalid" );
        valid = false;
    }

    if( !boundedText( aContext.projectName, 128 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "projectName is invalid" );
        valid = false;
    }

    if( !sha256( aContext.sourceSha256 ) )
    {
        diagnostic( aResult, "invalid_reconcile_context", "sourceSha256 is invalid" );
        valid = false;
    }

    return valid;
}


bool readManagedItem( const JSON& aJson, const std::string& aProjectName,
                      MANAGED_ITEM& aItem, bool aRequirePayload, RESULT& aResult,
                      const std::string& aCode )
{
    if( !aJson.is_object() || !aJson.contains( "logicalId" )
        || !aJson["logicalId"].is_string() || !aJson.contains( "itemType" )
        || !aJson["itemType"].is_string() || !aJson.contains( "itemId" )
        || !aJson["itemId"].is_string() )
    {
        diagnostic( aResult, aCode, "managed item identity is malformed" );
        return false;
    }

    aItem.logicalId = aJson["logicalId"].get<std::string>();
    aItem.itemType = aJson["itemType"].get<std::string>();
    aItem.itemId = aJson["itemId"].get<std::string>();

    if( !boundedText( aItem.logicalId, 128 ) || !managedType( aItem.itemType )
        || !uuid( aItem.itemId )
        || KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                   aProjectName, aItem.itemType, aItem.logicalId ) != aItem.itemId )
    {
        diagnostic( aResult, aCode, "managed item identity contains an invalid value" );
        return false;
    }

    if( aRequirePayload )
    {
        if( !aJson.contains( "item" ) || !aJson["item"].is_object()
            || !aJson["item"].contains( "id" ) || !aJson["item"]["id"].is_object()
            || !aJson["item"]["id"].contains( "value" )
            || !aJson["item"]["id"]["value"].is_string()
            || aJson["item"]["id"]["value"].get<std::string>() != aItem.itemId )
        {
            diagnostic( aResult, aCode, "planned item payload does not contain its itemId" );
            return false;
        }

        aItem.item = aJson["item"];
    }

    return true;
}


bool readPreviousState( const JSON& aState, const CONTEXT& aContext,
                        std::map<std::string, MANAGED_ITEM>& aItems, RESULT& aResult )
{
    if( aState.is_null() )
        return true;

    if( !aState.is_object() || aState.value( "format", "" ) != "kichad-kds-managed-state"
        || aState.value( "version", 0 ) != 1 || !aState.contains( "sourcePath" )
        || !aState["sourcePath"].is_string() || !aState.contains( "boardPath" )
        || !aState["boardPath"].is_string() || !aState.contains( "projectName" )
        || !aState["projectName"].is_string() || !aState.contains( "sourceSha256" )
        || !aState["sourceSha256"].is_string() || !aState.contains( "managedPcbItems" )
        || !aState["managedPcbItems"].is_array()
        || aState["managedPcbItems"].size() > MAX_MANAGED_ITEMS )
    {
        diagnostic( aResult, "invalid_managed_state",
                    "managed-state manifest is malformed or unsupported" );
        return false;
    }

    if( aState["sourcePath"].get<std::string>() != aContext.sourcePath
        || aState["boardPath"].get<std::string>() != aContext.boardPath
        || aState["projectName"].get<std::string>() != aContext.projectName )
    {
        diagnostic( aResult, "managed_state_scope_mismatch",
                    "managed-state manifest belongs to a different source, board, or project" );
        return false;
    }

    if( !boundedText( aState["projectName"].get<std::string>(), 128 )
        || !sha256( aState["sourceSha256"].get<std::string>() ) )
    {
        diagnostic( aResult, "invalid_managed_state",
                    "managed-state project or source revision is invalid" );
        return false;
    }

    std::set<std::string> logicalIds;

    for( const JSON& entry : aState["managedPcbItems"] )
    {
        MANAGED_ITEM item;

        if( !readManagedItem( entry, aState["projectName"].get<std::string>(), item, false,
                              aResult, "invalid_managed_state" ) )
            continue;

        if( !aItems.emplace( item.itemId, item ).second
            || !logicalIds.emplace( item.logicalId ).second )
        {
            diagnostic( aResult, "invalid_managed_state",
                        "managed-state item identities are not unique" );
        }
    }

    return aResult.diagnostics.empty();
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_PCB_RECONCILER::RESULT DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
        const JSON& aDesiredOperations, const JSON& aPreviousState,
        const JSON& aLiveInventory, const CONTEXT& aContext )
{
    RESULT result;
    result.counts = { { "create", 0 }, { "update", 0 }, { "delete", 0 },
                      { "placement", 0 } };
    validateContext( aContext, result );

    std::map<std::string, MANAGED_ITEM> previous;
    readPreviousState( aPreviousState, aContext, previous, result );

    std::map<std::string, MANAGED_ITEM> desired;
    std::set<std::string>               desiredLogicalIds;
    std::map<std::string, PLACEMENT>    placements;

    if( !aDesiredOperations.is_array() || aDesiredOperations.size() > MAX_MANAGED_ITEMS )
    {
        diagnostic( result, "invalid_desired_operations",
                    "desired PCB operations must be a bounded array" );
    }
    else
    {
        for( const JSON& operation : aDesiredOperations )
        {
            const std::string action = operation.is_object()
                                               ? operation.value( "action", "" )
                                               : "";

            if( action == "place_by_reference" )
            {
                PLACEMENT placement;

                if( readPlacement( operation, placement, result )
                    && !placements.emplace( placement.component, placement ).second )
                {
                    diagnostic( result, "duplicate_component_placement",
                                "component " + placement.component
                                        + " has more than one planned placement" );
                }

                continue;
            }

            if( action != "upsert" )
            {
                diagnostic( result, "unsupported_desired_operation",
                            "apply received an unsupported lowered PCB operation" );
                continue;
            }

            MANAGED_ITEM item;

            if( !readManagedItem( operation, aContext.projectName, item, true, result,
                                  "invalid_desired_operations" ) )
            {
                continue;
            }

            if( !desired.emplace( item.itemId, item ).second
                || !desiredLogicalIds.emplace( item.logicalId ).second )
            {
                diagnostic( result, "invalid_desired_operations",
                            "desired PCB item identities are not unique" );
            }
        }
    }

    std::map<std::string, std::string> live;
    std::map<std::string, std::vector<LIVE_FOOTPRINT>> liveFootprints;
    std::set<std::string> liveFootprintIds;

    if( !aLiveInventory.is_array() || aLiveInventory.size() > MAX_LIVE_ITEMS )
    {
        diagnostic( result, "invalid_live_inventory",
                    "live PCB inventory must be a bounded array" );
    }
    else
    {
        for( const JSON& entry : aLiveInventory )
        {
            if( !entry.is_object() || !entry.contains( "itemId" )
                || !entry["itemId"].is_string() || !entry.contains( "itemType" )
                || !entry["itemType"].is_string()
                || !uuid( entry["itemId"].get<std::string>() )
                || !boundedText( entry["itemType"].get<std::string>(), 128 ) )
            {
                diagnostic( result, "invalid_live_inventory",
                            "live PCB inventory contains an invalid item" );
                continue;
            }

            const std::string itemId = entry["itemId"].get<std::string>();
            const std::string itemType = entry["itemType"].get<std::string>();
            auto [existing, inserted] = live.emplace( itemId, itemType );

            if( !inserted && existing->second != itemType )
            {
                diagnostic( result, "invalid_live_inventory",
                            "live PCB inventory contains conflicting item types" );
            }

            if( itemType == "footprint" )
            {
                if( !entry.contains( "reference" ) || !entry["reference"].is_string()
                    || !componentReference( entry["reference"].get<std::string>() )
                    || !entry.contains( "schematicLinked" )
                    || !entry["schematicLinked"].is_boolean()
                    || !liveFootprintIds.emplace( itemId ).second )
                {
                    diagnostic( result, "invalid_live_inventory",
                                "live footprint inventory contains an invalid identity" );
                    continue;
                }

                liveFootprints[entry["reference"].get<std::string>()].push_back(
                        { itemId, entry["schematicLinked"].get<bool>() } );
            }
        }
    }

    if( !result.diagnostics.empty() )
        return result;

    for( const auto& [itemId, item] : desired )
    {
        auto liveItem = live.find( itemId );

        if( liveItem == live.end() )
        {
            result.actions.push_back( { { "action", "create" },
                                        { "logicalId", item.logicalId },
                                        { "itemType", item.itemType },
                                        { "itemId", item.itemId },
                                        { "item", item.item } } );
            ++result.counts["create"].get_ref<int64_t&>();
            continue;
        }

        auto previousItem = previous.find( itemId );

        if( previousItem == previous.end() )
        {
            diagnostic( result, "unmanaged_uuid_collision",
                        "desired UUID " + itemId + " is already used by an unmanaged board item" );
            continue;
        }

        if( previousItem->second.itemType != item.itemType
            || previousItem->second.logicalId != item.logicalId
            || liveItem->second != item.itemType )
        {
            diagnostic( result, "managed_item_identity_conflict",
                        "managed UUID " + itemId + " has changed identity or live type" );
            continue;
        }

        result.actions.push_back( { { "action", "update" },
                                    { "logicalId", item.logicalId },
                                    { "itemType", item.itemType },
                                    { "itemId", item.itemId },
                                    { "fieldMask", updateFields( item.itemType ) },
                                    { "item", item.item } } );
        ++result.counts["update"].get_ref<int64_t&>();
    }

    for( const auto& [itemId, item] : previous )
    {
        if( desired.contains( itemId ) )
            continue;

        auto liveItem = live.find( itemId );

        if( liveItem == live.end() )
            continue;

        if( liveItem->second != item.itemType )
        {
            diagnostic( result, "managed_item_identity_conflict",
                        "obsolete managed UUID " + itemId + " has a different live type" );
            continue;
        }

        result.actions.push_back( { { "action", "delete" },
                                    { "logicalId", item.logicalId },
                                    { "itemType", item.itemType },
                                    { "itemId", item.itemId } } );
        ++result.counts["delete"].get_ref<int64_t&>();
    }

    for( const auto& [component, placement] : placements )
    {
        auto footprints = liveFootprints.find( component );

        if( footprints == liveFootprints.end() )
        {
            diagnostic( result, "missing_component_footprint",
                        "no live PCB footprint has reference " + component );
            continue;
        }

        if( footprints->second.size() != 1 )
        {
            diagnostic( result, "ambiguous_component_footprint",
                        "more than one live PCB footprint has reference " + component );
            continue;
        }

        const LIVE_FOOTPRINT& footprint = footprints->second.front();

        if( !footprint.schematicLinked )
        {
            diagnostic( result, "unlinked_component_footprint",
                        "PCB footprint " + component + " is not linked to a schematic symbol" );
            continue;
        }

        JSON item = { { "id", { { "value", footprint.itemId } } },
                      { "position", { { "xNm", std::to_string( placement.xNm ) },
                                      { "yNm", std::to_string( placement.yNm ) } } },
                      { "orientation", { { "valueDegrees", placement.rotationDegrees } } },
                      { "layer", placement.side == "front" ? "BL_F_Cu" : "BL_B_Cu" },
                      { "locked", placement.locked ? "LS_LOCKED" : "LS_UNLOCKED" } };

        result.actions.push_back(
                { { "action", "update" },
                  { "component", component },
                  { "itemType", "footprint" },
                  { "itemId", footprint.itemId },
                  { "fieldMask", JSON::array( { "position", "orientation", "layer", "locked" } ) },
                  { "item", std::move( item ) } } );
        ++result.counts["placement"].get_ref<int64_t&>();
    }

    if( !result.diagnostics.empty() )
    {
        result.actions = JSON::array();
        result.counts = { { "create", 0 }, { "update", 0 }, { "delete", 0 },
                          { "placement", 0 } };
        return result;
    }

    JSON managedItems = JSON::array();

    for( const auto& [itemId, item] : desired )
    {
        managedItems.push_back( { { "logicalId", item.logicalId },
                                  { "itemType", item.itemType },
                                  { "itemId", itemId } } );
    }

    result.nextState = { { "format", "kichad-kds-managed-state" },
                         { "version", 1 },
                         { "sourcePath", aContext.sourcePath },
                         { "boardPath", aContext.boardPath },
                         { "projectName", aContext.projectName },
                         { "sourceSha256", aContext.sourceSha256 },
                         { "managedPcbItems", std::move( managedItems ) } };
    result.ok = true;
    return result;
}

} // namespace KICHAD
