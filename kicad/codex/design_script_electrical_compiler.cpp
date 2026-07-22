/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "design_script_electrical_compiler.h"

#include "design_script_simulation_compiler.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using RESULT = KICHAD::DESIGN_SCRIPT_ELECTRICAL_COMPILER::RESULT;

constexpr size_t MAX_ELECTRICAL_ITEMS = 4096;
constexpr int64_t ONE_MILLION = 1'000'000;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" }, { "code", aCode },
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


bool identifier( const std::string& aValue )
{
    return !aValue.empty() && aValue.size() <= 128
           && std::all_of( aValue.begin(), aValue.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isalnum( aCharacter ) || aCharacter == '_'
                                      || aCharacter == '-' || aCharacter == '+'
                                      || aCharacter == '.' || aCharacter == '/'
                                      || aCharacter == '#';
                           } );
}


bool numberWithUnit( const std::string& aText,
                     const std::map<std::string_view, long double>& aScales,
                     int64_t& aValue, bool aAllowNegative = false )
{
    double value = 0.0;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = std::from_chars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr == begin || !std::isfinite( value )
        || ( !aAllowNegative && value < 0.0 ) )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( end - converted.ptr ) );
    const auto scale = aScales.find( unit );

    if( scale == aScales.end() )
        return false;

    const long double scaled = static_cast<long double>( value ) * scale->second;

    if( !std::isfinite( scaled )
        || scaled < static_cast<long double>( std::numeric_limits<int64_t>::min() )
        || scaled > static_cast<long double>( std::numeric_limits<int64_t>::max() ) )
    {
        return false;
    }

    aValue = static_cast<int64_t>( std::llround( scaled ) );
    return true;
}


bool voltage( const std::string& aText, int64_t& aNanovolts )
{
    static const std::map<std::string_view, long double> scales = {
        { "V", 1'000'000'000.0L }, { "mV", 1'000'000.0L },
        { "uV", 1'000.0L }, { "nV", 1.0L }
    };
    return numberWithUnit( aText, scales, aNanovolts, true );
}


bool current( const std::string& aText, int64_t& aNanoamps )
{
    static const std::map<std::string_view, long double> scales = {
        { "A", 1'000'000'000.0L }, { "mA", 1'000'000.0L },
        { "uA", 1'000.0L }, { "nA", 1.0L }
    };
    return numberWithUnit( aText, scales, aNanoamps );
}


bool power( const std::string& aText, int64_t& aNanowatts )
{
    static const std::map<std::string_view, long double> scales = {
        { "W", 1'000'000'000.0L }, { "mW", 1'000'000.0L },
        { "uW", 1'000.0L }, { "nW", 1.0L }
    };
    return numberWithUnit( aText, scales, aNanowatts );
}


bool temperature( const std::string& aText, int64_t& aMicroCelsius )
{
    static const std::map<std::string_view, long double> scales = {
        { "C", 1'000'000.0L }, { "mC", 1'000.0L }
    };
    return numberWithUnit( aText, scales, aMicroCelsius, true );
}


bool thermalResistance( const std::string& aText, int64_t& aMicroCelsiusPerWatt )
{
    static const std::map<std::string_view, long double> scales = {
        { "C/W", 1'000'000.0L }, { "mC/W", 1'000.0L }
    };
    return numberWithUnit( aText, scales, aMicroCelsiusPerWatt );
}


bool percent( const std::string& aText, int64_t& aPartsPerMillion )
{
    static const std::map<std::string_view, long double> scales = {
        { "%", 10'000.0L }, { "ppm", 1.0L }
    };

    return numberWithUnit( aText, scales, aPartsPerMillion )
           && aPartsPerMillion >= 0 && aPartsPerMillion <= ONE_MILLION;
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool endpoint( const DOCUMENT& aDocument, size_t aNode, JSON& aEndpoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string reference;
    std::string unitText;
    std::string pin;
    int64_t unit = 0;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 4
        || !scalar( aDocument, node.children[1], reference ) || !identifier( reference )
        || !scalar( aDocument, node.children[2], unitText )
        || !scalar( aDocument, node.children[3], pin ) || !identifier( pin ) )
    {
        return false;
    }

    const std::from_chars_result parsed =
            std::from_chars( unitText.data(), unitText.data() + unitText.size(), unit );

    if( parsed.ec != std::errc() || parsed.ptr != unitText.data() + unitText.size()
        || unit < 1 || unit > 1024 )
    {
        return false;
    }

    aEndpoint = { { "component", reference }, { "unit", unit }, { "pin", pin } };
    return true;
}


JSON compileRail( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_electrical_rail", "rail requires a bounded identifier" );
        return JSON::object();
    }

    JSON rail = { { "id", id }, { "loads", JSON::array() },
                  { "reservePpm", 0 } };
    std::set<std::string> fields;
    std::set<std::string> loads;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( head == "load" )
        {
            std::string component;
            std::string currentText;
            int64_t currentNa = 0;

            if( field.children.size() != 3
                || !scalar( aDocument, field.children[1], component )
                || !identifier( component )
                || !scalar( aDocument, field.children[2], currentText )
                || !current( currentText, currentNa ) || currentNa <= 0 )
            {
                diagnostic( aResult, "invalid_electrical_load",
                            "rail load requires COMPONENT and positive CURRENT" );
                continue;
            }

            if( !loads.emplace( component ).second )
            {
                diagnostic( aResult, "duplicate_electrical_load",
                            "rail " + id + " repeats load " + component );
                continue;
            }

            rail["loads"].push_back( { { "component", component },
                                        { "currentNa", currentNa } } );
            aResult.referencedComponents.push_back( component );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_electrical_rail_field",
                        "rail " + id + " repeats field " + head );
            continue;
        }

        if( head == "net" )
        {
            if( !oneValue( aDocument, child, value ) || !identifier( value ) )
            {
                diagnostic( aResult, "invalid_electrical_rail_net",
                            "rail net requires one declared net name" );
            }
            else
            {
                rail["net"] = value;
                aResult.referencedNets.push_back( value );
            }
        }
        else if( head == "voltage" )
        {
            std::string minimumText;
            std::string nominalText;
            std::string maximumText;
            int64_t minimum = 0;
            int64_t nominal = 0;
            int64_t maximum = 0;

            if( field.children.size() != 4
                || !scalar( aDocument, field.children[1], minimumText )
                || !scalar( aDocument, field.children[2], nominalText )
                || !scalar( aDocument, field.children[3], maximumText )
                || !voltage( minimumText, minimum ) || !voltage( nominalText, nominal )
                || !voltage( maximumText, maximum ) || minimum > nominal
                || nominal > maximum )
            {
                diagnostic( aResult, "invalid_electrical_rail_voltage",
                            "rail voltage requires ordered MIN NOMINAL MAX voltages" );
            }
            else
            {
                rail["voltage"] = { { "minimumNv", minimum },
                                      { "nominalNv", nominal },
                                      { "maximumNv", maximum } };
            }
        }
        else if( head == "source_current" )
        {
            int64_t currentNa = 0;

            if( !oneValue( aDocument, child, value ) || !current( value, currentNa )
                || currentNa <= 0 )
            {
                diagnostic( aResult, "invalid_electrical_source_current",
                            "rail source_current requires a positive current" );
            }
            else
            {
                rail["sourceCurrentNa"] = currentNa;
            }
        }
        else if( head == "reserve" )
        {
            int64_t reserve = 0;

            if( !oneValue( aDocument, child, value ) || !percent( value, reserve ) )
            {
                diagnostic( aResult, "invalid_electrical_reserve",
                            "rail reserve requires 0% through 100%" );
            }
            else
            {
                rail["reservePpm"] = reserve;
            }
        }
        else
        {
            diagnostic( aResult, "unknown_electrical_rail_field",
                        "rail " + id + " does not support field " + head );
        }
    }

    for( const char* required : { "net", "voltage", "sourceCurrentNa" } )
    {
        if( !rail.contains( required ) )
            diagnostic( aResult, "missing_electrical_rail_field",
                        "rail " + id + " requires " + required );
    }

    if( rail["loads"].empty() )
        diagnostic( aResult, "missing_electrical_load",
                    "rail " + id + " requires at least one load" );

    return rail;
}


JSON compileRating( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string component;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], component )
        || !identifier( component ) )
    {
        diagnostic( aResult, "invalid_electrical_rating",
                    "rating requires a component reference" );
        return JSON::object();
    }

    JSON rating = { { "component", component }, { "deratingPpm", ONE_MILLION } };
    std::set<std::string> fields;
    const std::map<std::string, std::pair<const char*, const char*>> quantities = {
        { "operating_voltage", { "operatingVoltageNv", "voltage" } },
        { "rated_voltage", { "ratedVoltageNv", "voltage" } },
        { "operating_current", { "operatingCurrentNa", "current" } },
        { "rated_current", { "ratedCurrentNa", "current" } },
        { "operating_power", { "operatingPowerNw", "power" } },
        { "rated_power", { "ratedPowerNw", "power" } }
    };

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_electrical_rating_field",
                        "rating " + component + " repeats field " + head );
            continue;
        }

        if( head == "derating" )
        {
            int64_t derating = 0;

            if( !oneValue( aDocument, child, value ) || !percent( value, derating )
                || derating <= 0 )
            {
                diagnostic( aResult, "invalid_electrical_derating",
                            "rating derating requires more than 0% through 100%" );
            }
            else
            {
                rating["deratingPpm"] = derating;
            }

            continue;
        }

        const auto quantity = quantities.find( head );
        int64_t parsed = 0;
        bool valid = quantity != quantities.end() && oneValue( aDocument, child, value );

        if( valid && quantity->second.second == std::string( "voltage" ) )
            valid = voltage( value, parsed );
        else if( valid && quantity->second.second == std::string( "current" ) )
            valid = current( value, parsed );
        else if( valid && quantity->second.second == std::string( "power" ) )
            valid = power( value, parsed );

        if( !valid || parsed < 0 )
        {
            diagnostic( aResult, "invalid_electrical_rating_field",
                        "rating " + component + " has invalid field " + head );
        }
        else
        {
            rating[quantity->second.first] = parsed;
        }
    }

    bool hasPair = false;

    for( const auto& [operating, rated] :
         { std::pair{ "operatingVoltageNv", "ratedVoltageNv" },
           std::pair{ "operatingCurrentNa", "ratedCurrentNa" },
           std::pair{ "operatingPowerNw", "ratedPowerNw" } } )
    {
        if( rating.contains( operating ) != rating.contains( rated ) )
        {
            diagnostic( aResult, "incomplete_electrical_rating_pair",
                        "rating " + component + " requires both " + operating + " and " + rated );
        }
        else if( rating.contains( operating ) )
        {
            hasPair = true;

            if( rating[rated].get<int64_t>() <= 0 )
                diagnostic( aResult, "invalid_electrical_rating_limit",
                            "rating " + component + " requires positive rated limits" );
        }
    }

    if( !hasPair )
        diagnostic( aResult, "empty_electrical_rating",
                    "rating " + component + " requires at least one operating/rated pair" );

    aResult.referencedComponents.push_back( component );
    return rating;
}


JSON compileThermal( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string component;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], component )
        || !identifier( component ) )
    {
        diagnostic( aResult, "invalid_electrical_thermal",
                    "thermal requires a component reference" );
        return JSON::object();
    }

    JSON thermal = { { "component", component } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;
        int64_t parsed = 0;

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_electrical_thermal_field",
                        "thermal " + component + " repeats field " + head );
            continue;
        }

        bool valid = oneValue( aDocument, child, value );

        if( valid && head == "dissipation" )
        {
            valid = power( value, parsed );

            if( valid )
                thermal["dissipationNw"] = parsed;
        }
        else if( valid && head == "theta_ja" )
        {
            valid = thermalResistance( value, parsed );

            if( valid )
                thermal["thetaJaMicroCPerW"] = parsed;
        }
        else if( valid && head == "ambient" )
        {
            valid = temperature( value, parsed );

            if( valid )
                thermal["ambientMicroC"] = parsed;
        }
        else if( valid && head == "maximum_junction" )
        {
            valid = temperature( value, parsed );

            if( valid )
                thermal["maximumJunctionMicroC"] = parsed;
        }
        else
        {
            valid = false;
        }

        if( !valid )
            diagnostic( aResult, "invalid_electrical_thermal_field",
                        "thermal " + component + " has invalid field " + head );
    }

    for( const char* required : { "dissipationNw", "thetaJaMicroCPerW", "ambientMicroC",
                                  "maximumJunctionMicroC" } )
    {
        if( !thermal.contains( required ) )
            diagnostic( aResult, "missing_electrical_thermal_field",
                        "thermal " + component + " requires " + required );
    }

    aResult.referencedComponents.push_back( component );
    return thermal;
}


JSON compileLogic( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_electrical_logic",
                    "logic requires a bounded identifier" );
        return JSON::object();
    }

    JSON logic = { { "id", id }, { "receivers", JSON::array() } };
    std::set<std::string> fields;
    const std::map<std::string, const char*> voltageFields = {
        { "output_low", "outputLowNv" }, { "output_high", "outputHighNv" },
        { "input_low", "inputLowNv" }, { "input_high", "inputHighNv" },
        { "signal_min", "signalMinimumNv" }, { "signal_max", "signalMaximumNv" },
        { "absolute_min", "absoluteMinimumNv" },
        { "absolute_max", "absoluteMaximumNv" }
    };

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "driver" || head == "receiver" )
        {
            JSON parsed;

            if( !endpoint( aDocument, child, parsed ) )
            {
                diagnostic( aResult, "invalid_electrical_logic_endpoint",
                            "logic endpoint requires COMPONENT UNIT PIN" );
                continue;
            }

            const std::string component = parsed["component"].get<std::string>();
            aResult.referencedComponents.push_back( component );

            if( head == "driver" )
            {
                if( logic.contains( "driver" ) )
                    diagnostic( aResult, "duplicate_electrical_logic_driver",
                                "logic " + id + " has more than one driver" );
                else
                    logic["driver"] = std::move( parsed );
            }
            else
            {
                logic["receivers"].push_back( std::move( parsed ) );
            }

            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_electrical_logic_field",
                        "logic " + id + " repeats field " + head );
            continue;
        }

        std::string value;

        if( head == "net" )
        {
            if( !oneValue( aDocument, child, value ) || !identifier( value ) )
            {
                diagnostic( aResult, "invalid_electrical_logic_net",
                            "logic " + id + " net requires one declared net name" );
            }
            else
            {
                logic["net"] = value;
                aResult.referencedNets.push_back( value );
            }

            continue;
        }

        const auto field = voltageFields.find( head );
        int64_t parsed = 0;

        if( field == voltageFields.end() || !oneValue( aDocument, child, value )
            || !voltage( value, parsed ) )
        {
            diagnostic( aResult, "invalid_electrical_logic_field",
                        "logic " + id + " has invalid field " + head );
        }
        else
        {
            logic[field->second] = parsed;
        }
    }

    for( const char* required : { "net", "driver", "outputLowNv", "outputHighNv", "inputLowNv",
                                  "inputHighNv", "signalMinimumNv", "signalMaximumNv" } )
    {
        if( !logic.contains( required ) )
            diagnostic( aResult, "missing_electrical_logic_field",
                        "logic " + id + " requires " + required );
    }

    if( logic["receivers"].empty() )
        diagnostic( aResult, "missing_electrical_logic_receiver",
                    "logic " + id + " requires at least one receiver" );

    if( logic.contains( "absoluteMinimumNv" ) != logic.contains( "absoluteMaximumNv" ) )
        diagnostic( aResult, "incomplete_electrical_logic_absolute_range",
                    "logic " + id + " requires both absolute_min and absolute_max" );

    return logic;
}

} // namespace


KICHAD::DESIGN_SCRIPT_ELECTRICAL_COMPILER::RESULT
KICHAD::DESIGN_SCRIPT_ELECTRICAL_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    result.electrical = { { "rails", JSON::array() }, { "ratings", JSON::array() },
                          { "thermal", JSON::array() }, { "logic", JSON::array() },
                          { "simulations", JSON::array() } };
    const LOSSLESS_SEXPR_DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( node.kind != LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
        || aDocument.ListHead( aNode ) != "electrical" )
    {
        diagnostic( result, "invalid_electrical", "electrical must be a named list" );
        return result;
    }

    if( node.children.size() > MAX_ELECTRICAL_ITEMS + 1 )
    {
        diagnostic( result, "electrical_too_large",
                    "electrical may contain at most 4096 declarations" );
        return result;
    }

    std::map<std::string, std::set<std::string>> ids;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        JSON compiled;
        std::string collection;
        std::string idField;

        if( head == "rail" )
        {
            compiled = compileRail( aDocument, child, result );
            collection = "rails";
            idField = "id";
        }
        else if( head == "rating" )
        {
            compiled = compileRating( aDocument, child, result );
            collection = "ratings";
            idField = "component";
        }
        else if( head == "thermal" )
        {
            compiled = compileThermal( aDocument, child, result );
            collection = "thermal";
            idField = "component";
        }
        else if( head == "logic" )
        {
            compiled = compileLogic( aDocument, child, result );
            collection = "logic";
            idField = "id";
        }
        else if( head == "simulation" )
        {
            KICHAD::DESIGN_SCRIPT_SIMULATION_COMPILER::RESULT simulation =
                    KICHAD::DESIGN_SCRIPT_SIMULATION_COMPILER::Compile( aDocument, child );

            for( JSON& entry : simulation.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.referencedComponents.insert( result.referencedComponents.end(),
                                                simulation.referencedComponents.begin(),
                                                simulation.referencedComponents.end() );
            result.referencedNets.insert( result.referencedNets.end(),
                                         simulation.referencedNets.begin(),
                                         simulation.referencedNets.end() );
            compiled = std::move( simulation.simulation );
            collection = "simulations";
            idField = "id";
        }
        else
        {
            diagnostic( result, "unknown_electrical_declaration",
                        "electrical does not support declaration " + head );
            continue;
        }

        const std::string id = compiled.value( idField, "" );

        if( !id.empty() && !ids[collection].emplace( id ).second )
        {
            diagnostic( result, "duplicate_electrical_declaration",
                        "electrical " + head + " " + id + " occurs more than once" );
        }

        result.electrical[collection].push_back( std::move( compiled ) );
    }

    if( node.children.size() == 1 )
        diagnostic( result, "empty_electrical", "electrical requires at least one declaration" );

    return result;
}
