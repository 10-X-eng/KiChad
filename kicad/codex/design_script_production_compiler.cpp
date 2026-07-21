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

#include "design_script_production_compiler.h"

#include "design_script_assembly_compiler.h"
#include "design_script_device_code_compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include <picosha2.h>
#include <wx/base64.h>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_PRODUCTION_COMPILER::RESULT;

constexpr size_t MAX_FIRMWARE_IMAGES = 16;
constexpr size_t MAX_PROGRAM_STEPS = 16;
constexpr size_t MAX_POWER_PROFILES = 16;
constexpr size_t MAX_BRINGUP_TESTS = 128;
constexpr size_t MAX_PROGRAM_SIGNALS = 32;
constexpr size_t MAX_PROGRAM_OPTIONS = 32;
constexpr uint64_t MAX_FIRMWARE_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_EMBEDDED_FIRMWARE_BYTES = 8ULL * 1024ULL * 1024ULL;


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


bool boundedText( const std::string& aValue, size_t aMaximum, bool aAllowEmpty = false )
{
    return ( aAllowEmpty || !aValue.empty() ) && aValue.size() <= aMaximum
           && std::none_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
           {
               return aCharacter == 0 || aCharacter == 0x7F
                      || ( aCharacter < 0x20 && aCharacter != '\t' && aCharacter != '\n' );
           } );
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
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


bool finiteNumber( const std::string& aText, double aMinimum, double aMaximum,
                   double& aValue )
{
    if( aText.empty() )
        return false;

    const char* begin = aText.data();
    const char* end = begin + aText.size();
    double value = 0.0;
    const auto converted = std::from_chars( begin, end, value, std::chars_format::general );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value )
        || value < aMinimum || value > aMaximum )
    {
        return false;
    }

    aValue = value;
    return true;
}


bool relativeArtifactPath( const std::string& aPath )
{
    if( aPath.empty() || aPath.size() > 1024 || aPath.front() == '/'
        || aPath.front() == '\\' || aPath.find( '\\' ) != std::string::npos
        || aPath.find( ':' ) != std::string::npos || !boundedText( aPath, 1024 ) )
    {
        return false;
    }

    size_t begin = 0;

    while( begin <= aPath.size() )
    {
        const size_t end = aPath.find( '/', begin );
        const std::string_view part( aPath.data() + begin,
                                     ( end == std::string::npos ? aPath.size() : end ) - begin );

        if( part.empty() || part == "." || part == ".." )
            return false;

        if( end == std::string::npos )
            break;

        begin = end + 1;
    }

    return true;
}


bool lowerHexDigest( const std::string& aValue )
{
    return aValue.size() == 64
           && std::all_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
           {
               return std::isdigit( aCharacter ) || ( aCharacter >= 'a' && aCharacter <= 'f' );
           } );
}


bool extensionMatchesFormat( const std::string& aPath, const std::string& aFormat )
{
    const size_t dot = aPath.find_last_of( '.' );
    std::string extension = dot == std::string::npos ? std::string() : aPath.substr( dot );
    std::transform( extension.begin(), extension.end(), extension.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );

    if( aFormat == "binary" )
        return extension == ".bin";
    if( aFormat == "ihex" )
        return extension == ".hex" || extension == ".ihex";
    if( aFormat == "elf" )
        return extension == ".elf";
    if( aFormat == "uf2" )
        return extension == ".uf2";
    if( aFormat == "srec" )
        return extension == ".srec" || extension == ".s19" || extension == ".s28"
               || extension == ".s37";

    return false;
}


JSON compileFirmware( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_production_firmware_id",
                    "firmware requires one unique bounded ID" );
        return JSON::object();
    }

    JSON firmware = { { "id", id } };
    std::set<std::string> fields;
    std::string decoded;
    bool sawDeviceCode = false;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( head == "device_code" )
        {
            if( sawDeviceCode )
            {
                diagnostic( aResult, "duplicate_firmware_device_code",
                            "firmware " + id + " device_code occurs more than once" );
            }
            else
            {
                KICHAD::DESIGN_SCRIPT_DEVICE_CODE_COMPILER::RESULT code =
                        KICHAD::DESIGN_SCRIPT_DEVICE_CODE_COMPILER::Compile( aDocument, child );

                for( JSON& codeDiagnostic : code.diagnostics )
                    aResult.diagnostics.push_back( std::move( codeDiagnostic ) );

                firmware["deviceCode"] = std::move( code.code );
            }

            sawDeviceCode = true;
            continue;
        }

        if( !std::set<std::string>{ "path", "data_base64", "format", "sha256", "bytes",
                                    "version", "target" }.contains( head )
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_production_firmware_field",
                        "firmware supports device_code, path or data_base64, format, sha256, bytes, version, and target" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_production_firmware_field",
                        "firmware " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "path" )
        {
            if( !relativeArtifactPath( value ) )
                diagnostic( aResult, "invalid_production_firmware_path",
                            "firmware path must be one confined project-relative file" );
            else
                firmware["path"] = value;
        }
        else if( head == "data_base64" )
        {
            size_t errorPosition = 0;

            if( value.empty()
                || value.size() > MAX_EMBEDDED_FIRMWARE_BYTES * 4 / 3 + 4 )
            {
                diagnostic( aResult, "invalid_production_firmware_data",
                            "firmware data_base64 must encode 1 byte through 8 MiB" );
                continue;
            }

            wxMemoryBuffer buffer = wxBase64Decode( value.data(), value.size(),
                                                     wxBase64DecodeMode_Strict,
                                                     &errorPosition );

            if( buffer.GetDataLen() == 0
                || buffer.GetDataLen() > MAX_EMBEDDED_FIRMWARE_BYTES )
            {
                diagnostic( aResult, "invalid_production_firmware_data",
                            "firmware data_base64 is malformed or exceeds 8 MiB decoded" );
            }
            else
            {
                const char* bytes = static_cast<const char*>( buffer.GetData() );
                decoded.assign( bytes, bytes + buffer.GetDataLen() );
                firmware["dataBase64"] = value;
            }
        }
        else if( head == "format" )
        {
            if( !std::set<std::string>{ "binary", "ihex", "elf", "uf2", "srec" }
                         .contains( value ) )
                diagnostic( aResult, "invalid_production_firmware_format",
                            "firmware format must be binary, ihex, elf, uf2, or srec" );
            else
                firmware["format"] = value;
        }
        else if( head == "sha256" )
        {
            if( !lowerHexDigest( value ) )
                diagnostic( aResult, "invalid_production_firmware_sha256",
                            "firmware sha256 must be one lowercase SHA-256 digest" );
            else
                firmware["sha256"] = value;
        }
        else if( head == "bytes" )
        {
            uint64_t bytes = 0;

            if( !unsignedInteger( value, 1, MAX_FIRMWARE_BYTES, bytes ) )
                diagnostic( aResult, "invalid_production_firmware_bytes",
                            "firmware bytes must be 1 through 268435456" );
            else
                firmware["bytes"] = bytes;
        }
        else if( head == "version" )
        {
            if( !boundedText( value, 256 ) )
                diagnostic( aResult, "invalid_production_firmware_version",
                            "firmware version must be non-empty bounded text" );
            else
                firmware["version"] = value;
        }
        else if( head == "target" )
        {
            if( !identifier( value ) )
                diagnostic( aResult, "invalid_production_firmware_target",
                            "firmware target must be a bounded component reference" );
            else
            {
                firmware["target"] = value;
                aResult.referencedComponents.emplace_back( value );
            }
        }
    }

    for( const char* required : { "format", "sha256", "bytes", "version", "target" } )
    {
        if( !firmware.contains( required ) )
            diagnostic( aResult, "missing_production_firmware_field",
                        "firmware " + id + " is missing " + required );
    }

    const bool hasPath = firmware.contains( "path" );
    const bool hasEmbeddedData = firmware.contains( "dataBase64" );

    if( hasPath == hasEmbeddedData )
    {
        diagnostic( aResult, "invalid_production_firmware_source",
                    "firmware requires exactly one of path or data_base64" );
    }

    if( hasPath && firmware.contains( "format" )
        && !extensionMatchesFormat( firmware["path"].get<std::string>(),
                                    firmware["format"].get<std::string>() ) )
    {
        diagnostic( aResult, "production_firmware_extension_mismatch",
                    "firmware " + id + " path extension does not match its declared format" );
    }

    if( hasEmbeddedData && firmware.contains( "bytes" )
        && decoded.size() != firmware["bytes"].get<uint64_t>() )
    {
        diagnostic( aResult, "production_firmware_size_mismatch",
                    "firmware " + id + " bytes does not match decoded data_base64" );
    }

    if( hasEmbeddedData && firmware.contains( "sha256" ) )
    {
        std::string digest;
        picosha2::hash256_hex_string( decoded, digest );

        if( digest != firmware["sha256"].get<std::string>() )
            diagnostic( aResult, "production_firmware_digest_mismatch",
                        "firmware " + id + " sha256 does not match decoded data_base64" );
    }

    return firmware;
}


JSON compileProgram( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_production_program_id",
                    "program requires one unique bounded ID" );
        return JSON::object();
    }

    JSON program = { { "id", id }, { "signals", JSON::array() },
                     { "options", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> signals;
    std::set<std::string> signalPins;
    std::set<std::string> options;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );

        if( head == "signal" )
        {
            std::string name;
            std::string pin;

            if( field.children.size() != 3 || !scalar( aDocument, field.children[1], name )
                || !identifier( name ) || !scalar( aDocument, field.children[2], pin )
                || !boundedText( pin, 128 ) || program["signals"].size() >= MAX_PROGRAM_SIGNALS )
            {
                diagnostic( aResult, "invalid_production_program_signal",
                            "program signal requires a unique semantic name and connector pin" );
                continue;
            }

            if( !signals.emplace( name ).second )
                diagnostic( aResult, "duplicate_production_program_signal",
                            "program " + id + " signal " + name + " occurs more than once" );
            else if( !signalPins.emplace( pin ).second )
                diagnostic( aResult, "duplicate_production_program_signal_pin",
                            "program " + id + " maps more than one signal to connector pin "
                                    + pin );
            else
                program["signals"].push_back( { { "name", name }, { "pin", pin } } );

            continue;
        }

        if( head == "option" )
        {
            std::string name;
            std::string value;

            if( field.children.size() != 3 || !scalar( aDocument, field.children[1], name )
                || !identifier( name ) || !scalar( aDocument, field.children[2], value )
                || !boundedText( value, 256, true )
                || program["options"].size() >= MAX_PROGRAM_OPTIONS )
            {
                diagnostic( aResult, "invalid_production_program_option",
                            "program option requires a unique semantic name and bounded value" );
                continue;
            }

            if( !options.emplace( name ).second )
                diagnostic( aResult, "duplicate_production_program_option",
                            "program " + id + " option " + name + " occurs more than once" );
            else
                program["options"].push_back( { { "name", name }, { "value", value } } );

            continue;
        }

        std::string value;

        if( !std::set<std::string>{ "firmware", "target", "interface", "connector", "device",
                                    "voltage", "speed_khz", "erase", "reset", "verify" }
                         .contains( head )
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_production_program_field",
                        "program contains an unknown or malformed field" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_production_program_field",
                        "program " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "firmware" )
        {
            if( !identifier( value ) )
                diagnostic( aResult, "invalid_production_program_firmware",
                            "program firmware must name one declared firmware image" );
            else
                program["firmware"] = value;
        }
        else if( head == "target" || head == "connector" )
        {
            if( !identifier( value ) )
                diagnostic( aResult, "invalid_production_program_component",
                            "program target and connector must be component references" );
            else
            {
                program[head] = value;
                aResult.referencedComponents.emplace_back( value );
            }
        }
        else if( head == "interface" )
        {
            static const std::set<std::string> INTERFACES = {
                "swd", "jtag", "isp", "updi", "pdi", "debugwire", "uart_bootloader",
                "usb_dfu", "can_bootloader"
            };

            if( !INTERFACES.contains( value ) )
                diagnostic( aResult, "invalid_production_program_interface",
                            "program interface is not a supported deterministic protocol" );
            else
                program["interface"] = value;
        }
        else if( head == "device" )
        {
            if( !boundedText( value, 256 ) )
                diagnostic( aResult, "invalid_production_program_device",
                            "program device must be non-empty bounded text" );
            else
                program["device"] = value;
        }
        else if( head == "voltage" )
        {
            double voltage = 0.0;

            if( !finiteNumber( value, 0.8, 60.0, voltage ) )
                diagnostic( aResult, "invalid_production_program_voltage",
                            "program voltage must be between 0.8 and 60 volts" );
            else
                program["voltageV"] = voltage;
        }
        else if( head == "speed_khz" )
        {
            uint64_t speed = 0;

            if( !unsignedInteger( value, 1, 100000, speed ) )
                diagnostic( aResult, "invalid_production_program_speed",
                            "program speed_khz must be 1 through 100000" );
            else
                program["speedKhz"] = speed;
        }
        else if( head == "erase" )
        {
            if( !std::set<std::string>{ "none", "chip", "required_regions" }.contains( value ) )
                diagnostic( aResult, "invalid_production_program_erase",
                            "program erase must be none, chip, or required_regions" );
            else
                program["erase"] = value;
        }
        else if( head == "reset" )
        {
            if( !std::set<std::string>{ "none", "run", "halt" }.contains( value ) )
                diagnostic( aResult, "invalid_production_program_reset",
                            "program reset must be none, run, or halt" );
            else
                program["reset"] = value;
        }
        else if( head == "verify" )
        {
            if( value != "true" && value != "false" )
                diagnostic( aResult, "invalid_production_program_verify",
                            "program verify must be true or false" );
            else
                program["verify"] = value == "true";
        }
    }

    for( const char* required : { "firmware", "target", "interface", "connector", "device",
                                  "voltageV", "speedKhz", "erase", "reset", "verify" } )
    {
        if( !program.contains( required ) )
            diagnostic( aResult, "missing_production_program_field",
                        "program " + id + " is missing " + required );
    }

    if( program.contains( "verify" ) && !program["verify"].get<bool>() )
        diagnostic( aResult, "unsafe_production_program_verification",
                    "production programming must verify the exact firmware after writing" );

    if( program.contains( "interface" ) )
    {
        const std::string interface = program["interface"].get<std::string>();
        std::map<std::string, std::set<std::string>> requiredSignals = {
            { "swd", { "swdio", "swclk", "gnd" } },
            { "jtag", { "tms", "tck", "tdi", "tdo", "gnd" } },
            { "isp", { "mosi", "miso", "sck", "reset", "vcc", "gnd" } },
            { "updi", { "updi", "vcc", "gnd" } },
            { "pdi", { "pdi_data", "pdi_clock", "vcc", "gnd" } },
            { "debugwire", { "debugwire", "vcc", "gnd" } },
            { "uart_bootloader", { "tx", "rx", "gnd" } },
            { "usb_dfu", { "usb_dp", "usb_dm", "gnd" } },
            { "can_bootloader", { "can_h", "can_l", "gnd" } }
        };
        std::set<std::string> semanticSignals;

        for( const JSON& signal : program["signals"] )
        {
            std::string semantic = signal.value( "name", "" );
            std::transform( semantic.begin(), semantic.end(), semantic.begin(),
                            []( unsigned char aCharacter )
                            {
                                return static_cast<char>( std::tolower( aCharacter ) );
                            } );

            static const std::map<std::string, std::string> ALIASES = {
                { "ground", "gnd" }, { "vtref", "vcc" }, { "power", "vcc" },
                { "transmit", "tx" }, { "receive", "rx" },
                { "clock", "pdi_clock" }, { "data", "pdi_data" },
                { "d+", "usb_dp" }, { "dp", "usb_dp" },
                { "d-", "usb_dm" }, { "dm", "usb_dm" }
            };
            const auto alias = ALIASES.find( semantic );

            if( alias != ALIASES.end() )
                semantic = alias->second;

            semanticSignals.emplace( std::move( semantic ) );
        }

        for( const std::string& required : requiredSignals[interface] )
        {
            if( !semanticSignals.contains( required ) )
            {
                diagnostic( aResult, "missing_production_program_protocol_signal",
                            "program " + id + " interface " + interface
                                    + " requires connector signal " + required );
            }
        }
    }

    return program;
}


JSON compilePower( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_production_power_id",
                    "power requires one unique bounded ID" );
        return JSON::object();
    }

    JSON power = { { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( !std::set<std::string>{ "connector", "positive_pin", "return_pin", "voltage",
                                    "current_limit", "settle_ms" }.contains( head )
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_production_power_field",
                        "power supports connector, positive_pin, return_pin, voltage, current_limit, and settle_ms" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_production_power_field",
                        "power " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "connector" )
        {
            if( !identifier( value ) )
                diagnostic( aResult, "invalid_production_power_connector",
                            "power connector must be a component reference" );
            else
            {
                power["connector"] = value;
                aResult.referencedComponents.emplace_back( value );
            }
        }
        else if( head == "positive_pin" || head == "return_pin" )
        {
            if( !boundedText( value, 128 ) )
                diagnostic( aResult, "invalid_production_power_pin",
                            "power pins must be non-empty bounded pin identifiers" );
            else
                power[head == "positive_pin" ? "positivePin" : "returnPin"] = value;
        }
        else if( head == "voltage" || head == "current_limit" )
        {
            double number = 0.0;
            const double minimum = head == "voltage" ? 0.05 : 0.001;
            const double maximum = head == "voltage" ? 1000.0 : 100.0;

            if( !finiteNumber( value, minimum, maximum, number ) )
                diagnostic( aResult, "invalid_production_power_value",
                            "power voltage/current_limit is outside the supported safe range" );
            else
                power[head == "voltage" ? "voltageV" : "currentLimitA"] = number;
        }
        else
        {
            uint64_t settle = 0;

            if( !unsignedInteger( value, 0, 600000, settle ) )
                diagnostic( aResult, "invalid_production_power_settle",
                            "power settle_ms must be 0 through 600000" );
            else
                power["settleMs"] = settle;
        }
    }

    for( const char* required : { "connector", "positivePin", "returnPin", "voltageV",
                                  "currentLimitA", "settleMs" } )
    {
        if( !power.contains( required ) )
            diagnostic( aResult, "missing_production_power_field",
                        "power " + id + " is missing " + required );
    }

    if( power.contains( "positivePin" ) && power.contains( "returnPin" )
        && power["positivePin"] == power["returnPin"] )
    {
        diagnostic( aResult, "invalid_production_power_pin_pair",
                    "power positive_pin and return_pin must be different" );
    }

    return power;
}


JSON compileTest( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_production_test_id",
                    "test requires one unique bounded ID" );
        return JSON::object();
    }

    JSON test = { { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );

        if( head == "target" )
        {
            std::string kind;

            if( !fields.emplace( head ).second || field.children.size() < 3
                || field.children.size() > 4 || !scalar( aDocument, field.children[1], kind ) )
            {
                diagnostic( aResult, "invalid_production_test_target",
                            "test target must occur once as net NAME, component REF, or pin REF PIN" );
                continue;
            }

            if( kind == "net" && field.children.size() == 3 )
            {
                std::string net;

                if( !scalar( aDocument, field.children[2], net ) || !boundedText( net, 256 ) )
                    diagnostic( aResult, "invalid_production_test_target",
                                "test net target requires one bounded net name" );
                else
                {
                    test["target"] = { { "kind", "net" }, { "net", net } };
                    aResult.referencedNets.emplace_back( net );
                }
            }
            else if( kind == "component" && field.children.size() == 3 )
            {
                std::string reference;

                if( !scalar( aDocument, field.children[2], reference )
                    || !identifier( reference ) )
                    diagnostic( aResult, "invalid_production_test_target",
                                "test component target requires one component reference" );
                else
                {
                    test["target"] = { { "kind", "component" },
                                       { "component", reference } };
                    aResult.referencedComponents.emplace_back( reference );
                }
            }
            else if( kind == "pin" && field.children.size() == 4 )
            {
                std::string reference;
                std::string pin;

                if( !scalar( aDocument, field.children[2], reference )
                    || !identifier( reference ) || !scalar( aDocument, field.children[3], pin )
                    || !boundedText( pin, 128 ) )
                    diagnostic( aResult, "invalid_production_test_target",
                                "test pin target requires component reference and pin identifier" );
                else
                {
                    test["target"] = { { "kind", "pin" }, { "component", reference },
                                       { "pin", pin } };
                    aResult.referencedComponents.emplace_back( reference );
                }
            }
            else
            {
                diagnostic( aResult, "invalid_production_test_target",
                            "test target must be net NAME, component REF, or pin REF PIN" );
            }

            continue;
        }

        if( head == "range" )
        {
            std::string minimumText;
            std::string maximumText;
            std::string unit;
            double minimum = 0.0;
            double maximum = 0.0;

            if( !fields.emplace( head ).second || field.children.size() != 4
                || !scalar( aDocument, field.children[1], minimumText )
                || !scalar( aDocument, field.children[2], maximumText )
                || !scalar( aDocument, field.children[3], unit )
                || !finiteNumber( minimumText, -1.0e12, 1.0e12, minimum )
                || !finiteNumber( maximumText, -1.0e12, 1.0e12, maximum )
                || minimum > maximum
                || !std::set<std::string>{ "V", "A", "ohm", "Hz" }.contains( unit ) )
            {
                diagnostic( aResult, "invalid_production_test_range",
                            "test range requires ordered finite minimum/maximum and V, A, ohm, or Hz" );
            }
            else
            {
                test["range"] = { { "minimum", minimum }, { "maximum", maximum },
                                   { "unit", unit } };
            }

            continue;
        }

        if( head == "power" || head == "program" )
        {
            if( !fields.emplace( head ).second || field.children.size() < 2
                || field.children.size() > 17 )
            {
                diagnostic( aResult, "invalid_production_test_dependency",
                            "test power/program must occur once with one through sixteen declared IDs" );
                continue;
            }

            JSON dependencies = JSON::array();
            std::set<std::string> uniqueDependencies;

            for( size_t dependencyIndex = 1; dependencyIndex < field.children.size();
                 ++dependencyIndex )
            {
                std::string dependency;

                if( !scalar( aDocument, field.children[dependencyIndex], dependency )
                    || !identifier( dependency ) )
                {
                    diagnostic( aResult, "invalid_production_test_dependency",
                                "test power/program entries must be declared production IDs" );
                    continue;
                }

                if( !uniqueDependencies.emplace( dependency ).second )
                {
                    diagnostic( aResult, "duplicate_production_test_dependency",
                                "test " + id + " repeats " + head + " dependency "
                                        + dependency );
                    continue;
                }

                dependencies.push_back( std::move( dependency ) );
            }

            if( dependencies.empty() )
                diagnostic( aResult, "missing_production_test_dependency",
                            "test " + id + " requires at least one valid " + head + " ID" );

            test[head] = std::move( dependencies );
            continue;
        }

        std::string value;

        if( !std::set<std::string>{ "stage", "method", "instrument", "expected", "testpoint",
                                    "after_ms", "timeout_ms", "required",
                                    "procedure" }
                         .contains( head )
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_production_test_field",
                        "test contains an unknown or malformed field" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_production_test_field",
                        "test " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "stage" )
        {
            if( !std::set<std::string>{ "unpowered", "power_on", "programmed", "functional" }
                         .contains( value ) )
                diagnostic( aResult, "invalid_production_test_stage",
                            "test stage must be unpowered, power_on, programmed, or functional" );
            else
                test["stage"] = value;
        }
        else if( head == "method" )
        {
            if( !std::set<std::string>{ "voltage", "current", "resistance", "continuity",
                                        "logic", "frequency", "visual", "functional" }
                         .contains( value ) )
                diagnostic( aResult, "invalid_production_test_method",
                            "test method is not a supported semantic measurement" );
            else
                test["method"] = value;
        }
        else if( head == "instrument" )
        {
            if( !std::set<std::string>{ "dmm", "oscilloscope", "logic_analyzer",
                                        "frequency_counter", "ict", "flying_probe", "camera",
                                        "operator", "fixture" }.contains( value ) )
                diagnostic( aResult, "invalid_production_test_instrument",
                            "test instrument is not a supported production instrument" );
            else
                test["instrument"] = value;
        }
        else if( head == "expected" )
        {
            if( !std::set<std::string>{ "open", "closed", "high", "low", "pulse", "pass" }
                         .contains( value ) )
                diagnostic( aResult, "invalid_production_test_expected",
                            "test expected must be open, closed, high, low, pulse, or pass" );
            else
                test["expected"] = value;
        }
        else if( head == "testpoint" )
        {
            if( !identifier( value ) )
                diagnostic( aResult, "invalid_production_test_testpoint",
                            "testpoint must be a component reference" );
            else
            {
                test["testpoint"] = value;
                aResult.referencedComponents.emplace_back( value );
            }
        }
        else if( head == "after_ms" || head == "timeout_ms" )
        {
            uint64_t milliseconds = 0;
            const uint64_t minimum = head == "after_ms" ? 0 : 1;

            if( !unsignedInteger( value, minimum, 3600000, milliseconds ) )
                diagnostic( aResult, "invalid_production_test_timing",
                            "test timing must be bounded to at most one hour" );
            else
                test[head == "after_ms" ? "afterMs" : "timeoutMs"] = milliseconds;
        }
        else if( head == "required" )
        {
            if( value != "true" && value != "false" )
                diagnostic( aResult, "invalid_production_test_required",
                            "test required must be true or false" );
            else
                test["required"] = value == "true";
        }
        else if( head == "procedure" )
        {
            if( !boundedText( value, 1024 ) )
                diagnostic( aResult, "invalid_production_test_procedure",
                            "test procedure must be non-empty bounded text" );
            else
                test["procedure"] = value;
        }
    }

    for( const char* required : { "stage", "method", "instrument", "target", "afterMs",
                                  "timeoutMs", "required" } )
    {
        if( !test.contains( required ) )
            diagnostic( aResult, "missing_production_test_field",
                        "test " + id + " is missing " + required );
    }

    if( test.contains( "required" ) && !test["required"].get<bool>() )
        diagnostic( aResult, "optional_production_test",
                    "production bring-up tests must be explicitly required" );

    if( test.contains( "method" ) )
    {
        const std::string method = test["method"].get<std::string>();
        const bool numeric = method == "voltage" || method == "current"
                             || method == "resistance" || method == "frequency";
        const bool procedural = method == "visual" || method == "functional";

        if( numeric && !test.contains( "range" ) )
            diagnostic( aResult, "missing_production_test_range",
                        "numeric test " + id + " requires an explicit accepted range" );
        if( !numeric && !test.contains( "expected" ) )
            diagnostic( aResult, "missing_production_test_expected",
                        "discrete test " + id + " requires an explicit expected result" );
        if( procedural && !test.contains( "procedure" ) )
            diagnostic( aResult, "missing_production_test_procedure",
                        "visual or functional test " + id + " requires a procedure" );

        if( numeric && test.contains( "range" ) )
        {
            const std::string unit = test["range"]["unit"].get<std::string>();
            const std::string expectedUnit = method == "voltage" ? "V"
                                             : method == "current" ? "A"
                                             : method == "resistance" ? "ohm" : "Hz";

            if( unit != expectedUnit )
                diagnostic( aResult, "production_test_unit_mismatch",
                            "test " + id + " range unit does not match its method" );
        }
    }

    if( test.contains( "stage" ) )
    {
        const std::string stage = test["stage"].get<std::string>();
        const bool hasPower = test.contains( "power" ) && test["power"].is_array()
                              && !test["power"].empty();
        const bool hasProgram = test.contains( "program" ) && test["program"].is_array()
                                && !test["program"].empty();

        if( stage == "unpowered" && ( hasPower || hasProgram ) )
            diagnostic( aResult, "invalid_unpowered_test_dependency",
                        "an unpowered test cannot select power or programming" );
        else if( stage == "power_on" )
        {
            if( !hasPower )
                diagnostic( aResult, "missing_production_test_power",
                            "a power_on test must select at least one declared power profile" );
            if( hasProgram )
                diagnostic( aResult, "invalid_power_on_test_program",
                            "a power_on test runs before programming and cannot select a program" );
        }
        else if( stage == "programmed" || stage == "functional" )
        {
            if( !hasPower )
                diagnostic( aResult, "missing_production_test_power",
                            "a programmed or functional test must select at least one power profile" );
            if( !hasProgram )
                diagnostic( aResult, "missing_production_test_program",
                            "a programmed or functional test must select at least one programming step" );
        }
    }

    return test;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_PRODUCTION_COMPILER::RESULT DESIGN_SCRIPT_PRODUCTION_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;

    if( aDocument.ListHead( aNode ) != "production" )
    {
        diagnostic( result, "invalid_production", "production must be a named list" );
        return result;
    }

    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    result.production = { { "firmware", JSON::array() },
                          { "programming", JSON::array() },
                          { "power", JSON::array() },
                          { "tests", JSON::array() },
                          { "assembly", nullptr } };
    std::set<std::string> firmwareIds;
    std::set<std::string> programIds;
    std::set<std::string> powerIds;
    std::set<std::string> testIds;
    bool sawAssembly = false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "assembly" )
        {
            if( sawAssembly )
            {
                diagnostic( result, "duplicate_production_assembly",
                            "production assembly occurs more than once" );
            }
            else
            {
                KICHAD::DESIGN_SCRIPT_ASSEMBLY_COMPILER::RESULT assembly =
                        KICHAD::DESIGN_SCRIPT_ASSEMBLY_COMPILER::Compile( aDocument, child );

                for( JSON& assemblyDiagnostic : assembly.diagnostics )
                    result.diagnostics.push_back( std::move( assemblyDiagnostic ) );

                result.referencedComponents.insert( result.referencedComponents.end(),
                                                    assembly.referencedComponents.begin(),
                                                    assembly.referencedComponents.end() );
                result.production["assembly"] = std::move( assembly.assembly );
            }

            sawAssembly = true;
            continue;
        }

        JSON entry;
        std::set<std::string>* ids = nullptr;
        const char* output = nullptr;

        if( head == "firmware" )
        {
            if( result.production["firmware"].size() >= MAX_FIRMWARE_IMAGES )
            {
                diagnostic( result, "too_many_production_firmware_images",
                            "production may declare at most 16 firmware images" );
                continue;
            }

            entry = compileFirmware( aDocument, child, result );
            ids = &firmwareIds;
            output = "firmware";
        }
        else if( head == "program" )
        {
            if( result.production["programming"].size() >= MAX_PROGRAM_STEPS )
            {
                diagnostic( result, "too_many_production_program_steps",
                            "production may declare at most 16 programming steps" );
                continue;
            }

            entry = compileProgram( aDocument, child, result );
            ids = &programIds;
            output = "programming";
        }
        else if( head == "power" )
        {
            if( result.production["power"].size() >= MAX_POWER_PROFILES )
            {
                diagnostic( result, "too_many_production_power_profiles",
                            "production may declare at most 16 power profiles" );
                continue;
            }

            entry = compilePower( aDocument, child, result );
            ids = &powerIds;
            output = "power";
        }
        else if( head == "test" )
        {
            if( result.production["tests"].size() >= MAX_BRINGUP_TESTS )
            {
                diagnostic( result, "too_many_production_tests",
                            "production may declare at most 128 ordered bring-up tests" );
                continue;
            }

            entry = compileTest( aDocument, child, result );
            ids = &testIds;
            output = "tests";
        }
        else
        {
            diagnostic( result, "unknown_production_form",
                        "production supports assembly, firmware, program, power, and test forms" );
            continue;
        }

        const std::string id = entry.value( "id", "" );

        if( !id.empty() && !ids->emplace( id ).second )
            diagnostic( result, "duplicate_production_id",
                        head + " ID " + id + " occurs more than once" );

        result.production[output].push_back( std::move( entry ) );
    }

    if( result.production["firmware"].empty() )
        diagnostic( result, "missing_production_firmware",
                    "production requires at least one exact firmware image" );
    if( result.production["programming"].empty() )
        diagnostic( result, "missing_production_programming",
                    "production requires at least one deterministic programming step" );
    if( result.production["power"].empty() )
        diagnostic( result, "missing_production_power",
                    "production requires at least one current-limited power profile" );
    if( result.production["tests"].empty() )
        diagnostic( result, "missing_production_tests",
                    "production requires at least one ordered bring-up test" );
    if( !result.production["assembly"].is_object() )
        diagnostic( result, "missing_production_assembly",
                    "production requires one complete assembly process handoff" );

    std::map<std::string, const JSON*> firmwareById;

    for( const JSON& firmware : result.production["firmware"] )
    {
        if( firmware.is_object() && !firmware.value( "id", "" ).empty() )
            firmwareById[firmware["id"].get<std::string>()] = &firmware;
    }

    for( const JSON& program : result.production["programming"] )
    {
        const std::string id = program.value( "id", "" );
        const std::string firmwareId = program.value( "firmware", "" );
        auto firmware = firmwareById.find( firmwareId );

        if( !firmwareId.empty() && firmware == firmwareById.end() )
        {
            diagnostic( result, "unresolved_production_program_firmware",
                        "program " + id + " references undeclared firmware " + firmwareId );
        }
        else if( firmware != firmwareById.end()
                 && program.value( "target", "" ) != firmware->second->value( "target", "" ) )
        {
            diagnostic( result, "production_program_target_mismatch",
                        "program " + id + " target does not match its firmware target" );
        }
    }

    std::set<std::string> testStages;
    int priorStage = -1;
    const std::map<std::string, int> stageOrder = {
        { "unpowered", 0 }, { "power_on", 1 }, { "programmed", 2 }, { "functional", 3 }
    };

    for( const JSON& test : result.production["tests"] )
    {
        const std::string id = test.value( "id", "" );
        const std::string stage = test.value( "stage", "" );
        testStages.emplace( stage );
        const auto order = stageOrder.find( stage );

        if( order != stageOrder.end() )
        {
            if( order->second < priorStage )
            {
                diagnostic( result, "invalid_production_test_stage_order",
                            "test " + id
                                    + " moves the ordered bring-up traveler back to stage "
                                    + stage );
            }

            priorStage = std::max( priorStage, order->second );
        }

        if( test.contains( "power" ) && test["power"].is_array() )
        {
            for( const JSON& power : test["power"] )
            {
                if( power.is_string() && !powerIds.contains( power.get<std::string>() ) )
                {
                    diagnostic( result, "unresolved_production_test_power",
                                "test " + id + " references undeclared power profile "
                                        + power.get<std::string>() );
                }
            }
        }

        if( test.contains( "program" ) && test["program"].is_array() )
        {
            for( const JSON& program : test["program"] )
            {
                if( program.is_string() && !programIds.contains( program.get<std::string>() ) )
                {
                    diagnostic( result, "unresolved_production_test_program",
                                "test " + id + " references undeclared program "
                                        + program.get<std::string>() );
                }
            }
        }
    }

    if( !testStages.contains( "unpowered" ) )
        diagnostic( result, "missing_unpowered_production_test",
                    "production requires an unpowered short/resistance/continuity check" );
    if( !testStages.contains( "power_on" ) )
        diagnostic( result, "missing_power_on_production_test",
                    "production requires a current-limited power-on rail check" );
    if( !testStages.contains( "programmed" ) && !testStages.contains( "functional" ) )
        diagnostic( result, "missing_functional_production_test",
                    "production requires a programmed or functional acceptance check" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
