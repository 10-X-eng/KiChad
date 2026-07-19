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

#include "design_script_compiler.h"

#include "design_script_board_compiler.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <picosha2.h>
#include <wx/string.h>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;

constexpr size_t MAX_SCRIPT_BYTES = 1024 * 1024;
constexpr size_t MAX_TOP_LEVEL_FORMS = 20000;
constexpr size_t MAX_IDENTIFIER_BYTES = 128;


void diagnostic( KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult, const std::string& aSeverity,
                 const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", aSeverity },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool isScalar( const DOCUMENT& aDocument, size_t aNode )
{
    return aNode < aDocument.Nodes().size()
           && aDocument.Nodes()[aNode].kind != DOCUMENT::NODE_KIND::LIST;
}


bool scalarText( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( !isScalar( aDocument, aNode ) )
        return false;

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool validIdentifier( const std::string& aValue )
{
    if( aValue.empty() || aValue.size() > MAX_IDENTIFIER_BYTES )
        return false;

    return std::all_of( aValue.begin(), aValue.end(),
                        []( unsigned char aCharacter )
                        {
                            return std::isalnum( aCharacter ) || aCharacter == '_'
                                   || aCharacter == '-' || aCharacter == '+'
                                   || aCharacter == '.' || aCharacter == '/'
                                   || aCharacter == '#';
                        } );
}


JSON scalarValue( const DOCUMENT& aDocument, size_t aNode )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    const std::string     value = aDocument.AtomText( aNode );

    if( node.kind == DOCUMENT::NODE_KIND::STRING )
        return value;

    if( value == "true" )
        return true;

    if( value == "false" )
        return false;

    int64_t integer = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    std::from_chars_result converted = std::from_chars( begin, end, integer );

    if( converted.ec == std::errc() && converted.ptr == end )
        return integer;

    return value;
}


JSON expressionToIr( const DOCUMENT& aDocument, size_t aNode )
{
    if( isScalar( aDocument, aNode ) )
        return scalarValue( aDocument, aNode );

    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON                  arguments = JSON::array();

    for( size_t i = 1; i < node.children.size(); ++i )
        arguments.emplace_back( expressionToIr( aDocument, node.children[i] ) );

    return { { "op", aDocument.ListHead( aNode ) }, { "args", std::move( arguments ) } };
}


bool parseSingleValueForm( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], aValue );
}


JSON compileProject( const DOCUMENT& aDocument, size_t aNode,
                     KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_project",
                    "project requires a non-empty name of at most 128 bytes" );
        return JSON::object();
    }

    JSON project = { { "name", name }, { "properties", JSON::object() } };
    const std::set<std::string> allowed = { "title", "company", "revision", "date", "comment" };
    std::set<std::string>       fields;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        std::string       value;

        if( !allowed.contains( head ) || !parseSingleValueForm( aDocument, child, value ) )
        {
            diagnostic( aResult, "error", "invalid_project_field",
                        "project fields must be title, company, revision, date, or comment with "
                        "one scalar value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_project_field",
                        "project field '" + head + "' occurs more than once" );
            continue;
        }

        project["properties"][head] = value;
    }

    return project;
}


JSON compileLibrary( const DOCUMENT& aDocument, size_t aNode,
                     KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           kind;
    std::string           id;

    if( node.children.size() < 3 || !scalarText( aDocument, node.children[1], kind )
        || !scalarText( aDocument, node.children[2], id )
        || ( kind != "symbol" && kind != "footprint" && kind != "model" )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_library",
                    "library requires kind symbol, footprint, or model and a bounded identifier" );
        return JSON::object();
    }

    JSON library = { { "kind", kind }, { "id", id } };
    std::set<std::string> fields;

    for( size_t i = 3; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        std::string       value;

        if( ( head != "uri" && head != "table" )
            || !parseSingleValueForm( aDocument, child, value ) )
        {
            diagnostic( aResult, "error", "invalid_library_field",
                        "library fields must be uri or table with one scalar value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_library_field",
                        "library field '" + head + "' occurs more than once" );
            continue;
        }

        library[head] = value;
    }

    return library;
}


JSON compileComponent( const DOCUMENT& aDocument, size_t aNode,
                       KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_component",
                    "component requires a bounded logical reference" );
        return JSON::object();
    }

    JSON component = { { "reference", reference }, { "properties", JSON::object() } };
    std::set<std::string> singletonFields;
    std::set<std::string> propertyNames;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( head == "property" )
        {
            std::string key;
            std::string value;

            if( field.children.size() != 3
                || !scalarText( aDocument, field.children[1], key )
                || !scalarText( aDocument, field.children[2], value ) || key.empty() )
            {
                diagnostic( aResult, "error", "invalid_component_property",
                            "component property requires a name and value" );
                continue;
            }

            if( !propertyNames.emplace( key ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_property",
                            "component property '" + key + "' occurs more than once" );
                continue;
            }

            component["properties"][key] = value;
            continue;
        }

        if( head == "dnp" )
        {
            std::string value;

            if( !singletonFields.emplace( head ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_field",
                            "component field 'dnp' occurs more than once" );
            }
            else if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "true" && value != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_component_dnp",
                            "component dnp must be true or false" );
            }
            else
            {
                component["dnp"] = value == "true";
            }

            continue;
        }

        if( head != "symbol" && head != "value" && head != "footprint" )
        {
            diagnostic( aResult, "error", "unknown_component_field",
                        "component supports symbol, value, footprint, property, and dnp" );
            continue;
        }

        std::string value;

        if( !parseSingleValueForm( aDocument, child, value ) || value.empty() )
        {
            diagnostic( aResult, "error", "invalid_component_field",
                        "component symbol, value, and footprint require one non-empty value" );
            continue;
        }

        if( !singletonFields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_component_field",
                        "component field '" + head + "' occurs more than once" );
            continue;
        }

        component[head] = value;
    }

    for( const char* required : { "symbol", "value", "footprint" } )
    {
        if( !component.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_component_field",
                        "component " + reference + " is missing " + required );
        }
    }

    return component;
}


JSON compileNet( const DOCUMENT& aDocument, size_t aNode,
                 KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                 std::vector<std::string>& aReferencedComponents,
                 std::set<std::string>& aConnectedPins, size_t& aPinConnections )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_net", "net requires a bounded name" );
        return JSON::object();
    }

    JSON net = { { "name", name }, { "pins", JSON::array() } };

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t child = node.children[i];
        const DOCUMENT::NODE& pin = aDocument.Nodes()[child];
        std::string reference;
        std::string number;

        if( aDocument.ListHead( child ) != "pin" || pin.children.size() != 3
            || !scalarText( aDocument, pin.children[1], reference )
            || !scalarText( aDocument, pin.children[2], number )
            || !validIdentifier( reference ) || number.empty() || number.size() > 64 )
        {
            diagnostic( aResult, "error", "invalid_pin",
                        "net endpoints must use (pin COMPONENT PIN_NUMBER)" );
            continue;
        }

        net["pins"].push_back( { { "component", reference }, { "number", number } } );
        aReferencedComponents.emplace_back( reference );

        const std::string endpoint = reference + ":" + number;

        if( !aConnectedPins.emplace( endpoint ).second )
        {
            diagnostic( aResult, "error", "duplicate_pin_connection",
                        "pin " + endpoint + " is assigned to more than one net endpoint" );
        }

        ++aPinConnections;
    }

    if( net["pins"].size() < 2 )
    {
        diagnostic( aResult, "error", "underspecified_net",
                    "net " + name + " must connect at least two pins" );
    }

    return net;
}


JSON compileSource( const DOCUMENT& aDocument, size_t aNode,
                    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                    std::vector<std::string>& aReferencedComponents )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_source",
                    "source requires a bounded component reference" );
        return JSON::object();
    }

    JSON source = { { "component", reference } };
    const std::set<std::string> allowed = { "manufacturer", "mpn", "supplier", "sku",
                                             "lifecycle", "quantity", "unit_price" };
    std::set<std::string> fields;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( !allowed.contains( head ) || field.children.size() != 2
            || !isScalar( aDocument, field.children[1] ) )
        {
            diagnostic( aResult, "error", "invalid_source_field",
                        "source fields must be manufacturer, mpn, supplier, sku, lifecycle, "
                        "quantity, or unit_price with one value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_source_field",
                        "source field '" + head + "' occurs more than once" );
            continue;
        }

        source[head] = scalarValue( aDocument, field.children[1] );
    }

    if( !source.contains( "manufacturer" ) || !source.contains( "mpn" ) )
    {
        diagnostic( aResult, "warning", "incomplete_source",
                    "source for " + reference + " has no verified manufacturer and MPN pair" );
    }

    aReferencedComponents.emplace_back( reference );
    return source;
}


JSON compileNamedFacet( const DOCUMENT& aDocument, size_t aNode, const std::string& aFacet,
                        KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || !validIdentifier( name ) )
    {
        diagnostic( aResult, "error", "invalid_" + aFacet,
                    aFacet + " requires a bounded logical name" );
        return JSON::object();
    }

    JSON statements = JSON::array();

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        if( aDocument.ListHead( node.children[i] ).empty() )
        {
            diagnostic( aResult, "error", "invalid_" + aFacet + "_statement",
                        aFacet + " payloads must contain named expressions" );
            continue;
        }

        statements.emplace_back( expressionToIr( aDocument, node.children[i] ) );
    }

    return { { "name", name }, { "statements", std::move( statements ) } };
}


JSON compileEnumeratedFacet( const DOCUMENT& aDocument, size_t aNode, const std::string& aFacet,
                             const std::set<std::string>& aAllowed,
                             KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    std::string kind;

    if( !parseSingleValueForm( aDocument, aNode, kind ) || !aAllowed.contains( kind ) )
    {
        diagnostic( aResult, "error", "invalid_" + aFacet,
                    aFacet + " must contain exactly one supported KDS version 1 kind" );
        return JSON::object();
    }

    return { { "kind", kind } };
}


bool containsError( const JSON& aDiagnostics )
{
    return std::any_of( aDiagnostics.begin(), aDiagnostics.end(),
                        []( const JSON& aDiagnostic )
                        {
                            return aDiagnostic.value( "severity", "error" ) == "error";
                        } );
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_COMPILER::JSON DESIGN_SCRIPT_COMPILER::Describe()
{
    return {
        { "language", "kichad-design" },
        { "version", LANGUAGE_VERSION },
        { "syntax", "s-expression" },
        { "root", "kichad_design" },
        { "deterministic", true },
        { "hostCodeExecution", false },
        { "topLevelForms",
          JSON::array( {
                  { { "form", "(version 1)" }, { "required", true } },
                  { { "form", "(project NAME (title TEXT) (company TEXT) (revision TEXT))" },
                    { "required", true } },
                  { { "form", "(units mm|mil)" }, { "default", "mm" } },
                  { { "form", "(library symbol|footprint|model ID (uri URI))" } },
                  { { "form",
                      "(component REF (symbol LIB:ID) (value VALUE) (footprint LIB:ID) "
                      "(property NAME VALUE) (dnp true|false))" } },
                  { { "form", "(net NAME (pin REF NUMBER) (pin REF NUMBER) ...)" } },
                  { { "form", "(sheet NAME ...)" } },
                  { { "form",
                      "(board (stackup ...) (outline (rect (id ID) (at X Y) (size W H))) "
                      "(place REF (at X Y) ...) (route NET (id ID) (from X Y) (to X Y) ...) "
                      "(via NET (id ID) (at X Y) ...) (zone NET ...) (text ...) (dimension ...) "
                      "(keepout ...))" } },
                  { { "form",
                      "(zone NET (id ID) (layers F.Cu ...) "
                      "(outline (polygon (point X Y) ... (hole (point X Y) ...))) "
                      "(clearance D) (min_thickness D) "
                      "(connection none|solid|thermal|pth_thermal "
                      "(thermal_gap D) (thermal_spoke_width D)) "
                      "(islands remove_all|keep_all|remove_below ...) "
                      "(fill solid|hatched ...) "
                      "(hatch_offsets (layer F.Cu X Y) ...))" } },
                  { { "form",
                      "(keepout (id ID) (layers F.Cu ...) "
                      "(outline (polygon (point X Y) ... (hole (point X Y) ...))) "
                      "(prohibit (copper BOOL) (vias BOOL) (tracks BOOL) "
                      "(pads BOOL) (footprints BOOL)))" } },
                  { { "form",
                      "(text VALUE (id ID) (layer LAYER) (at X Y) (size W H) "
                      "(stroke D) (angle A) (justify HORIZONTAL VERTICAL) "
                      "(font stroke|NAME) ...)" } },
                  { { "form",
                      "(dimension aligned|orthogonal|radial|leader|center (id ID) "
                      "(layer LAYER) STYLE_GEOMETRY (line_width D) (arrow_length D) ...)" } },
                  { { "form", "(rule NAME ...)" } },
                  { { "form",
                      "(source REF (manufacturer NAME) (mpn PART) (supplier NAME) (sku PART) "
                      "(quantity N))" } },
                  { { "form", "(check erc|drc|sourcing|footprints|fabrication)" } },
                  { { "form", "(output gerbers|drill|pick_place|bom|step|pdf)" } }
          } ) },
        { "compilerPasses",
          JSON::array( { "parse", "typecheck", "resolve", "plan", "snapshot", "schematic",
                         "libraries", "pcb", "sourcing", "erc", "drc", "fabrication" } ) },
        { "example",
          "(kichad_design\n"
          "  (version 1)\n"
          "  (project sensor (title \"Sensor Board\"))\n"
          "  (units mm)\n"
          "  (component R1 (symbol \"Device:R\") (value \"10k\")\n"
          "    (footprint \"Resistor_SMD:R_0603_1608Metric\"))\n"
          "  (component LED1 (symbol \"Device:LED\") (value \"GREEN\")\n"
          "    (footprint \"LED_SMD:LED_0603_1608Metric\"))\n"
          "  (net LED_A (pin R1 1) (pin LED1 1))\n"
          "  (check erc)\n"
          "  (check drc)\n"
          "  (output gerbers))" }
    };
}


DESIGN_SCRIPT_COMPILER::RESULT DESIGN_SCRIPT_COMPILER::Compile( const std::string& aSource )
{
    RESULT result;

    if( aSource.empty() || aSource.size() > MAX_SCRIPT_BYTES )
    {
        diagnostic( result, "error", "invalid_source_size",
                    "KiChad Design Script source must contain 1 byte to 1 MiB" );
        return result;
    }

    picosha2::hash256_hex_string( aSource, result.sourceSha256 );

    if( aSource.find( '\0' ) != std::string::npos )
    {
        diagnostic( result, "error", "invalid_encoding",
                    "KiChad Design Script source must not contain embedded NUL bytes" );
        return result;
    }

    const wxString decoded = wxString::FromUTF8( aSource.data(), aSource.size() );
    const wxScopedCharBuffer reencoded = decoded.ToUTF8();

    if( reencoded.length() != aSource.size()
        || std::memcmp( reencoded.data(), aSource.data(), aSource.size() ) != 0 )
    {
        diagnostic( result, "error", "invalid_encoding",
                    "KiChad Design Script source must be valid UTF-8" );
        return result;
    }

    std::string parseError;
    std::unique_ptr<DOCUMENT> document = DOCUMENT::Parse( aSource, &parseError );

    if( !document )
    {
        diagnostic( result, "error", "parse_failed", parseError );
        return result;
    }

    if( document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "kichad_design" )
    {
        diagnostic( result, "error", "invalid_root",
                    "script must contain exactly one kichad_design root expression" );
        return result;
    }

    const DOCUMENT::NODE& root = document->Nodes()[document->Roots().front()];

    if( root.children.size() > MAX_TOP_LEVEL_FORMS + 1 )
    {
        diagnostic( result, "error", "program_too_large",
                    "script contains more than 20000 top-level forms" );
        return result;
    }

    result.ir = {
        { "language", "kichad-design" },
        { "version", LANGUAGE_VERSION },
        { "sourceSha256", result.sourceSha256 },
        { "units", "mm" },
        { "project", JSON::object() },
        { "libraries", JSON::array() },
        { "schematic",
          { { "sheets", JSON::array() }, { "components", JSON::array() },
            { "nets", JSON::array() } } },
        { "pcb", JSON::array() },
        { "rules", JSON::array() },
        { "sourcing", JSON::array() },
        { "checks", JSON::array() },
        { "outputs", JSON::array() }
    };

    bool                     sawVersion = false;
    bool                     sawProject = false;
    bool                     sawUnits = false;
    bool                     sawBoard = false;
    bool                     boardFullyTyped = true;
    std::set<std::string>    componentIds;
    std::set<std::string>    netNames;
    std::set<std::string>    libraryIds;
    std::set<std::string>    sheetIds;
    std::set<std::string>    ruleIds;
    std::set<std::string>    sourceIds;
    std::set<std::string>    checkKinds;
    std::set<std::string>    outputKinds;
    std::set<std::string>    connectedPins;
    std::vector<std::string> referencedComponents;
    std::vector<std::string> referencedNets;
    size_t                   pinConnections = 0;

    for( size_t i = 1; i < root.children.size(); ++i )
    {
        const size_t      formNode = root.children[i];
        const std::string form = document->ListHead( formNode );

        if( form.empty() )
        {
            diagnostic( result, "error", "invalid_top_level_form",
                        "every top-level design form must be a named list" );
            continue;
        }

        if( form == "version" )
        {
            std::string version;

            if( sawVersion || !parseSingleValueForm( *document, formNode, version )
                || version != std::to_string( LANGUAGE_VERSION ) )
            {
                diagnostic( result, "error", "invalid_version",
                            "script requires exactly one (version 1) form" );
            }

            sawVersion = true;
        }
        else if( form == "project" )
        {
            if( sawProject )
                diagnostic( result, "error", "duplicate_project", "project occurs more than once" );

            result.ir["project"] = compileProject( *document, formNode, result );
            sawProject = true;
        }
        else if( form == "units" )
        {
            std::string units;

            if( sawUnits || !parseSingleValueForm( *document, formNode, units )
                || ( units != "mm" && units != "mil" ) )
            {
                diagnostic( result, "error", "invalid_units",
                            "units may occur once and must be mm or mil" );
            }
            else
            {
                result.ir["units"] = units;
            }

            sawUnits = true;
        }
        else if( form == "library" )
        {
            JSON library = compileLibrary( *document, formNode, result );
            const std::string key = library.value( "kind", "" ) + ":"
                                    + library.value( "id", "" );

            if( key != ":" && !libraryIds.emplace( key ).second )
            {
                diagnostic( result, "error", "duplicate_library",
                            "library " + key + " occurs more than once" );
            }

            result.ir["libraries"].emplace_back( std::move( library ) );
        }
        else if( form == "component" )
        {
            JSON component = compileComponent( *document, formNode, result );
            const std::string reference = component.value( "reference", "" );

            if( !reference.empty() && !componentIds.emplace( reference ).second )
            {
                diagnostic( result, "error", "duplicate_component",
                            "component " + reference + " occurs more than once" );
            }

            result.ir["schematic"]["components"].emplace_back( std::move( component ) );
        }
        else if( form == "net" )
        {
            JSON net = compileNet( *document, formNode, result, referencedComponents,
                                   connectedPins, pinConnections );
            const std::string name = net.value( "name", "" );

            if( !name.empty() && !netNames.emplace( name ).second )
            {
                diagnostic( result, "error", "duplicate_net",
                            "net " + name + " occurs more than once" );
            }

            result.ir["schematic"]["nets"].emplace_back( std::move( net ) );
        }
        else if( form == "sheet" )
        {
            JSON sheet = compileNamedFacet( *document, formNode, "sheet", result );
            const std::string name = sheet.value( "name", "" );

            if( !name.empty() && !sheetIds.emplace( name ).second )
            {
                diagnostic( result, "error", "duplicate_sheet",
                            "sheet " + name + " occurs more than once" );
            }

            result.ir["schematic"]["sheets"].emplace_back( std::move( sheet ) );
        }
        else if( form == "board" )
        {
            if( sawBoard )
                diagnostic( result, "error", "duplicate_board", "board occurs more than once" );

            KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::RESULT board =
                    KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::Compile( *document, formNode );

            for( JSON& statement : board.statements )
                result.ir["pcb"].emplace_back( std::move( statement ) );

            for( JSON& boardDiagnostic : board.diagnostics )
                result.diagnostics.emplace_back( std::move( boardDiagnostic ) );

            referencedComponents.insert( referencedComponents.end(),
                                         board.componentReferences.begin(),
                                         board.componentReferences.end() );
            referencedNets.insert( referencedNets.end(), board.netReferences.begin(),
                                   board.netReferences.end() );
            boardFullyTyped = boardFullyTyped && board.fullyTyped;

            sawBoard = true;
        }
        else if( form == "rule" )
        {
            JSON rule = compileNamedFacet( *document, formNode, "rule", result );
            const std::string name = rule.value( "name", "" );

            if( !name.empty() && !ruleIds.emplace( name ).second )
            {
                diagnostic( result, "error", "duplicate_rule",
                            "rule " + name + " occurs more than once" );
            }

            result.ir["rules"].emplace_back( std::move( rule ) );
        }
        else if( form == "source" )
        {
            JSON source = compileSource( *document, formNode, result, referencedComponents );
            const std::string reference = source.value( "component", "" );

            if( !reference.empty() && !sourceIds.emplace( reference ).second )
            {
                diagnostic( result, "error", "duplicate_source",
                            "source for " + reference + " occurs more than once" );
            }

            result.ir["sourcing"].emplace_back( std::move( source ) );
        }
        else if( form == "check" )
        {
            static const std::set<std::string> allowed = {
                "erc", "drc", "sourcing", "footprints", "fabrication"
            };
            JSON check = compileEnumeratedFacet( *document, formNode, "check", allowed, result );
            const std::string kind = check.value( "kind", "" );

            if( !kind.empty() && !checkKinds.emplace( kind ).second )
            {
                diagnostic( result, "error", "duplicate_check",
                            "check " + kind + " occurs more than once" );
            }

            result.ir["checks"].emplace_back( std::move( check ) );
        }
        else if( form == "output" )
        {
            static const std::set<std::string> allowed = {
                "gerbers", "drill", "pick_place", "bom", "step", "pdf"
            };
            JSON output = compileEnumeratedFacet( *document, formNode, "output", allowed, result );
            const std::string kind = output.value( "kind", "" );

            if( !kind.empty() && !outputKinds.emplace( kind ).second )
            {
                diagnostic( result, "error", "duplicate_output",
                            "output " + kind + " occurs more than once" );
            }

            result.ir["outputs"].emplace_back( std::move( output ) );
        }
        else
        {
            diagnostic( result, "error", "unknown_top_level_form",
                        "unknown top-level form '" + form + "'" );
        }
    }

    if( !sawVersion )
        diagnostic( result, "error", "missing_version", "script is missing (version 1)" );

    if( !sawProject )
        diagnostic( result, "error", "missing_project", "script is missing project metadata" );

    for( const std::string& reference : referencedComponents )
    {
        if( !componentIds.contains( reference ) )
        {
            diagnostic( result, "error", "unresolved_component",
                        "component reference " + reference + " is not declared" );
        }
    }

    for( const std::string& name : referencedNets )
    {
        if( !netNames.contains( name ) )
        {
            diagnostic( result, "error", "unresolved_net",
                        "net reference " + name + " is not declared" );
        }
    }

    JSON passes = JSON::array( { "parse", "typecheck", "resolve", "plan", "snapshot" } );

    if( !result.ir["libraries"].empty() )
        passes.emplace_back( "libraries" );

    if( !result.ir["schematic"]["components"].empty() || !result.ir["schematic"]["nets"].empty() )
        passes.emplace_back( "schematic" );

    if( !result.ir["pcb"].empty() )
        passes.emplace_back( "pcb" );

    if( !result.ir["sourcing"].empty() )
        passes.emplace_back( "sourcing" );

    if( !result.ir["checks"].empty() )
        passes.emplace_back( "verification" );

    if( !result.ir["outputs"].empty() )
        passes.emplace_back( "fabrication" );

    result.plan = {
        { "passes", std::move( passes ) },
        { "mutationRequired", true },
        { "transactional", true },
        { "boardFullyTyped", boardFullyTyped },
        { "counts",
          { { "libraries", result.ir["libraries"].size() },
            { "sheets", result.ir["schematic"]["sheets"].size() },
            { "components", result.ir["schematic"]["components"].size() },
            { "nets", result.ir["schematic"]["nets"].size() },
            { "pinConnections", pinConnections },
            { "boardStatements", result.ir["pcb"].size() },
            { "rules", result.ir["rules"].size() },
            { "sourcingRecords", result.ir["sourcing"].size() },
            { "checks", result.ir["checks"].size() },
            { "outputs", result.ir["outputs"].size() } } }
    };

    result.ok = !containsError( result.diagnostics );
    return result;
}

} // namespace KICHAD
