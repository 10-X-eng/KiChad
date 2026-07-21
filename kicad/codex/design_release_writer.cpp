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

#include "design_release_writer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

#include <picosha2.h>


namespace
{

constexpr uintmax_t MAX_DESIGN_RELEASE_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr uintmax_t MAX_DESIGN_RELEASE_FILE_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_DESIGN_RELEASE_FILES = 10000;


bool hashFile( const std::filesystem::path& aPath, std::string& aDigest )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
        return false;

    std::array<picosha2::byte_t, picosha2::k_digest_size> digest{};
    picosha2::hash256( input, digest.begin(), digest.end() );
    aDigest = picosha2::bytes_to_hex_string( digest );
    return !input.bad() && aDigest.size() == 64;
}


bool privatePath( const std::filesystem::path& aRelative )
{
    for( const std::filesystem::path& part : aRelative.parent_path() )
    {
        const std::string name = part.string();

        if( name.empty() || name == "." || name == ".." || name == ".git"
            || name == ".history" || name == "fabrication"
            || name.starts_with( ".kichad-fabrication-" ) )
        {
            return true;
        }
    }

    return false;
}


bool portableDesignInput( const std::filesystem::path& aRelative )
{
    if( aRelative.empty() || aRelative.is_absolute()
        || aRelative.lexically_normal() != aRelative || privatePath( aRelative ) )
    {
        return false;
    }

    std::string extension = aRelative.extension().string();
    std::transform( extension.begin(), extension.end(), extension.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );
    const std::string filename = aRelative.filename().string();
    static const std::set<std::string> EXTENSIONS = {
        ".kicad_kds", ".kicad_pcb", ".kicad_sch", ".kicad_sym", ".kicad_mod",
        ".kicad_pro", ".kicad_dru", ".kicad_wks", ".step", ".stp", ".wrl"
    };

    return EXTENSIONS.contains( extension ) || filename == "sym-lib-table"
           || filename == "fp-lib-table";
}


std::string artifactKind( const std::filesystem::path& aRelative )
{
    std::string extension = aRelative.extension().string();
    std::transform( extension.begin(), extension.end(), extension.begin(),
                    []( unsigned char aCharacter )
                    {
                        return static_cast<char>( std::tolower( aCharacter ) );
                    } );

    if( extension == ".kicad_kds" )
        return "kds_source";
    if( extension == ".kicad_pcb" )
        return "native_board";
    if( extension == ".kicad_sch" )
        return "native_schematic";
    if( extension == ".kicad_sym" || extension == ".kicad_mod"
        || aRelative.filename() == "sym-lib-table"
        || aRelative.filename() == "fp-lib-table" )
    {
        return "project_library";
    }
    if( extension == ".step" || extension == ".stp" || extension == ".wrl" )
        return "project_model";

    return "native_project";
}

} // namespace


namespace KICHAD
{

bool DESIGN_RELEASE_WRITER::Write( const std::filesystem::path& aVerificationRoot,
                                   const std::filesystem::path& aStaging,
                                   JSON& aArtifacts, std::string& aError )
{
    aArtifacts = JSON::array();
    std::error_code filesystemError;
    const std::filesystem::path canonicalRoot =
            std::filesystem::canonical( aVerificationRoot, filesystemError );

    if( filesystemError || !std::filesystem::is_directory( canonicalRoot ) )
    {
        aError = "private design-release snapshot is unavailable";
        return false;
    }

    const std::filesystem::path releaseRoot = aStaging / "design";
    std::filesystem::create_directories( releaseRoot, filesystemError );

    if( filesystemError )
    {
        aError = "could not create the portable design-release directory";
        return false;
    }

    const auto cleanup = [&]()
    {
        std::error_code cleanupError;
        std::filesystem::remove_all( releaseRoot, cleanupError );
    };
    uintmax_t totalBytes = 0;
    size_t fileCount = 0;
    std::filesystem::recursive_directory_iterator iterator(
            canonicalRoot, std::filesystem::directory_options::none, filesystemError );
    const std::filesystem::recursive_directory_iterator end;

    while( iterator != end && !filesystemError )
    {
        const std::filesystem::directory_entry& entry = *iterator;
        const std::filesystem::path relative = entry.path().lexically_relative( canonicalRoot );

        if( entry.is_directory( filesystemError ) )
        {
            if( privatePath( relative / "placeholder" ) )
                iterator.disable_recursion_pending();

            iterator.increment( filesystemError );
            continue;
        }

        const std::filesystem::file_status status = entry.symlink_status( filesystemError );

        if( filesystemError || !std::filesystem::is_regular_file( status )
            || !portableDesignInput( relative ) )
        {
            if( !filesystemError )
                iterator.increment( filesystemError );

            continue;
        }

        const uintmax_t bytes = entry.file_size( filesystemError );

        if( filesystemError || bytes == 0 || bytes > MAX_DESIGN_RELEASE_FILE_BYTES
            || fileCount >= MAX_DESIGN_RELEASE_FILES
            || totalBytes > MAX_DESIGN_RELEASE_BYTES - bytes )
        {
            cleanup();
            aError = "portable design release exceeds bounded file or total size limits";
            return false;
        }

        const std::filesystem::path canonical =
                std::filesystem::canonical( entry.path(), filesystemError );
        const std::filesystem::path confined = filesystemError
                                                       ? std::filesystem::path()
                                                       : canonical.lexically_relative( canonicalRoot );

        if( filesystemError || confined != relative || confined.empty() || confined.is_absolute()
            || confined.begin() == confined.end() || *confined.begin() == ".." )
        {
            cleanup();
            aError = "portable design input escapes its private release snapshot";
            return false;
        }

        const std::filesystem::path destination = releaseRoot / relative;
        std::filesystem::create_directories( destination.parent_path(), filesystemError );

        if( !filesystemError )
        {
            std::filesystem::copy_file( canonical, destination,
                                        std::filesystem::copy_options::none, filesystemError );
        }

        std::string sourceDigest;
        std::string destinationDigest;

        if( filesystemError || !hashFile( canonical, sourceDigest )
            || !hashFile( destination, destinationDigest ) || sourceDigest != destinationDigest )
        {
            cleanup();
            aError = "portable design input failed exact copy and digest validation";
            return false;
        }

        aArtifacts.push_back( { { "kind", artifactKind( relative ) },
                                { "path", ( std::filesystem::path( "design" ) / relative )
                                                  .generic_string() },
                                { "sourcePath", relative.generic_string() },
                                { "bytes", bytes },
                                { "sha256", destinationDigest } } );
        totalBytes += bytes;
        ++fileCount;
        iterator.increment( filesystemError );
    }

    if( filesystemError )
    {
        cleanup();
        aError = "could not enumerate the complete portable design release";
        return false;
    }

    const size_t kdsFiles = static_cast<size_t>( std::count_if(
            aArtifacts.begin(), aArtifacts.end(), []( const JSON& aArtifact )
            {
                return aArtifact.value( "kind", "" ) == "kds_source";
            } ) );
    const size_t boardFiles = static_cast<size_t>( std::count_if(
            aArtifacts.begin(), aArtifacts.end(), []( const JSON& aArtifact )
            {
                return aArtifact.value( "kind", "" ) == "native_board";
            } ) );
    const size_t schematicFiles = static_cast<size_t>( std::count_if(
            aArtifacts.begin(), aArtifacts.end(), []( const JSON& aArtifact )
            {
                return aArtifact.value( "kind", "" ) == "native_schematic";
            } ) );

    if( kdsFiles != 1 || boardFiles == 0 || schematicFiles == 0 )
    {
        cleanup();
        aArtifacts = JSON::array();
        aError = "portable design release requires exactly one KDS and at least one board and schematic";
        return false;
    }

    return true;
}

} // namespace KICHAD
