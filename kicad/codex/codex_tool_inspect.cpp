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

#include "codex_tool_registry.h"
#include "codex_tool_internal.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wx/file.h>
#include <wx/filename.h>


namespace
{

constexpr wxFileOffset MAX_INSPECTION_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_EXPRESSION_BYTES = 32 * 1024;
constexpr size_t       MAX_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DISTINCT_HEADS = 512;

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json InspectSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation", "path" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array( { "summary", "find" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative KiCad design file path." } };
    schema["properties"]["head"] =
            { { "type", "string" }, { "maxLength", 128 },
              { "description", "List head required by operation 'find'." } };
    schema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 },
              { "description", "Maximum matches returned; defaults to 20." } };

    return { { "type", "function" },
             { "name", "inspect" },
             { "description",
               "Inspect a KiCad 10 schematic, board, symbol library, or footprint without "
               "changing it. Use 'summary' for structural counts or 'find' for bounded raw "
               "expressions with a particular list head." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleInspect(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    if( !aArguments.is_object() )
        return failure( "invalid_arguments", "inspect arguments must be an object" );

    if( !aArguments.contains( "operation" ) || !aArguments["operation"].is_string()
        || !aArguments.contains( "path" ) || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "inspect.operation and inspect.path must be strings" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();
    const std::string relativePath = aArguments["path"].get<std::string>();

    if( operation != "summary" && operation != "find" )
        return failure( "invalid_arguments", "inspect.operation must be 'summary' or 'find'" );

    wxString root = aProjectPath;

    if( !wxFileName::DirExists( root ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    wxFileName resolved;
    std::string pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( root, relativePath, resolved, pathError ) )
        return failure( "invalid_path", pathError );

    wxFile file( resolved.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
        return failure( "read_failed", "Could not open the requested project file" );

    const wxFileOffset length = file.Length();

    if( length == wxInvalidOffset || length > MAX_INSPECTION_BYTES )
        return failure( "file_too_large", "Inspection is limited to 16 MiB per file" );

    std::string source( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( source.data(), static_cast<size_t>( length ) ) != length )
        return failure( "read_failed", "Could not read the complete project file" );

    std::string parseError;
    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( source ), &parseError );

    if( !document )
        return failure( "parse_failed", parseError );

    if( document->Roots().empty() )
        return failure( "parse_failed", "The requested file has no root expression" );

    const std::string rootHead = document->ListHead( document->Roots().front() );

    if( rootHead != KICHAD::CODEX_TOOLS::ExpectedRootHead( resolved.GetExt() ) )
        return failure( "format_mismatch", "The file root does not match its KiCad extension" );

    JSON payload = { { "operation", operation },
                     { "path", relativePath },
                     { "bytes", static_cast<uint64_t>( length ) },
                     { "rootHead", rootHead } };

    if( operation == "summary" )
    {
        std::map<std::string, size_t> counts;

        for( size_t i = 0; i < document->Nodes().size(); ++i )
        {
            std::string head = document->ListHead( i );

            if( !head.empty() )
                ++counts[head];
        }

        std::vector<std::pair<std::string, size_t>> ordered( counts.begin(), counts.end() );
        std::sort( ordered.begin(), ordered.end(),
                   []( const auto& aLeft, const auto& aRight )
                   {
                       if( aLeft.second != aRight.second )
                           return aLeft.second > aRight.second;

                       return aLeft.first < aRight.first;
                   } );

        JSON listCounts = JSON::array();

        for( size_t i = 0; i < ordered.size() && i < MAX_DISTINCT_HEADS; ++i )
        {
            const auto& [head, count] = ordered[i];
            listCounts.push_back( { { "head", head }, { "count", count } } );
        }

        payload["roots"] = document->Roots().size();
        payload["nodes"] = document->Nodes().size();
        payload["distinctHeads"] = ordered.size();
        payload["listCounts"] = std::move( listCounts );
        payload["resultTruncated"] = ordered.size() > MAX_DISTINCT_HEADS;
        return success( payload );
    }

    if( !aArguments.contains( "head" ) || !aArguments["head"].is_string() )
        return failure( "invalid_arguments", "inspect.head must be a string for operation 'find'" );

    const std::string head = aArguments["head"].get<std::string>();

    if( head.empty() || head.size() > 128 )
        return failure( "invalid_arguments", "inspect.head must contain 1 to 128 bytes" );

    int limit = 20;

    if( aArguments.contains( "limit" ) )
    {
        if( !aArguments["limit"].is_number_integer() )
            return failure( "invalid_arguments", "inspect.limit must be an integer" );

        limit = aArguments["limit"].get<int>();

        if( limit < 1 || limit > 50 )
            return failure( "invalid_arguments", "inspect.limit must be between 1 and 50" );
    }

    const std::vector<size_t> matches = document->FindLists( head );
    JSON expressions = JSON::array();
    size_t resultBytes = 0;

    for( size_t i = 0; i < matches.size() && expressions.size() < static_cast<size_t>( limit ); ++i )
    {
        std::string raw = document->RawText( matches[i] );
        bool truncated = raw.size() > MAX_EXPRESSION_BYTES;

        if( truncated )
        {
            size_t boundary = MAX_EXPRESSION_BYTES;

            while( boundary > 0 && boundary < raw.size()
                   && ( static_cast<unsigned char>( raw[boundary] ) & 0xC0 ) == 0x80 )
            {
                --boundary;
            }

            raw.resize( boundary );
        }

        if( resultBytes + raw.size() > MAX_RESULT_BYTES )
            break;

        resultBytes += raw.size();
        expressions.push_back( { { "index", i }, { "text", std::move( raw ) },
                                 { "truncated", truncated } } );
    }

    payload["head"] = head;
    payload["totalMatches"] = matches.size();
    payload["expressions"] = std::move( expressions );
    payload["resultTruncated"] = payload["expressions"].size() < matches.size();
    return success( payload );
}
