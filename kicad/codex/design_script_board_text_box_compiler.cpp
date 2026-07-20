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

#include "design_script_board_text_box_compiler.h"

#include "design_script_footprint_text_compiler.h"

#include <string>


namespace
{

using JSON = nlohmann::json;


void normalizeDiagnostic( JSON& aDiagnostic )
{
    if( aDiagnostic.contains( "code" ) && aDiagnostic["code"].is_string() )
    {
        std::string code = aDiagnostic["code"].get<std::string>();
        const std::string from = "footprint_text";
        const size_t position = code.find( from );

        if( position != std::string::npos )
            code.replace( position, from.size(), "board_text_box" );

        aDiagnostic["code"] = std::move( code );
    }

    if( aDiagnostic.contains( "message" ) && aDiagnostic["message"].is_string() )
    {
        std::string message = aDiagnostic["message"].get<std::string>();
        size_t position = 0;

        while( ( position = message.find( "footprint text", position ) )
               != std::string::npos )
        {
            message.replace( position, 14, "board text box" );
            position += 14;
        }

        aDiagnostic["message"] = std::move( message );
    }
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_BOARD_TEXT_BOX_COMPILER::RESULT
DESIGN_SCRIPT_BOARD_TEXT_BOX_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    DESIGN_SCRIPT_FOOTPRINT_TEXT_COMPILER::RESULT compiled =
            DESIGN_SCRIPT_FOOTPRINT_TEXT_COMPILER::Compile( aDocument, aNode );

    for( JSON& entry : compiled.diagnostics )
    {
        normalizeDiagnostic( entry );
        result.diagnostics.push_back( std::move( entry ) );
    }

    if( !compiled.text.is_object() || compiled.text.value( "kind", "" ) != "text_box"
        || !compiled.text.contains( "id" ) || !compiled.text["id"].is_string() )
    {
        result.ok = false;
        return result;
    }

    result.statement = {
        { "kind", "board_text_box" },
        { "logicalId", compiled.text.at( "id" ) },
        { "textBox", std::move( compiled.text ) },
        { "typed", true }
    };
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
