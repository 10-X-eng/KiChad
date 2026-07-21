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

#include "design_script_assembly_compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <set>
#include <string>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_ASSEMBLY_COMPILER::RESULT;

constexpr size_t MAX_INSTRUCTIONS = 128;


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


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool identifier( const std::string& aValue )
{
    if( aValue.empty() || aValue.size() > 128 )
        return false;

    return std::all_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
    {
        return std::isalnum( aCharacter ) || aCharacter == '_' || aCharacter == '-'
               || aCharacter == '.' || aCharacter == '+';
    } );
}


bool boundedText( const std::string& aValue, size_t aMaximum )
{
    return !aValue.empty() && aValue.size() <= aMaximum
           && std::none_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
           {
               return aCharacter == 0 || aCharacter == 0x7F
                      || ( aCharacter < 0x20 && aCharacter != '\t' && aCharacter != '\n' );
           } );
}


bool unsignedInteger( const std::string& aText, uint64_t aMinimum, uint64_t aMaximum,
                      uint64_t& aValue )
{
    if( aText.empty() )
        return false;

    const char* begin = aText.data();
    const char* end = begin + aText.size();
    uint64_t value = 0;
    const auto converted = std::from_chars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || value < aMinimum
        || value > aMaximum )
    {
        return false;
    }

    aValue = value;
    return true;
}


JSON compileInstruction( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_assembly_instruction_id",
                    "assembly instruction requires one unique bounded ID" );
        return JSON::object();
    }

    JSON instruction = { { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_assembly_instruction_field",
                        "assembly instruction " + id + " field " + head
                                + " occurs more than once" );
            continue;
        }

        if( head == "scope" )
        {
            std::string scope;

            if( field.children.size() < 2 || field.children.size() > 3
                || !scalar( aDocument, field.children[1], scope ) )
            {
                diagnostic( aResult, "invalid_assembly_instruction_scope",
                            "assembly instruction scope must be board or component REF" );
            }
            else if( scope == "board" && field.children.size() == 2 )
            {
                instruction["scope"] = { { "kind", "board" } };
            }
            else if( scope == "component" && field.children.size() == 3 )
            {
                std::string reference;

                if( !scalar( aDocument, field.children[2], reference )
                    || !identifier( reference ) )
                {
                    diagnostic( aResult, "invalid_assembly_instruction_scope",
                                "component assembly scope requires one component reference" );
                }
                else
                {
                    instruction["scope"] = { { "kind", "component" },
                                               { "component", reference } };
                    aResult.referencedComponents.emplace_back( reference );
                }
            }
            else
            {
                diagnostic( aResult, "invalid_assembly_instruction_scope",
                            "assembly instruction scope must be board or component REF" );
            }
        }
        else if( head == "text" )
        {
            std::string text;

            if( !oneValue( aDocument, child, text ) || !boundedText( text, 2048 ) )
                diagnostic( aResult, "invalid_assembly_instruction_text",
                            "assembly instruction text must be non-empty bounded text" );
            else
                instruction["text"] = text;
        }
        else
        {
            diagnostic( aResult, "unknown_assembly_instruction_field",
                        "assembly instruction supports scope and text" );
        }
    }

    for( const char* required : { "scope", "text" } )
    {
        if( !instruction.contains( required ) )
            diagnostic( aResult, "missing_assembly_instruction_field",
                        "assembly instruction " + id + " is missing " + required );
    }

    return instruction;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_ASSEMBLY_COMPILER::RESULT DESIGN_SCRIPT_ASSEMBLY_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;

    if( aDocument.ListHead( aNode ) != "assembly" )
    {
        diagnostic( result, "invalid_assembly", "assembly must be a named list" );
        return result;
    }

    result.assembly = { { "instructions", JSON::array() } };
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> fields;
    std::set<std::string> instructionIds;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "instruction" )
        {
            if( result.assembly["instructions"].size() >= MAX_INSTRUCTIONS )
            {
                diagnostic( result, "too_many_assembly_instructions",
                            "assembly may declare at most 128 special instructions" );
                continue;
            }

            JSON instruction = compileInstruction( aDocument, child, result );
            const std::string id = instruction.value( "id", "" );

            if( !id.empty() && !instructionIds.emplace( id ).second )
                diagnostic( result, "duplicate_assembly_instruction",
                            "assembly instruction " + id + " occurs more than once" );

            result.assembly["instructions"].push_back( std::move( instruction ) );
            continue;
        }

        std::string value;

        if( !std::set<std::string>{ "acceptance", "process", "solder_alloy", "stencil",
                                    "stencil_thickness_um", "cleaning", "coating" }
                         .contains( head )
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_assembly_field",
                        "assembly contains an unknown or malformed field" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_assembly_field",
                        "assembly field " + head + " occurs more than once" );
            continue;
        }

        if( head == "acceptance" )
        {
            if( !std::set<std::string>{ "ipc-a-610-class-1", "ipc-a-610-class-2",
                                        "ipc-a-610-class-3" }.contains( value ) )
                diagnostic( result, "invalid_assembly_acceptance",
                            "assembly acceptance must select IPC-A-610 class 1, 2, or 3" );
            else
                result.assembly["acceptance"] = value;
        }
        else if( head == "process" )
        {
            if( !std::set<std::string>{ "lead_free_reflow", "leaded_reflow", "hand",
                                        "mixed" }.contains( value ) )
                diagnostic( result, "invalid_assembly_process",
                            "assembly process must be lead_free_reflow, leaded_reflow, hand, or mixed" );
            else
                result.assembly["process"] = value;
        }
        else if( head == "solder_alloy" )
        {
            if( !boundedText( value, 128 ) )
                diagnostic( result, "invalid_assembly_solder_alloy",
                            "assembly solder_alloy must be non-empty bounded text" );
            else
                result.assembly["solderAlloy"] = value;
        }
        else if( head == "stencil" )
        {
            if( !std::set<std::string>{ "none", "top", "bottom", "both" }.contains( value ) )
                diagnostic( result, "invalid_assembly_stencil",
                            "assembly stencil must be none, top, bottom, or both" );
            else
                result.assembly["stencil"] = value;
        }
        else if( head == "stencil_thickness_um" )
        {
            uint64_t thickness = 0;

            if( !unsignedInteger( value, 50, 500, thickness ) )
                diagnostic( result, "invalid_assembly_stencil_thickness",
                            "assembly stencil_thickness_um must be 50 through 500" );
            else
                result.assembly["stencilThicknessUm"] = thickness;
        }
        else if( head == "cleaning" )
        {
            if( !std::set<std::string>{ "no_clean", "aqueous", "solvent" }.contains( value ) )
                diagnostic( result, "invalid_assembly_cleaning",
                            "assembly cleaning must be no_clean, aqueous, or solvent" );
            else
                result.assembly["cleaning"] = value;
        }
        else if( head == "coating" )
        {
            if( !std::set<std::string>{ "none", "acrylic", "silicone", "urethane",
                                        "parylene" }.contains( value ) )
                diagnostic( result, "invalid_assembly_coating",
                            "assembly coating is not a supported production coating" );
            else
                result.assembly["coating"] = value;
        }
    }

    for( const char* required : { "acceptance", "process", "solderAlloy", "stencil",
                                  "cleaning", "coating" } )
    {
        if( !result.assembly.contains( required ) )
            diagnostic( result, "missing_assembly_field",
                        std::string( "assembly is missing " ) + required );
    }

    if( result.assembly.contains( "stencil" ) )
    {
        const bool usesStencil = result.assembly["stencil"].get<std::string>() != "none";

        if( usesStencil != result.assembly.contains( "stencilThicknessUm" ) )
            diagnostic( result, "invalid_assembly_stencil_policy",
                        "stencil_thickness_um is required exactly when a stencil is used" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
