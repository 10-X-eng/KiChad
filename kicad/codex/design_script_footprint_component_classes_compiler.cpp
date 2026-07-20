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

#include "design_script_footprint_component_classes_compiler.h"

#include <set>
#include <string>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_COMPONENT_CLASSES_COMPILER::RESULT;

constexpr size_t MAX_COMPONENT_CLASSES = 256;
constexpr size_t MAX_COMPONENT_CLASS_NAME_BYTES = 256;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool scalar( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( aNode >= aDocument.Nodes().size()
        || aDocument.Nodes()[aNode].kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    aValue = aDocument.AtomText( aNode );
    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_COMPONENT_CLASSES_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_COMPONENT_CLASSES_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> unique;

    if( aDocument.ListHead( aNode ) != "component_classes" || node.children.size() < 2
        || node.children.size() > MAX_COMPONENT_CLASSES + 1 )
    {
        diagnostic( result, "invalid_authored_footprint_component_classes",
                    "component_classes requires 1 through 256 class names" );
        return result;
    }

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string name;

        if( !scalar( aDocument, node.children[index], name ) || name.empty()
            || name.size() > MAX_COMPONENT_CLASS_NAME_BYTES
            || name.find( '\0' ) != std::string::npos
            || name.find_first_of( "\r\n" ) != std::string::npos )
        {
            diagnostic( result, "invalid_authored_footprint_component_class_name",
                        "component class names require 1 through 256 bounded UTF-8 bytes" );
        }
        else if( !unique.emplace( name ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_component_class",
                        "component class " + name + " occurs more than once" );
        }
        else
        {
            result.classes.push_back( name );
        }
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
