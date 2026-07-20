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

#include "design_script_board_graphic_compiler.h"

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
        const std::string from = "board_graphic";
        const size_t position = code.find( from );

        if( position != std::string::npos )
            code.replace( position, from.size(), "board_outline" );

        aDiagnostic["code"] = std::move( code );
    }

    if( aDiagnostic.contains( "message" ) && aDiagnostic["message"].is_string() )
    {
        std::string message = aDiagnostic["message"].get<std::string>();

        for( const std::string from : { "board graphic" } )
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
    DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::RESULT compiled =
            DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::Compile( aDocument, aNode );

    for( JSON& entry : compiled.diagnostics )
    {
        normalizeDiagnostic( entry );
        result.diagnostics.push_back( std::move( entry ) );
    }

    if( !compiled.statement.is_object() || !compiled.statement.contains( "graphic" ) )
    {
        result.ok = false;
        return result;
    }

    JSON graphic = std::move( compiled.statement["graphic"] );

    if( graphic.value( "layers", JSON::array() )
        != JSON::array( { "Edge.Cuts" } ) )
    {
        diagnostic( result, "invalid_authored_board_outline_layer",
                    "every board outline primitive requires exactly (layers Edge.Cuts)" );
    }

    if( graphic.value( "fill", "" ) != "none" )
        diagnostic( result, "invalid_authored_board_outline_fill",
                    "board outline primitives must use (fill none)" );

    if( graphic.value( "stroke", JSON::object() ).value( "style", "" ) != "solid" )
        diagnostic( result, "invalid_authored_board_outline_stroke",
                    "board outline primitives require a solid stroke" );

    if( !graphic.value( "solderMaskMarginNm", JSON( nullptr ) ).is_null() )
        diagnostic( result, "invalid_authored_board_outline_mask_margin",
                    "board outline primitives cannot declare a solder-mask margin" );

    if( !graphic.value( "net", "" ).empty() )
        diagnostic( result, "invalid_authored_board_outline_net",
                    "board outline primitives cannot own an electrical net" );

    result.statement = {
        { "kind", "outline_shape" },
        { "logicalId", graphic.at( "id" ) },
        { "graphic", std::move( graphic ) },
        { "typed", true }
    };
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
