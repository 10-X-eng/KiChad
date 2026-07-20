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

#include "design_script_board_graphic_compiler.h"

#include "design_script_footprint_graphic_compiler.h"

#include <set>
#include <string>


namespace
{

using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::RESULT;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


void normalizeDiagnostic( JSON& aDiagnostic )
{
    if( aDiagnostic.contains( "code" ) && aDiagnostic["code"].is_string() )
    {
        std::string code = aDiagnostic["code"].get<std::string>();
        const std::string from = "footprint_graphic";
        const size_t position = code.find( from );

        if( position != std::string::npos )
            code.replace( position, from.size(), "board_graphic" );

        aDiagnostic["code"] = std::move( code );
    }

    if( aDiagnostic.contains( "message" ) && aDiagnostic["message"].is_string() )
    {
        std::string message = aDiagnostic["message"].get<std::string>();
        size_t position = 0;

        while( ( position = message.find( "footprint graphic", position ) )
               != std::string::npos )
        {
            message.replace( position, 17, "board graphic" );
            position += 13;
        }

        aDiagnostic["message"] = std::move( message );
    }
}


bool copperLayer( const std::string& aLayer )
{
    return aLayer == "F.Cu" || aLayer == "B.Cu"
           || ( aLayer.starts_with( "In" ) && aLayer.ends_with( ".Cu" ) );
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::IsGraphicHead( const std::string& aHead )
{
    return DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER::IsGraphicHead( aHead );
}


DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::RESULT
DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER::RESULT graphic =
            DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER::Compile( aDocument, aNode );

    for( JSON& entry : graphic.diagnostics )
    {
        normalizeDiagnostic( entry );
        result.diagnostics.push_back( std::move( entry ) );
    }

    if( !graphic.graphic.is_object() || !graphic.graphic.contains( "id" ) )
    {
        result.ok = false;
        return result;
    }

    std::set<std::string> layers;

    for( const JSON& layer : graphic.graphic.value( "layers", JSON::array() ) )
    {
        if( layer.is_string() )
            layers.emplace( layer.get<std::string>() );
    }

    const bool oneLayer = layers.size() == 1;
    const bool frontMask = layers == std::set<std::string>{ "F.Cu", "F.Mask" };
    const bool backMask = layers == std::set<std::string>{ "B.Cu", "B.Mask" };

    if( !oneLayer && !frontMask && !backMask )
        diagnostic( result, "invalid_authored_board_graphic_layers",
                    "board graphics require one layer or exactly F.Cu/F.Mask or B.Cu/B.Mask" );

    const std::string net = graphic.graphic.value( "net", "" );
    const std::string primaryLayer = frontMask ? "F.Cu" : backMask ? "B.Cu"
                                               : layers.empty() ? "" : *layers.begin();

    if( !net.empty() && !copperLayer( primaryLayer ) )
        diagnostic( result, "invalid_authored_board_graphic_net_layer",
                    "a board graphic net requires a copper layer" );

    const std::string fill = graphic.graphic.value( "fill", "" );

    if( fill != "none" && fill != "solid" )
        diagnostic( result, "invalid_authored_board_graphic_fill",
                    "typed board graphics support none or solid fill" );

    result.statement = {
        { "kind", "board_graphic" },
        { "logicalId", graphic.graphic.at( "id" ) },
        { "graphic", std::move( graphic.graphic ) },
        { "typed", true }
    };
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
