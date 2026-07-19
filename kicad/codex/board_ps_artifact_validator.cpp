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

#include "board_ps_artifact_validator.h"

#include <charconv>
#include <fstream>
#include <string_view>


namespace
{

constexpr uintmax_t MAX_BOARD_PS_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_BOARD_PS_LINES = 4'000'000;
constexpr size_t MAX_BOARD_PS_LINE_BYTES = 1024 * 1024;


void trimCarriageReturn( std::string& aLine )
{
    if( !aLine.empty() && aLine.back() == '\r' )
        aLine.pop_back();
}


bool parseSinglePage( std::string_view aLine )
{
    static constexpr std::string_view PREFIX = "%%Pages: ";

    if( !aLine.starts_with( PREFIX ) )
        return false;

    uint64_t pages = 0;
    const char* begin = aLine.data() + PREFIX.size();
    const char* end = aLine.data() + aLine.size();
    const auto [parsedEnd, error] = std::from_chars( begin, end, pages );
    return error == std::errc() && parsedEnd == end && pages == 1;
}

} // namespace


bool KICHAD::BOARD_PS_ARTIFACT_VALIDATOR::ValidateFile(
        const std::filesystem::path& aPath, const std::string& aExpectedLayer,
        std::string& aError )
{
    std::error_code filesystemError;
    const uintmax_t bytes = std::filesystem::file_size( aPath, filesystemError );

    if( filesystemError || bytes == 0 || bytes > MAX_BOARD_PS_BYTES
        || aExpectedLayer.empty() || aExpectedLayer.size() > 128 )
    {
        aError = "board PostScript artifact has an invalid size or layer identity";
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::string line;
    std::string lastLine;
    size_t lines = 0;
    size_t beginPrologs = 0;
    size_t endPrologs = 0;
    size_t pageMarkers = 0;
    size_t pageSetups = 0;
    size_t pageSetupEnds = 0;
    size_t showPages = 0;
    bool header = false;
    bool creator = false;
    bool pages = false;
    bool boundingBox = false;
    bool media = false;
    bool orientation = false;
    const std::string expectedPage = "%%Page: (" + aExpectedLayer + ") 1";

    while( std::getline( input, line ) )
    {
        trimCarriageReturn( line );

        if( line.size() > MAX_BOARD_PS_LINE_BYTES
            || line.find( '\0' ) != std::string::npos
            || ++lines > MAX_BOARD_PS_LINES )
        {
            aError = "board PostScript artifact exceeds structural limits";
            return false;
        }

        if( lines == 1 )
            header = line == "%!PS-Adobe-3.0";

        creator = creator || line == "%%Creator: PCBNEW";
        boundingBox = boundingBox || line.starts_with( "%%BoundingBox: " );
        media = media || line.starts_with( "%%DocumentMedia: A4 " );
        orientation = orientation || line == "%%Orientation: Landscape";

        if( line.starts_with( "%%Pages: " ) )
        {
            if( pages || !parseSinglePage( line ) )
            {
                aError = "board PostScript artifact has an invalid page declaration";
                return false;
            }

            pages = true;
        }
        else if( line.starts_with( "%%Page: " ) )
        {
            if( line != expectedPage )
            {
                aError = "board PostScript artifact identifies the wrong native layer";
                return false;
            }

            ++pageMarkers;
        }
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
                aError = "board PostScript artifact contains a privileged operator";
                return false;
            }
        }

        if( !line.empty() )
            lastLine = line;
    }

    if( input.bad() || !header || !creator || !pages || !boundingBox || !media
        || !orientation || beginPrologs != 1 || endPrologs != 1
        || pageMarkers != 1 || pageSetups != 1 || pageSetupEnds != 1
        || showPages != 1 || lastLine != "%%EOF" )
    {
        aError = "board PostScript artifact is not one complete native layer drawing";
        return false;
    }

    return true;
}
