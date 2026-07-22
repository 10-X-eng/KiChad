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

#include "design_script_simulation_compiler.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using RESULT = KICHAD::DESIGN_SCRIPT_SIMULATION_COMPILER::RESULT;

constexpr size_t MAX_SIMULATION_ITEMS = 4096;


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


bool spiceParameterName( const std::string& aValue )
{
    return !aValue.empty() && aValue.size() <= 64
           && std::all_of( aValue.begin(), aValue.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isalnum( aCharacter ) || aCharacter == '_';
                           } );
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool quantity( const std::string& aText,
               const std::map<std::string_view, long double>& aScales, double& aSi,
               bool aPositive = false )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result parsed = std::from_chars( begin, end, value );

    if( parsed.ec != std::errc() || parsed.ptr == begin || !std::isfinite( value )
        || ( aPositive && value <= 0.0L ) )
    {
        return false;
    }

    const std::string_view unit( parsed.ptr, static_cast<size_t>( end - parsed.ptr ) );
    const auto scale = aScales.find( unit );

    if( scale == aScales.end() )
        return false;

    const long double converted = value * scale->second;

    if( !std::isfinite( converted )
        || std::abs( converted ) > static_cast<long double>( 1.0e300 ) )
    {
        return false;
    }

    aSi = static_cast<double>( converted );
    return true;
}


bool voltage( const std::string& aText, double& aValue, bool aPositive = false )
{
    static const std::map<std::string_view, long double> scales = {
        { "V", 1.0L }, { "mV", 1.0e-3L }, { "uV", 1.0e-6L }, { "nV", 1.0e-9L }
    };
    return quantity( aText, scales, aValue, aPositive );
}


bool current( const std::string& aText, double& aValue, bool aPositive = false )
{
    static const std::map<std::string_view, long double> scales = {
        { "A", 1.0L }, { "mA", 1.0e-3L }, { "uA", 1.0e-6L }, { "nA", 1.0e-9L }
    };
    return quantity( aText, scales, aValue, aPositive );
}


bool resistance( const std::string& aText, double& aValue )
{
    static const std::map<std::string_view, long double> scales = {
        { "ohm", 1.0L }, { "kohm", 1.0e3L }, { "Mohm", 1.0e6L },
        { "Gohm", 1.0e9L }
    };
    return quantity( aText, scales, aValue, true );
}


bool capacitance( const std::string& aText, double& aValue )
{
    static const std::map<std::string_view, long double> scales = {
        { "F", 1.0L }, { "mF", 1.0e-3L }, { "uF", 1.0e-6L },
        { "nF", 1.0e-9L }, { "pF", 1.0e-12L }
    };
    return quantity( aText, scales, aValue, true );
}


bool inductance( const std::string& aText, double& aValue )
{
    static const std::map<std::string_view, long double> scales = {
        { "H", 1.0L }, { "mH", 1.0e-3L }, { "uH", 1.0e-6L }, { "nH", 1.0e-9L }
    };
    return quantity( aText, scales, aValue, true );
}


bool timeValue( const std::string& aText, double& aValue, bool aAllowZero = false )
{
    static const std::map<std::string_view, long double> scales = {
        { "s", 1.0L }, { "ms", 1.0e-3L }, { "us", 1.0e-6L },
        { "ns", 1.0e-9L }, { "ps", 1.0e-12L }
    };
    return quantity( aText, scales, aValue, !aAllowZero )
           && ( !aAllowZero || aValue >= 0.0 );
}


bool frequency( const std::string& aText, double& aValue )
{
    static const std::map<std::string_view, long double> scales = {
        { "Hz", 1.0L }, { "kHz", 1.0e3L }, { "MHz", 1.0e6L }, { "GHz", 1.0e9L }
    };
    return quantity( aText, scales, aValue, true );
}


bool finiteNumber( const std::string& aText, double& aValue )
{
    const std::from_chars_result parsed =
            std::from_chars( aText.data(), aText.data() + aText.size(), aValue );
    return parsed.ec == std::errc() && parsed.ptr == aText.data() + aText.size()
           && std::isfinite( aValue ) && std::abs( aValue ) <= 1.0e300;
}


bool boundedInteger( const std::string& aText, int64_t aMinimum, int64_t aMaximum,
                     int64_t& aValue )
{
    const std::from_chars_result parsed =
            std::from_chars( aText.data(), aText.data() + aText.size(), aValue );
    return parsed.ec == std::errc() && parsed.ptr == aText.data() + aText.size()
           && aValue >= aMinimum && aValue <= aMaximum;
}


JSON compileDevice( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_simulation_device", "device requires a bounded identifier" );
        return JSON::object();
    }

    JSON device = { { "id", id }, { "nodes", JSON::array() }, { "pins", JSON::array() } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& form = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_simulation_device_field",
                        "device " + id + " repeats field " + head );
            continue;
        }

        std::string value;

        if( head == "nodes" || head == "pins" )
        {
            if( form.children.size() < 3 || form.children.size() > 5 )
            {
                diagnostic( aResult, "invalid_simulation_device_nodes",
                            "device nodes/pins requires two through four identifiers" );
                continue;
            }

            for( size_t nodeIndex = 1; nodeIndex < form.children.size(); ++nodeIndex )
            {
                if( !scalar( aDocument, form.children[nodeIndex], value )
                    || !identifier( value ) )
                {
                    diagnostic( aResult, "invalid_simulation_device_nodes",
                                "device nodes/pins requires bounded identifiers" );
                    continue;
                }

                device[head].push_back( value );

                if( head == "nodes" )
                    aResult.referencedNets.push_back( value );
            }
        }
        else if( head == "unit" )
        {
            int64_t unit = 0;

            if( !oneValue( aDocument, child, value )
                || !boundedInteger( value, 1, 1024, unit ) )
            {
                diagnostic( aResult, "invalid_simulation_device_unit",
                            "device " + id + " unit must be 1 through 1024" );
            }
            else
            {
                device["unit"] = unit;
            }
        }
        else if( head == "component" || head == "kind" || head == "model" )
        {
            if( !oneValue( aDocument, child, value ) || !identifier( value ) )
            {
                diagnostic( aResult, "invalid_simulation_device_field",
                            "device " + id + " has invalid field " + head );
                continue;
            }

            device[head] = value;

            if( head == "component" )
                aResult.referencedComponents.push_back( value );
        }
        else if( head == "value" )
        {
            if( !oneValue( aDocument, child, value ) )
            {
                diagnostic( aResult, "invalid_simulation_device_value",
                            "device value requires one typed quantity" );
                continue;
            }

            device["valueSource"] = value;
        }
        else
        {
            diagnostic( aResult, "unknown_simulation_device_field",
                        "device " + id + " does not support field " + head );
        }
    }

    const std::string kind = device.value( "kind", "" );
    const size_t requiredNodes = kind == "bjt" ? 3 : kind == "mosfet" ? 4 : 2;
    const bool primitive = kind == "resistor" || kind == "capacitor" || kind == "inductor";
    double parsedValue = 0.0;
    bool valueOk = false;

    if( device.contains( "valueSource" ) )
    {
        const std::string source = device["valueSource"].get<std::string>();
        valueOk = kind == "resistor" ? resistance( source, parsedValue )
                : kind == "capacitor" ? capacitance( source, parsedValue )
                : kind == "inductor" ? inductance( source, parsedValue ) : false;
    }

    if( kind != "resistor" && kind != "capacitor" && kind != "inductor"
        && kind != "diode" && kind != "bjt" && kind != "mosfet" )
    {
        diagnostic( aResult, "invalid_simulation_device_kind",
                    "device " + id + " kind must be resistor, capacitor, inductor, diode, bjt, or mosfet" );
    }

    if( device["nodes"].size() != requiredNodes )
        diagnostic( aResult, "invalid_simulation_device_nodes",
                    "device " + id + " has the wrong node count for " + kind );

    if( device["pins"].size() != requiredNodes )
        diagnostic( aResult, "invalid_simulation_device_pins",
                    "device " + id + " has the wrong pin count for " + kind );

    if( primitive )
    {
        if( !valueOk )
            diagnostic( aResult, "invalid_simulation_device_value",
                        "device " + id + " requires a positive value with the correct SI unit" );
        else
            device["valueSi"] = parsedValue;

        if( device.contains( "model" ) )
            diagnostic( aResult, "redundant_simulation_device_model",
                        "primitive device " + id + " cannot name a model" );
    }
    else if( !device.contains( "model" ) )
    {
        diagnostic( aResult, "missing_simulation_device_model",
                    "device " + id + " requires a model" );
    }

    if( !device.contains( "component" ) )
        diagnostic( aResult, "missing_simulation_device_component",
                    "device " + id + " requires its KDS component reference" );

    if( !device.contains( "unit" ) )
        diagnostic( aResult, "missing_simulation_device_unit",
                    "device " + id + " requires its KDS component unit" );

    return device;
}


JSON compileModel( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 3 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_simulation_model", "model requires a bounded identifier" );
        return JSON::object();
    }

    JSON model = { { "id", id }, { "parameters", JSON::object() } };
    bool sawKind = false;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( head == "kind" )
        {
            if( sawKind || !oneValue( aDocument, child, value )
                || ( value != "diode" && value != "npn" && value != "pnp"
                     && value != "nmos" && value != "pmos" ) )
            {
                diagnostic( aResult, "invalid_simulation_model_kind",
                            "model kind must occur once and be diode, npn, pnp, nmos, or pmos" );
            }
            else
            {
                model["kind"] = value;
            }

            sawKind = true;
        }
        else if( head == "parameter" )
        {
            std::string name;
            double parsed = 0.0;

            if( field.children.size() != 3
                || !scalar( aDocument, field.children[1], name )
                || !spiceParameterName( name )
                || !scalar( aDocument, field.children[2], value )
                || !finiteNumber( value, parsed ) || model["parameters"].contains( name ) )
            {
                diagnostic( aResult, "invalid_simulation_model_parameter",
                            "model parameter requires unique NAME and finite numeric VALUE" );
            }
            else
            {
                model["parameters"][name] = parsed;
            }
        }
        else
        {
            diagnostic( aResult, "unknown_simulation_model_field",
                        "model " + id + " does not support field " + head );
        }
    }

    if( !sawKind )
        diagnostic( aResult, "missing_simulation_model_kind", "model " + id + " requires kind" );

    return model;
}


JSON compileSource( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_simulation_source", "simulation source requires an identifier" );
        return JSON::object();
    }

    JSON source = { { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_simulation_source_field",
                        "source " + id + " repeats field " + head );
            continue;
        }

        if( head == "ac" )
        {
            const DOCUMENT::NODE& form = aDocument.Nodes().at( child );
            std::string magnitude;
            std::string phase;

            if( form.children.size() != 3
                || !scalar( aDocument, form.children[1], magnitude )
                || !scalar( aDocument, form.children[2], phase ) )
            {
                diagnostic( aResult, "invalid_simulation_source_ac",
                            "source ac requires MAGNITUDE and PHASE_DEGREES" );
            }
            else
            {
                source["acMagnitudeSource"] = magnitude;
                source["acPhaseSource"] = phase;
            }
        }
        else if( head == "pulse" )
        {
            const DOCUMENT::NODE& form = aDocument.Nodes().at( child );
            JSON values = JSON::array();

            if( form.children.size() != 8 )
            {
                diagnostic( aResult, "invalid_simulation_source_pulse",
                            "source pulse requires INITIAL PULSED DELAY RISE FALL WIDTH PERIOD" );
            }
            else
            {
                bool valid = true;

                for( size_t valueIndex = 1; valueIndex < form.children.size(); ++valueIndex )
                {
                    if( !scalar( aDocument, form.children[valueIndex], value ) )
                        valid = false;

                    values.push_back( value );
                }

                if( valid )
                    source["pulseSource"] = std::move( values );
                else
                    diagnostic( aResult, "invalid_simulation_source_pulse",
                                "source pulse values must be scalar typed quantities" );
            }
        }
        else if( head == "kind" || head == "positive" || head == "negative" || head == "dc" )
        {
            if( !oneValue( aDocument, child, value )
                || ( head != "dc" && !identifier( value ) ) )
            {
                diagnostic( aResult, "invalid_simulation_source_field",
                            "source " + id + " has invalid field " + head );
                continue;
            }

            source[head] = value;

            if( head == "positive" || head == "negative" )
                aResult.referencedNets.push_back( value );
        }
        else
        {
            diagnostic( aResult, "unknown_simulation_source_field",
                        "source " + id + " does not support field " + head );
        }
    }

    const std::string kind = source.value( "kind", "" );
    double dc = 0.0;

    if( kind != "voltage" && kind != "current" )
        diagnostic( aResult, "invalid_simulation_source_kind",
                    "source " + id + " kind must be voltage or current" );

    if( !source.contains( "positive" ) || !source.contains( "negative" ) )
        diagnostic( aResult, "missing_simulation_source_node",
                    "source " + id + " requires positive and negative nets" );

    if( !source.contains( "dc" )
        || !( kind == "voltage" ? voltage( source.value( "dc", "" ), dc )
                                 : current( source.value( "dc", "" ), dc ) ) )
    {
        diagnostic( aResult, "invalid_simulation_source_dc",
                    "source " + id + " requires a dc value with the correct SI unit" );
    }
    else
    {
        source["dcSi"] = dc;
    }

    if( source.contains( "acMagnitudeSource" ) )
    {
        double magnitude = 0.0;
        double phase = 0.0;
        const bool validMagnitude = kind == "voltage"
                                            ? voltage( source["acMagnitudeSource"].get<std::string>(), magnitude,
                                                       true )
                                            : current( source["acMagnitudeSource"].get<std::string>(), magnitude,
                                                       true );

        if( !validMagnitude
            || !finiteNumber( source["acPhaseSource"].get<std::string>(), phase )
            || std::abs( phase ) > 360000.0 )
        {
            diagnostic( aResult, "invalid_simulation_source_ac",
                        "source ac requires a positive typed magnitude and finite phase" );
        }
        else
        {
            source["acMagnitudeSi"] = magnitude;
            source["acPhaseDegrees"] = phase;
        }
    }

    if( source.contains( "pulseSource" ) )
    {
        const JSON values = source["pulseSource"];
        JSON compiled = JSON::array();
        bool valid = values.size() == 7;

        for( size_t index = 0; valid && index < values.size(); ++index )
        {
            double parsed = 0.0;

            if( index < 2 )
                valid = kind == "voltage"
                                ? voltage( values[index].get<std::string>(), parsed )
                                : current( values[index].get<std::string>(), parsed );
            else
                valid = timeValue( values[index].get<std::string>(), parsed,
                                   index >= 2 && index <= 4 );

            compiled.push_back( parsed );
        }

        if( !valid || compiled[6].get<double>() <= 0.0
            || compiled[5].get<double>() > compiled[6].get<double>() )
        {
            diagnostic( aResult, "invalid_simulation_source_pulse",
                        "source pulse requires typed levels/times, positive period, and width no greater than period" );
        }
        else
        {
            source["pulseSi"] = std::move( compiled );
        }
    }

    return source;
}


JSON compileAnalysis( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_simulation_analysis", "analysis requires an identifier" );
        return JSON::object();
    }

    JSON analysis = { { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        static const std::set<std::string> allowed = {
            "kind", "step", "stop", "start", "source", "scale", "points"
        };

        if( !allowed.contains( head ) || !fields.emplace( head ).second
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_simulation_analysis_field",
                        "analysis " + id + " has duplicate or malformed field " + head );
            continue;
        }

        analysis[head] = value;
    }

    const std::string kind = analysis.value( "kind", "" );

    if( kind == "operating_point" )
    {
        if( fields.size() != 1 )
            diagnostic( aResult, "redundant_simulation_analysis_field",
                        "operating_point analysis " + id + " accepts only kind" );
    }
    else if( kind == "transient" )
    {
        double step = 0.0;
        double stop = 0.0;
        double start = 0.0;

        if( !timeValue( analysis.value( "step", "" ), step )
            || !timeValue( analysis.value( "stop", "" ), stop ) || step > stop
            || ( analysis.contains( "start" )
                 && ( !timeValue( analysis.value( "start", "" ), start ) || start >= stop ) ) )
        {
            diagnostic( aResult, "invalid_simulation_transient",
                        "transient analysis requires positive step/stop and optional start before stop" );
        }
        else
        {
            analysis["stepSi"] = step;
            analysis["stopSi"] = stop;
            analysis["startSi"] = start;
        }

        for( const std::string& field : fields )
        {
            if( field != "kind" && field != "step" && field != "stop" && field != "start" )
                diagnostic( aResult, "redundant_simulation_analysis_field",
                            "transient analysis " + id + " cannot use field " + field );
        }
    }
    else if( kind == "dc_sweep" )
    {
        if( !identifier( analysis.value( "source", "" ) )
            || !analysis.contains( "start" ) || !analysis.contains( "stop" )
            || !analysis.contains( "step" ) )
        {
            diagnostic( aResult, "invalid_simulation_dc_sweep",
                        "dc_sweep requires source and typed start, stop, and directed step" );
        }

        for( const std::string& field : fields )
        {
            if( field != "kind" && field != "source" && field != "start"
                && field != "stop" && field != "step" )
            {
                diagnostic( aResult, "redundant_simulation_analysis_field",
                            "dc_sweep analysis " + id + " cannot use field " + field );
            }
        }
    }
    else if( kind == "ac_sweep" )
    {
        int64_t points = 0;
        double start = 0.0;
        double stop = 0.0;
        const std::string scale = analysis.value( "scale", "" );

        if( ( scale != "decade" && scale != "octave" && scale != "linear" )
            || !boundedInteger( analysis.value( "points", "" ), 1, 1000000, points )
            || !frequency( analysis.value( "start", "" ), start )
            || !frequency( analysis.value( "stop", "" ), stop ) || start >= stop )
        {
            diagnostic( aResult, "invalid_simulation_ac_sweep",
                        "ac_sweep requires scale, bounded points, and ordered positive frequencies" );
        }
        else
        {
            analysis["points"] = points;
            analysis["startSi"] = start;
            analysis["stopSi"] = stop;
        }

        for( const std::string& field : fields )
        {
            if( field != "kind" && field != "scale" && field != "points"
                && field != "start" && field != "stop" )
            {
                diagnostic( aResult, "redundant_simulation_analysis_field",
                            "ac_sweep analysis " + id + " cannot use field " + field );
            }
        }
    }
    else
    {
        diagnostic( aResult, "invalid_simulation_analysis_kind",
                    "analysis " + id + " kind must be operating_point, transient, dc_sweep, or ac_sweep" );
    }

    return analysis;
}


JSON compileAssertion( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_simulation_assertion", "assert requires an identifier" );
        return JSON::object();
    }

    JSON assertion = { { "id", id }, { "scope", "all" } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& form = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_simulation_assertion_field",
                        "assert " + id + " repeats field " + head );
            continue;
        }

        std::string value;

        if( head == "probe" )
        {
            if( form.children.size() < 3 || form.children.size() > 4
                || !scalar( aDocument, form.children[1], value )
                || ( value != "voltage" && value != "current" ) )
            {
                diagnostic( aResult, "invalid_simulation_probe",
                            "probe requires voltage NET [RETURN_NET] or current SOURCE" );
                continue;
            }

            JSON probe = { { "kind", value }, { "targets", JSON::array() } };

            for( size_t targetIndex = 2; targetIndex < form.children.size(); ++targetIndex )
            {
                std::string target;

                if( !scalar( aDocument, form.children[targetIndex], target )
                    || !identifier( target ) )
                {
                    diagnostic( aResult, "invalid_simulation_probe",
                                "probe targets must be bounded identifiers" );
                    continue;
                }

                probe["targets"].push_back( target );

                if( value == "voltage" )
                    aResult.referencedNets.push_back( target );
            }

            if( ( value == "current" && probe["targets"].size() != 1 )
                || ( value == "voltage" && probe["targets"].size() != 1
                     && probe["targets"].size() != 2 ) )
            {
                diagnostic( aResult, "invalid_simulation_probe",
                            "probe has the wrong target count" );
            }

            assertion["probe"] = std::move( probe );
        }
        else if( head == "analysis" || head == "minimum" || head == "maximum"
                 || head == "scope" )
        {
            if( !oneValue( aDocument, child, value ) )
            {
                diagnostic( aResult, "invalid_simulation_assertion_field",
                            "assert " + id + " has invalid field " + head );
                continue;
            }

            assertion[head] = value;
        }
        else
        {
            diagnostic( aResult, "unknown_simulation_assertion_field",
                        "assert " + id + " does not support field " + head );
        }
    }

    if( !identifier( assertion.value( "analysis", "" ) ) || !assertion.contains( "probe" ) )
        diagnostic( aResult, "incomplete_simulation_assertion",
                    "assert " + id + " requires analysis and probe" );

    const std::string scope = assertion.value( "scope", "" );

    if( scope != "all" && scope != "final" )
        diagnostic( aResult, "invalid_simulation_assertion_scope",
                    "assert scope must be all or final" );

    if( assertion.contains( "probe" ) )
    {
        const bool volts = assertion["probe"].value( "kind", "" ) == "voltage";
        double minimum = 0.0;
        double maximum = 0.0;
        const bool hasMinimum = assertion.contains( "minimum" );
        const bool hasMaximum = assertion.contains( "maximum" );
        const bool minimumOk = !hasMinimum
                               || ( volts
                                            ? voltage( assertion["minimum"].get<std::string>(),
                                                       minimum )
                                            : current( assertion["minimum"].get<std::string>(),
                                                       minimum ) );
        const bool maximumOk = !hasMaximum
                               || ( volts
                                            ? voltage( assertion["maximum"].get<std::string>(),
                                                       maximum )
                                            : current( assertion["maximum"].get<std::string>(),
                                                       maximum ) );

        if( ( !hasMinimum && !hasMaximum ) || !minimumOk || !maximumOk
            || ( hasMinimum && hasMaximum && minimum > maximum ) )
        {
            diagnostic( aResult, "invalid_simulation_assertion_range",
                        "assert " + id + " requires an ordered typed minimum and/or maximum" );
        }
        else
        {
            if( hasMinimum )
                assertion["minimumSi"] = minimum;

            if( hasMaximum )
                assertion["maximumSi"] = maximum;
        }
    }

    return assertion;
}

} // namespace


KICHAD::DESIGN_SCRIPT_SIMULATION_COMPILER::RESULT
KICHAD::DESIGN_SCRIPT_SIMULATION_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    result.simulation = { { "devices", JSON::array() }, { "models", JSON::array() },
                          { "sources", JSON::array() }, { "analyses", JSON::array() },
                          { "assertions", JSON::array() } };
    const LOSSLESS_SEXPR_DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.kind != LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
        || aDocument.ListHead( aNode ) != "simulation" || node.children.size() < 3
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( result, "invalid_simulation", "simulation requires a bounded identifier" );
        return result;
    }

    result.simulation["id"] = id;

    if( node.children.size() > MAX_SIMULATION_ITEMS + 2 )
    {
        diagnostic( result, "simulation_too_large",
                    "simulation may contain at most 4096 declarations" );
        return result;
    }

    std::map<std::string, std::set<std::string>> ids;
    bool sawGround = false;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        JSON compiled;
        std::string collection;

        if( head == "ground" )
        {
            std::string ground;

            if( sawGround || !oneValue( aDocument, child, ground ) || !identifier( ground ) )
                diagnostic( result, "invalid_simulation_ground",
                            "simulation requires exactly one bounded ground net" );
            else
            {
                result.simulation["ground"] = ground;
                result.referencedNets.push_back( ground );
            }

            sawGround = true;
            continue;
        }
        else if( head == "device" )
        {
            compiled = compileDevice( aDocument, child, result );
            collection = "devices";
        }
        else if( head == "model" )
        {
            compiled = compileModel( aDocument, child, result );
            collection = "models";
        }
        else if( head == "source" )
        {
            compiled = compileSource( aDocument, child, result );
            collection = "sources";
        }
        else if( head == "analysis" )
        {
            compiled = compileAnalysis( aDocument, child, result );
            collection = "analyses";
        }
        else if( head == "assert" )
        {
            compiled = compileAssertion( aDocument, child, result );
            collection = "assertions";
        }
        else
        {
            diagnostic( result, "unknown_simulation_declaration",
                        "simulation " + id + " does not support declaration " + head );
            continue;
        }

        const std::string itemId = compiled.value( "id", "" );

        if( !itemId.empty() && !ids[collection].emplace( itemId ).second )
            diagnostic( result, "duplicate_simulation_declaration",
                        "simulation " + id + " repeats " + head + " " + itemId );

        result.simulation[collection].push_back( std::move( compiled ) );
    }

    if( !sawGround )
        diagnostic( result, "missing_simulation_ground", "simulation " + id + " requires ground" );

    if( result.simulation["sources"].empty() || result.simulation["analyses"].empty()
        || result.simulation["assertions"].empty() )
    {
        diagnostic( result, "incomplete_simulation",
                    "simulation " + id + " requires a source, analysis, and assertion" );
    }

    if( result.simulation["devices"].size() > 1024
        || result.simulation["models"].size() > 256
        || result.simulation["sources"].size() > 64
        || result.simulation["analyses"].size() > 32
        || result.simulation["assertions"].size() > 1024 )
    {
        diagnostic( result, "simulation_resource_limit",
                    "simulation exceeds device, model, source, analysis, or assertion limits" );
    }

    std::set<std::string> modelIds;
    std::set<std::string> sourceIds;
    std::map<std::string, std::string> sourceKinds;
    std::set<std::string> analysisIds;

    for( const JSON& model : result.simulation["models"] )
        modelIds.emplace( model.value( "id", "" ) );

    for( const JSON& source : result.simulation["sources"] )
    {
        sourceIds.emplace( source.value( "id", "" ) );
        sourceKinds[source.value( "id", "" )] = source.value( "kind", "" );
    }

    for( const JSON& analysis : result.simulation["analyses"] )
        analysisIds.emplace( analysis.value( "id", "" ) );

    for( const JSON& device : result.simulation["devices"] )
    {
        if( device.contains( "model" )
            && !modelIds.contains( device.value( "model", "" ) ) )
        {
            diagnostic( result, "unresolved_simulation_model",
                        "device " + device.value( "id", "" ) + " references an unknown model" );
        }
    }

    for( JSON& analysis : result.simulation["analyses"] )
    {
        if( analysis.value( "kind", "" ) == "dc_sweep"
            && !sourceIds.contains( analysis.value( "source", "" ) ) )
        {
            diagnostic( result, "unresolved_simulation_source",
                        "analysis " + analysis.value( "id", "" ) + " references an unknown source" );
        }
        else if( analysis.value( "kind", "" ) == "dc_sweep" )
        {
            const std::string source = analysis.value( "source", "" );
            const bool volts = sourceKinds[source] == "voltage";
            double start = 0.0;
            double stop = 0.0;
            double step = 0.0;
            const bool valid = volts
                                       ? voltage( analysis.value( "start", "" ), start )
                                                 && voltage( analysis.value( "stop", "" ), stop )
                                                 && voltage( analysis.value( "step", "" ), step )
                                       : current( analysis.value( "start", "" ), start )
                                                 && current( analysis.value( "stop", "" ), stop )
                                                 && current( analysis.value( "step", "" ), step );

            if( !valid || step == 0.0 || ( stop - start ) / step < 0.0
                || std::abs( ( stop - start ) / step ) > 1.0e6 )
            {
                diagnostic( result, "invalid_simulation_dc_sweep",
                            "dc_sweep values require source-matched units, a directed nonzero step, and at most one million points" );
            }
            else
            {
                analysis["startSi"] = start;
                analysis["stopSi"] = stop;
                analysis["stepSi"] = step;
            }
        }
    }

    for( const JSON& assertion : result.simulation["assertions"] )
    {
        if( !analysisIds.contains( assertion.value( "analysis", "" ) ) )
        {
            diagnostic( result, "unresolved_simulation_analysis",
                        "assert " + assertion.value( "id", "" ) + " references an unknown analysis" );
        }

        if( assertion.contains( "probe" )
            && assertion["probe"].value( "kind", "" ) == "current" )
        {
            const JSON targets = assertion["probe"].value( "targets", JSON::array() );

            if( targets.size() == 1 && !sourceIds.contains( targets[0].get<std::string>() ) )
                diagnostic( result, "unresolved_simulation_current_probe",
                            "current probe must name a simulation source" );
        }
    }

    return result;
}
