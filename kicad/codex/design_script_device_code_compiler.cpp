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

#include "design_script_device_code_compiler.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>

#include <picosha2.h>
#include <wx/base64.h>
#include <wx/string.h>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_DEVICE_CODE_COMPILER::RESULT;

constexpr size_t MAX_CODE_FILES = 128;
constexpr size_t MAX_DEPENDENCIES = 128;
constexpr size_t MAX_CODE_FILE_BYTES = 1024 * 1024;
constexpr size_t MAX_CODE_TOTAL_BYTES = 8 * 1024 * 1024;


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
    if( aValue.empty() || aValue.size() > 256 )
        return false;

    return std::all_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
    {
        return std::isalnum( aCharacter ) || aCharacter == '_' || aCharacter == '-'
               || aCharacter == '.' || aCharacter == '+' || aCharacter == ':';
    } );
}


bool boundedText( const std::string& aValue, size_t aMaximum )
{
    return !aValue.empty() && aValue.size() <= aMaximum
           && std::none_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
           {
               return aCharacter == 0 || aCharacter == 0x7F || aCharacter < 0x20;
           } );
}


bool relativePath( const std::string& aPath )
{
    if( aPath.empty() || aPath.size() > 512 || aPath.front() == '/'
        || aPath.front() == '\\' || aPath.find( '\\' ) != std::string::npos
        || aPath.find( ':' ) != std::string::npos || !boundedText( aPath, 512 ) )
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


bool strictUtf8( const std::string& aSource )
{
    if( aSource.empty() || aSource.find( '\0' ) != std::string::npos )
        return false;

    const wxString decoded = wxString::FromUTF8( aSource.data(), aSource.size() );
    const wxScopedCharBuffer encoded = decoded.ToUTF8();
    return encoded.length() == aSource.size()
           && std::equal( aSource.begin(), aSource.end(), encoded.data() );
}


JSON compileFile( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                  size_t& aTotalBytes )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string path;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], path )
        || !relativePath( path ) )
    {
        diagnostic( aResult, "invalid_device_code_file_path",
                    "device code file requires one normalized project-style relative path" );
        return JSON::object();
    }

    JSON file = { { "path", path } };
    std::set<std::string> fields;
    std::string decoded;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( !std::set<std::string>{ "language", "sha256", "data_base64" }.contains( head )
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_device_code_file_field",
                        "device code file supports language, sha256, and data_base64" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_device_code_file_field",
                        "device code file " + path + " field " + head
                                + " occurs more than once" );
            continue;
        }

        if( head == "language" )
        {
            if( !std::set<std::string>{ "c", "cpp", "rust", "assembly", "arduino",
                                        "linker_script", "configuration" }.contains( value ) )
                diagnostic( aResult, "invalid_device_code_language",
                            "device code language is not a supported source kind" );
            else
                file["language"] = value;
        }
        else if( head == "sha256" )
        {
            if( !lowerHexDigest( value ) )
                diagnostic( aResult, "invalid_device_code_sha256",
                            "device code sha256 must be one lowercase SHA-256 digest" );
            else
                file["sha256"] = value;
        }
        else
        {
            size_t errorPosition = 0;

            if( value.empty() || value.size() > MAX_CODE_FILE_BYTES * 4 / 3 + 4 )
            {
                diagnostic( aResult, "invalid_device_code_data",
                            "device code data_base64 must encode 1 byte through 1 MiB" );
                continue;
            }

            wxMemoryBuffer buffer = wxBase64Decode( value.data(), value.size(),
                                                     wxBase64DecodeMode_Strict,
                                                     &errorPosition );

            if( buffer.GetDataLen() == 0 || buffer.GetDataLen() > MAX_CODE_FILE_BYTES
                || aTotalBytes > MAX_CODE_TOTAL_BYTES - buffer.GetDataLen() )
            {
                diagnostic( aResult, "invalid_device_code_data",
                            "device code data is malformed or exceeds source bundle limits" );
            }
            else
            {
                const char* bytes = static_cast<const char*>( buffer.GetData() );
                decoded.assign( bytes, bytes + buffer.GetDataLen() );
                file["dataBase64"] = value;
                file["bytes"] = decoded.size();
                aTotalBytes += decoded.size();
            }
        }
    }

    for( const char* required : { "language", "sha256", "dataBase64", "bytes" } )
    {
        if( !file.contains( required ) )
            diagnostic( aResult, "missing_device_code_file_field",
                        "device code file " + path + " is missing " + required );
    }

    if( !decoded.empty() )
    {
        std::string digest;
        picosha2::hash256_hex_string( decoded, digest );

        if( !strictUtf8( decoded ) )
            diagnostic( aResult, "invalid_device_code_encoding",
                        "device code file " + path + " must contain strict UTF-8 text" );
        if( file.contains( "sha256" ) && digest != file["sha256"].get<std::string>() )
            diagnostic( aResult, "device_code_digest_mismatch",
                        "device code file " + path + " sha256 does not match its data" );
    }

    return file;
}


JSON compileDependency( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string name;
    std::string version;
    std::string digest;

    if( node.children.size() != 4 || !scalar( aDocument, node.children[1], name )
        || !identifier( name ) || !scalar( aDocument, node.children[2], version )
        || !boundedText( version, 256 ) || !scalar( aDocument, node.children[3], digest )
        || !lowerHexDigest( digest ) )
    {
        diagnostic( aResult, "invalid_device_code_dependency",
                    "device code dependency requires NAME VERSION SHA256" );
        return JSON::object();
    }

    return { { "name", name }, { "version", version }, { "sha256", digest } };
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_DEVICE_CODE_COMPILER::RESULT DESIGN_SCRIPT_DEVICE_CODE_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;

    if( aDocument.ListHead( aNode ) != "device_code" )
    {
        diagnostic( result, "invalid_device_code", "device_code must be a named list" );
        return result;
    }

    result.code = { { "files", JSON::array() }, { "dependencies", JSON::array() } };
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> fields;
    std::set<std::string> paths;
    std::set<std::string> dependencies;
    size_t totalBytes = 0;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "file" )
        {
            if( result.code["files"].size() >= MAX_CODE_FILES )
            {
                diagnostic( result, "too_many_device_code_files",
                            "device_code may contain at most 128 files" );
                continue;
            }

            JSON file = compileFile( aDocument, child, result, totalBytes );
            const std::string path = file.value( "path", "" );

            if( !path.empty() && !paths.emplace( path ).second )
                diagnostic( result, "duplicate_device_code_file",
                            "device code file " + path + " occurs more than once" );

            result.code["files"].push_back( std::move( file ) );
            continue;
        }

        if( head == "dependency" )
        {
            if( result.code["dependencies"].size() >= MAX_DEPENDENCIES )
            {
                diagnostic( result, "too_many_device_code_dependencies",
                            "device_code may contain at most 128 locked dependencies" );
                continue;
            }

            JSON dependency = compileDependency( aDocument, child, result );
            const std::string name = dependency.value( "name", "" );

            if( !name.empty() && !dependencies.emplace( name ).second )
                diagnostic( result, "duplicate_device_code_dependency",
                            "device code dependency " + name + " occurs more than once" );

            result.code["dependencies"].push_back( std::move( dependency ) );
            continue;
        }

        std::string value;

        if( !std::set<std::string>{ "toolchain", "toolchain_version", "target", "entry" }
                         .contains( head )
            || !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_device_code_field",
                        "device_code contains an unknown or malformed field" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_device_code_field",
                        "device_code field " + head + " occurs more than once" );
            continue;
        }

        if( head == "toolchain" )
        {
            if( !std::set<std::string>{ "arduino_cli", "platformio", "zephyr_west", "esp_idf",
                                        "pico_sdk", "cmake", "cargo_embedded" }.contains( value ) )
                diagnostic( result, "invalid_device_code_toolchain",
                            "device_code toolchain is not a supported reproducible adapter" );
            else
                result.code["toolchain"] = value;
        }
        else if( head == "toolchain_version" )
        {
            if( !boundedText( value, 256 ) )
                diagnostic( result, "invalid_device_code_toolchain_version",
                            "device_code toolchain_version must be exact bounded text" );
            else
                result.code["toolchainVersion"] = value;
        }
        else if( head == "target" )
        {
            if( !boundedText( value, 512 ) )
                diagnostic( result, "invalid_device_code_target",
                            "device_code target must be exact bounded text" );
            else
                result.code["target"] = value;
        }
        else
        {
            if( !relativePath( value ) )
                diagnostic( result, "invalid_device_code_entry",
                            "device_code entry must be a normalized bundled source path" );
            else
                result.code["entry"] = value;
        }
    }

    for( const char* required : { "toolchain", "toolchainVersion", "target", "entry" } )
    {
        if( !result.code.contains( required ) )
            diagnostic( result, "missing_device_code_field",
                        std::string( "device_code is missing " ) + required );
    }

    if( result.code["files"].empty() )
        diagnostic( result, "missing_device_code_files",
                    "device_code requires at least one self-contained source file" );

    if( result.code.contains( "entry" )
        && !paths.contains( result.code["entry"].get<std::string>() ) )
    {
        diagnostic( result, "unresolved_device_code_entry",
                    "device_code entry does not name one bundled source file" );
    }

    result.code["totalBytes"] = totalBytes;
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
