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

#include "schematic_ps_artifact_validator.h"

#include <charconv>
#include <fstream>
#include <string_view>


namespace
{

constexpr uintmax_t MAX_SCHEMATIC_PS_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_SCHEMATIC_PS_LINES = 4'000'000;
constexpr size_t MAX_SCHEMATIC_PS_LINE_BYTES = 1024 * 1024;
constexpr uint64_t MAX_SCHEMATIC_PS_PAGES = 10'000;


void trimCarriageReturn( std::string& aLine )
{
    if( !aLine.empty() && aLine.back() == '\r' )
        aLine.pop_back();
}


bool validFilename( const std::filesystem::path& aPath,
                    const std::string& aExpectedStem )
{
    const std::string filename = aPath.filename().string();
    return filename == aExpectedStem + ".ps"
           || ( filename.starts_with( aExpectedStem + "-" )
                && filename.ends_with( ".ps" ) );
}


bool parsePages( std::string_view aLine, uint64_t& aPages )
{
    static constexpr std::string_view PREFIX = "%%Pages: ";

    if( !aLine.starts_with( PREFIX ) )
        return false;

    const char* begin = aLine.data() + PREFIX.size();
    const char* end = aLine.data() + aLine.size();
    const auto [parsedEnd, error] = std::from_chars( begin, end, aPages );
    return error == std::errc() && parsedEnd == end;
}

} // namespace


bool KICHAD::SCHEMATIC_PS_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const std::string& aExpectedStem,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_SCHEMATIC_PS_BYTES
        || !validFilename( aPath, aExpectedStem ) )
    {
        aError = "schematic PostScript artifact has an invalid size or filename";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string line;
    std::string lastLine;
    size_t lines = 0;
    size_t pageMarkers = 0;
    size_t pageSetups = 0;
    size_t pageSetupEnds = 0;
    size_t showPages = 0;
    size_t beginPrologs = 0;
    size_t endPrologs = 0;
    uint64_t declaredPages = 0;
    bool header = false;
    bool creator = false;
    bool boundingBox = false;
    bool media = false;
    bool orientation = false;
    bool pagesSeen = false;

    while( std::getline( input, line ) )
    {
        trimCarriageReturn( line );

        if( line.size() > MAX_SCHEMATIC_PS_LINE_BYTES
            || line.find( '\0' ) != std::string::npos
            || ++lines > MAX_SCHEMATIC_PS_LINES )
        {
            aError = "schematic PostScript artifact exceeds structural limits";
            return false;
        }

        if( lines == 1 )
            header = line == "%!PS-Adobe-3.0";

        creator = creator || line == "%%Creator: Eeschema-PS";
        boundingBox = boundingBox || line.starts_with( "%%BoundingBox: " );
        media = media || line.starts_with( "%%DocumentMedia: " );
        orientation = orientation || line.starts_with( "%%Orientation: " );

        if( line.starts_with( "%%Pages: " ) )
        {
            if( pagesSeen || !parsePages( line, declaredPages ) )
            {
                aError = "schematic PostScript artifact has an invalid page declaration";
                return false;
            }

            pagesSeen = true;
        }
        else if( line.starts_with( "%%Page: " ) )
            ++pageMarkers;
        else if( line == "%%BeginPageSetup" )
            ++pageSetups;
        else if( line == "%%EndPageSetup" )
            ++pageSetupEnds;
        else if( line == "showpage" )
            ++showPages;
        else if( line == "%%BeginProlog" )
            ++beginPrologs;
        else if( line == "%%EndProlog" )
            ++endPrologs;

        for( std::string_view forbidden : { "deletefile", "renamefile", "setdevparams",
                                            "setsystemparams", "startjob", "%pipe" } )
        {
            if( line.find( forbidden ) != std::string::npos )
            {
                aError = "schematic PostScript artifact contains a privileged operator";
                return false;
            }
        }

        if( !line.empty() )
            lastLine = line;
    }

    if( input.bad() || !header || !creator || !boundingBox || !media || !orientation
        || !pagesSeen || declaredPages == 0
        || declaredPages > MAX_SCHEMATIC_PS_PAGES
        || pageMarkers != declaredPages || pageSetups != declaredPages
        || pageSetupEnds != declaredPages || showPages != declaredPages
        || beginPrologs != 1 || endPrologs != 1 || lastLine != "%%EOF" )
    {
        aError = "schematic PostScript artifact is not one complete native drawing";
        return false;
    }

    return true;
}
