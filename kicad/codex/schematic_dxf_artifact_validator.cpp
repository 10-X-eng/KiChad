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

#include "schematic_dxf_artifact_validator.h"

#include <charconv>
#include <fstream>
#include <map>
#include <string_view>


namespace
{

constexpr uintmax_t MAX_SCHEMATIC_DXF_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_SCHEMATIC_DXF_PAIRS = 2'000'000;
constexpr size_t MAX_SCHEMATIC_DXF_LINE_BYTES = 1024 * 1024;


void trimCarriageReturn( std::string& aLine )
{
    if( !aLine.empty() && aLine.back() == '\r' )
        aLine.pop_back();
}


std::string trim( const std::string& aLine )
{
    const size_t first = aLine.find_first_not_of( " \t" );

    if( first == std::string::npos )
        return {};

    const size_t last = aLine.find_last_not_of( " \t" );
    return aLine.substr( first, last - first + 1 );
}


bool validFilename( const std::filesystem::path& aPath,
                    const std::string& aExpectedStem )
{
    const std::string filename = aPath.filename().string();
    return filename == aExpectedStem + ".dxf"
           || ( filename.starts_with( aExpectedStem + "-" )
                && filename.ends_with( ".dxf" ) );
}

} // namespace


bool KICHAD::SCHEMATIC_DXF_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_SCHEMATIC_DXF_BYTES
        || !validFilename( aPath, aExpectedStem ) )
    {
        aError = "schematic DXF artifact has an invalid size or filename";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string codeLine;
    std::string valueLine;
    std::string section;
    std::string headerVariable;
    std::map<std::string, size_t> sections;
    size_t pairs = 0;
    size_t entities = 0;
    bool expectSectionName = false;
    bool sawEof = false;
    bool acad2004 = false;
    bool millimetres = false;
    bool nativeLayer = false;

    while( std::getline( input, codeLine ) )
    {
        if( !std::getline( input, valueLine ) )
        {
            aError = "schematic DXF artifact has an incomplete group pair";
            return false;
        }

        trimCarriageReturn( codeLine );
        trimCarriageReturn( valueLine );

        if( codeLine.size() > MAX_SCHEMATIC_DXF_LINE_BYTES
            || valueLine.size() > MAX_SCHEMATIC_DXF_LINE_BYTES
            || codeLine.find( '\0' ) != std::string::npos
            || valueLine.find( '\0' ) != std::string::npos
            || ++pairs > MAX_SCHEMATIC_DXF_PAIRS || sawEof )
        {
            aError = "schematic DXF artifact exceeds structural limits";
            return false;
        }

        const std::string codeText = trim( codeLine );
        const std::string value = trim( valueLine );
        int code = -1;
        const auto parsed = std::from_chars( codeText.data(),
                                             codeText.data() + codeText.size(), code );

        if( codeText.empty() || parsed.ec != std::errc()
            || parsed.ptr != codeText.data() + codeText.size()
            || code < 0 || code > 1071 )
        {
            aError = "schematic DXF artifact has an invalid group code";
            return false;
        }

        if( expectSectionName )
        {
            if( code != 2 || value.empty() || !sections.emplace( value, 1 ).second )
            {
                aError = "schematic DXF artifact has a malformed section";
                return false;
            }

            section = value;
            expectSectionName = false;
            continue;
        }

        if( code == 0 && value == "SECTION" )
        {
            if( !section.empty() )
            {
                aError = "schematic DXF artifact nests sections";
                return false;
            }

            expectSectionName = true;
            continue;
        }

        if( code == 0 && value == "ENDSEC" )
        {
            if( section.empty() )
            {
                aError = "schematic DXF artifact closes an unopened section";
                return false;
            }

            section.clear();
            headerVariable.clear();
            continue;
        }

        if( code == 0 && value == "EOF" )
        {
            if( !section.empty() )
            {
                aError = "schematic DXF artifact ends inside a section";
                return false;
            }

            sawEof = true;
            continue;
        }

        if( section == "HEADER" && code == 9 )
            headerVariable = value;
        else if( section == "HEADER" && headerVariable == "$ACADVER" && code == 1 )
            acad2004 = value == "AC1018";
        else if( section == "HEADER" && headerVariable == "$INSUNITS" && code == 70 )
            millimetres = value == "4";

        if( section == "TABLES" && value == "KICAD" )
            nativeLayer = true;

        if( section == "ENTITIES" && code == 0 )
            ++entities;
    }

    if( input.bad() || !section.empty() || expectSectionName || !sawEof
        || sections["HEADER"] != 1 || sections["TABLES"] != 1
        || sections["BLOCKS"] != 1 || sections["ENTITIES"] != 1
        || sections["OBJECTS"] != 1 || !acad2004 || !millimetres
        || !nativeLayer || entities == 0 )
    {
        aError = "schematic DXF artifact is not one complete KiCad millimetre drawing";
        return false;
    }

    return true;
}
