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

#include "schematic_bom_artifact_validator.h"

#include <array>
#include <fstream>
#include <map>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>


namespace
{

using JSON = nlohmann::json;

constexpr uintmax_t MAX_SCHEMATIC_BOM_BYTES = 64ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_SCHEMATIC_BOM_ROWS = 100'000;
constexpr size_t MAX_SCHEMATIC_BOM_FIELD_BYTES = 1024 * 1024;

using ROW = std::array<std::string, 5>;


bool parseCsv( const std::string& aSource, std::vector<ROW>& aRows,
               std::string& aError )
{
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool closedQuote = false;

    const auto finishField = [&]() -> bool
    {
        if( field.size() > MAX_SCHEMATIC_BOM_FIELD_BYTES )
            return false;

        row.push_back( std::move( field ) );
        field.clear();
        closedQuote = false;
        return row.size() <= 5;
    };

    const auto finishRow = [&]() -> bool
    {
        if( !finishField() || row.size() != 5
            || aRows.size() > MAX_SCHEMATIC_BOM_ROWS )
        {
            return false;
        }

        ROW parsed;

        for( size_t index = 0; index < parsed.size(); ++index )
            parsed[index] = std::move( row[index] );

        row.clear();
        aRows.push_back( std::move( parsed ) );
        return true;
    };

    for( size_t index = 0; index < aSource.size(); ++index )
    {
        const char character = aSource[index];

        if( quoted )
        {
            if( character != '"' )
            {
                field.push_back( character );
                continue;
            }

            if( index + 1 < aSource.size() && aSource[index + 1] == '"' )
            {
                field.push_back( '"' );
                ++index;
                continue;
            }

            quoted = false;
            closedQuote = true;
            continue;
        }

        if( closedQuote && character != ',' && character != '\r'
            && character != '\n' )
        {
            aError = "schematic BOM has data after a closing quote";
            return false;
        }

        if( character == '"' )
        {
            if( !field.empty() || closedQuote )
            {
                aError = "schematic BOM has a misplaced quote";
                return false;
            }

            quoted = true;
        }
        else if( character == ',' )
        {
            if( !finishField() )
            {
                aError = "schematic BOM row exceeds its field limits";
                return false;
            }
        }
        else if( character == '\n' )
        {
            if( !finishRow() )
            {
                aError = "schematic BOM has the wrong five-column row shape";
                return false;
            }
        }
        else if( character == '\r' )
        {
            if( index + 1 >= aSource.size() || aSource[index + 1] != '\n' )
            {
                aError = "schematic BOM has an invalid line ending";
                return false;
            }
        }
        else
        {
            field.push_back( character );
        }
    }

    if( quoted )
    {
        aError = "schematic BOM has an unterminated quoted field";
        return false;
    }

    if( !row.empty() || !field.empty() || closedQuote )
    {
        if( !finishRow() )
        {
            aError = "schematic BOM has an incomplete final row";
            return false;
        }
    }

    return !aRows.empty();
}


bool expectedRows( const JSON& aPlan, std::map<std::string, ROW>& aExpected,
                   std::string& aError )
{
    if( !aPlan.is_object() || !aPlan.contains( "expectedSchematicBomRows" )
        || !aPlan["expectedSchematicBomRows"].is_array()
        || aPlan["expectedSchematicBomRows"].size() > MAX_SCHEMATIC_BOM_ROWS )
    {
        aError = "fabrication plan has an invalid schematic BOM contract";
        return false;
    }

    for( const JSON& item : aPlan["expectedSchematicBomRows"] )
    {
        if( !item.is_object() || !item.contains( "reference" )
            || !item["reference"].is_string() || !item.contains( "value" )
            || !item["value"].is_string() || !item.contains( "footprint" )
            || !item["footprint"].is_string() || !item.contains( "dnp" )
            || !item["dnp"].is_boolean() )
        {
            aError = "fabrication plan has a malformed schematic BOM row";
            return false;
        }

        const std::string reference = item["reference"].get<std::string>();
        ROW row = { reference, item["value"].get<std::string>(),
                    item["footprint"].get<std::string>(), "1",
                    item["dnp"].get<bool>() ? "DNP" : "" };

        if( reference.empty() || !aExpected.emplace( reference, std::move( row ) ).second )
        {
            aError = "fabrication plan has a duplicate schematic BOM reference";
            return false;
        }
    }

    return true;
}

} // namespace


bool KICHAD::SCHEMATIC_BOM_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const nlohmann::json& aPlan,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_SCHEMATIC_BOM_BYTES )
    {
        aError = "schematic BOM artifact has an invalid size";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string source{ std::istreambuf_iterator<char>( input ),
                        std::istreambuf_iterator<char>() };

    if( input.bad() || source.size() != bytes
        || source.find( '\0' ) != std::string::npos )
    {
        aError = "schematic BOM artifact could not be read safely";
        return false;
    }

    std::vector<ROW> rows;

    if( !parseCsv( source, rows, aError ) )
    {
        if( aError.empty() )
            aError = "schematic BOM artifact contains no rows";

        return false;
    }

    static const ROW EXPECTED_HEADER = {
        "Reference", "Value", "Footprint", "Quantity", "DNP"
    };

    if( rows.front() != EXPECTED_HEADER )
    {
        aError = "schematic BOM artifact has the wrong native column schema";
        return false;
    }

    std::map<std::string, ROW> expected;

    if( !expectedRows( aPlan, expected, aError ) )
        return false;

    std::map<std::string, ROW> actual;

    for( size_t index = 1; index < rows.size(); ++index )
    {
        const ROW& row = rows[index];

        if( row[0].empty() || row[3] != "1"
            || !actual.emplace( row[0], row ).second )
        {
            aError = "schematic BOM artifact has an invalid quantity or duplicate reference";
            return false;
        }
    }

    if( actual != expected )
    {
        aError = "schematic BOM rows differ from the exact compiled KDS components";
        return false;
    }

    return true;
}
