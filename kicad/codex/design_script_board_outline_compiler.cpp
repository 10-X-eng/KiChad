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

#include "design_script_board_outline_compiler.h"

#include "design_script_footprint_graphic_compiler.h"

#include <string>


namespace
{

using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_BOARD_OUTLINE_COMPILER::RESULT;


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
            code.replace( position, from.size(), "board_outline" );

        aDiagnostic["code"] = std::move( code );
    }

    if( aDiagnostic.contains( "message" ) && aDiagnostic["message"].is_string() )
    {
        std::string message = aDiagnostic["message"].get<std::string>();

        for( const std::string from : { "footprint graphic", "footprint " } )
        {
            size_t position = 0;

            while( ( position = message.find( from, position ) ) != std::string::npos )
            {
                message.replace( position, from.size(), "board outline " );
                position += 14;
            }
        }

        aDiagnostic["message"] = std::move( message );
    }
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_BOARD_OUTLINE_COMPILER::RESULT
DESIGN_SCRIPT_BOARD_OUTLINE_COMPILER::Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument,
                                               size_t aNode )
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

    if( graphic.graphic.value( "layers", JSON::array() )
        != JSON::array( { "Edge.Cuts" } ) )
    {
        diagnostic( result, "invalid_authored_board_outline_layer",
                    "every board outline primitive requires exactly (layers Edge.Cuts)" );
    }

    if( graphic.graphic.value( "fill", "" ) != "none" )
        diagnostic( result, "invalid_authored_board_outline_fill",
                    "board outline primitives must use (fill none)" );

    if( graphic.graphic.value( "stroke", JSON::object() ).value( "style", "" ) != "solid" )
        diagnostic( result, "invalid_authored_board_outline_stroke",
                    "board outline primitives require a solid stroke" );

    if( !graphic.graphic.value( "solderMaskMarginNm", JSON( nullptr ) ).is_null() )
        diagnostic( result, "invalid_authored_board_outline_mask_margin",
                    "board outline primitives cannot declare a solder-mask margin" );

    result.statement = {
        { "kind", "outline_shape" },
        { "logicalId", graphic.graphic.at( "id" ) },
        { "graphic", std::move( graphic.graphic ) },
        { "typed", true }
    };
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
