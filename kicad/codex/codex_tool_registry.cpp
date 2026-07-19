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
#include "design_script_compiler.h"
#include "design_script_pcb_planner.h"
#include "design_script_pcb_reconciler.h"
#include "design_script_schematic_planner.h"
#include "design_script_schematic_reconciler.h"
#include "design_script_symbol_resolver.h"
#include "kicad_ipc_client.h"
#include "lossless_sexpr_document.h"

#include <build_version.h>
#include <kiid.h>
#include <libraries/library_table_parser.h>

#include <algorithm>
#include <array>
#include <boost/process.hpp>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <api/board/board_commands.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/util/field_mask_util.h>
#include <google/protobuf/util/json_util.h>
#include <wx/base64.h>
#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/xml/xml.h>
#include <wx/zipstrm.h>

#include <picosha2.h>


namespace
{

using JSON = nlohmann::json;

constexpr wxFileOffset MAX_INSPECTION_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_EXPRESSION_BYTES = 32 * 1024;
constexpr size_t       MAX_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DISTINCT_HEADS = 512;
constexpr size_t       MAX_PCB_ARGUMENT_BYTES = 1024 * 1024;
constexpr size_t       MAX_PCB_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DESIGN_SCRIPT_BYTES = 1024 * 1024;
constexpr size_t       MAX_DESIGN_STATE_BYTES = 64 * 1024 * 1024;
constexpr size_t       MAX_PROJECT_LIBRARY_TABLE_BYTES = 1024 * 1024;
constexpr size_t       MAX_PROJECT_LIBRARY_ROWS = 256;
constexpr size_t       MAX_SCHEMATIC_FILE_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_SCHEMATIC_INVENTORY_BYTES = 32 * 1024 * 1024;
constexpr wxFileOffset MAX_VERIFICATION_REPORT_BYTES = 64 * 1024 * 1024;
constexpr size_t       MAX_VERIFICATION_RESULT_BYTES = 240 * 1024;
constexpr size_t       MAX_VERIFICATION_IGNORED_CHECKS = 200;
constexpr size_t       MAX_VERIFICATION_IGNORED_BYTES = 64 * 1024;
constexpr uintmax_t    MAX_VERIFICATION_SNAPSHOT_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr size_t       MAX_VERIFICATION_SNAPSHOT_FILES = 10000;
constexpr size_t       MAX_PCB_FOOTPRINTS = 10000;
constexpr size_t       MAX_PCB_ZONES = 10000;
constexpr auto         MAX_ZONE_REFILL_WAIT = std::chrono::seconds( 30 );
constexpr auto         MAX_SCHEMATIC_VALIDATION_WAIT = std::chrono::seconds( 30 );
constexpr auto         MAX_NATIVE_CHECK_WAIT = std::chrono::seconds( 60 );
constexpr auto         MAX_NATIVE_FABRICATION_WAIT = std::chrono::seconds( 120 );
constexpr uintmax_t    MAX_FABRICATION_FILE_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr uintmax_t    MAX_FABRICATION_TOTAL_BYTES = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr size_t       MAX_FABRICATION_FILES = 512;


class PRIVATE_TEMPORARY_DIRECTORY
{
public:
    ~PRIVATE_TEMPORARY_DIRECTORY()
    {
        if( !m_path.empty() )
        {
            std::error_code ignored;
            std::filesystem::remove_all( m_path, ignored );
        }
    }

    bool Create( const std::string& aPrefix, std::string& aError )
    {
        std::error_code filesystemError;
        const std::filesystem::path temporaryRoot =
                std::filesystem::temp_directory_path( filesystemError );

        if( filesystemError )
        {
            aError = "could not resolve the private temporary directory";
            return false;
        }

        m_path = temporaryRoot / ( aPrefix + KIID().AsString().ToStdString() );
        std::filesystem::create_directory( m_path, filesystemError );

        if( filesystemError )
        {
            m_path.clear();
            aError = "could not create the private temporary directory";
            return false;
        }

        std::filesystem::permissions( m_path, std::filesystem::perms::owner_all,
                                      std::filesystem::perm_options::replace,
                                      filesystemError );

        if( filesystemError )
        {
            std::error_code ignored;
            std::filesystem::remove_all( m_path, ignored );
            m_path.clear();
            aError = "could not secure the private temporary directory";
            return false;
        }

        return true;
    }

    const std::filesystem::path& Path() const { return m_path; }

private:
    std::filesystem::path m_path;
};


bool createFabricationVerificationSnapshot(
        const wxFileName& aProjectRoot, const wxFileName& aBoard,
        const wxFileName& aSchematic, const wxFileName& aSidecar,
        const std::array<std::string, 3>& aRelativePaths,
        PRIVATE_TEMPORARY_DIRECTORY& aSnapshot, std::string& aError )
{
    if( !aSnapshot.Create( "kichad-fabrication-verify-", aError ) )
        return false;

    const std::filesystem::path sourceRoot( aProjectRoot.GetFullPath().ToStdString() );
    uintmax_t totalBytes = 0;
    size_t fileCount = 0;
    std::set<std::filesystem::path> copied;

    const auto copyFile = [&]( const std::filesystem::path& aSource,
                               const std::filesystem::path& aRelative )
    {
        const std::filesystem::path normalized = aRelative.lexically_normal();

        if( normalized.empty() || normalized.is_absolute()
            || normalized.begin() == normalized.end() || *normalized.begin() == ".." )
        {
            aError = "verification snapshot contains an unsafe project-relative path";
            return false;
        }

        std::error_code filesystemError;
        const std::filesystem::file_status status =
                std::filesystem::symlink_status( aSource, filesystemError );

        if( filesystemError || !std::filesystem::is_regular_file( status ) )
        {
            aError = "verification snapshot input is not a regular file";
            return false;
        }

        const uintmax_t bytes = std::filesystem::file_size( aSource, filesystemError );

        if( filesystemError || bytes > MAX_FABRICATION_FILE_BYTES )
        {
            aError = "verification snapshot input is unreadable or exceeds 512 MiB";
            return false;
        }

        const bool alreadyCopied = copied.contains( normalized );

        if( !alreadyCopied
            && ( fileCount >= MAX_VERIFICATION_SNAPSHOT_FILES
                 || totalBytes > MAX_VERIFICATION_SNAPSHOT_BYTES - bytes ) )
        {
            aError = "verification snapshot exceeds 10,000 files or 512 MiB";
            return false;
        }

        const std::filesystem::path destination = aSnapshot.Path() / normalized;
        std::filesystem::create_directories( destination.parent_path(), filesystemError );

        if( filesystemError )
        {
            aError = "could not create the verification snapshot directory structure";
            return false;
        }

        std::filesystem::copy_file( aSource, destination,
                                    std::filesystem::copy_options::overwrite_existing,
                                    filesystemError );

        if( filesystemError )
        {
            aError = "could not copy a complete verification snapshot input";
            return false;
        }

        if( !alreadyCopied )
        {
            copied.emplace( normalized );
            totalBytes += bytes;
            ++fileCount;
        }

        return true;
    };

    std::error_code filesystemError;
    std::filesystem::recursive_directory_iterator iterator(
            sourceRoot, std::filesystem::directory_options::skip_permission_denied,
            filesystemError );
    const std::filesystem::recursive_directory_iterator end;

    while( iterator != end && !filesystemError )
    {
        const std::filesystem::directory_entry& entry = *iterator;
        const std::filesystem::path relative = entry.path().lexically_relative( sourceRoot );
        const std::string name = entry.path().filename().string();

        if( entry.is_directory( filesystemError ) )
        {
            if( name == ".git" || name == "fabrication"
                || name.starts_with( ".kichad-fabrication-" ) )
            {
                iterator.disable_recursion_pending();
            }
        }
        else if( !filesystemError )
        {
            std::string extension = entry.path().extension().string();
            std::transform( extension.begin(), extension.end(), extension.begin(),
                            []( unsigned char aCharacter )
                            {
                                return static_cast<char>( std::tolower( aCharacter ) );
                            } );
            const bool selected = extension == ".kicad_sch" || extension == ".kicad_sym"
                                  || extension == ".kicad_mod"
                                  || extension == ".kicad_pro"
                                  || extension == ".kicad_prl"
                                  || extension == ".kicad_dru"
                                  || extension == ".kicad_wks"
                                  || extension == ".step" || extension == ".stp"
                                  || extension == ".wrl"
                                  || name == "sym-lib-table" || name == "fp-lib-table";

            if( selected && !copyFile( entry.path(), relative ) )
                return false;
        }

        iterator.increment( filesystemError );
    }

    if( filesystemError )
    {
        aError = "could not enumerate project inputs for the verification snapshot";
        return false;
    }

    const std::array<const wxFileName*, 3> required = {
        &aSidecar, &aBoard, &aSchematic
    };

    for( size_t index = 0; index < required.size(); ++index )
    {
        if( !copyFile( required[index]->GetFullPath().ToStdString(),
                       std::filesystem::path( aRelativePaths[index] ) ) )
        {
            return false;
        }
    }

    return true;
}


struct PROJECT_LIBRARY_TABLE_UPDATE
{
    std::string kind;
    wxFileName  path;
    std::string source;
    size_t      rows = 0;
    bool        previousPresent = false;
    std::string previousSource;
    bool        applied = false;
};


struct SCHEMATIC_FILE_UPDATE
{
    std::string relativePath;
    wxFileName  path;
    std::string source;
    bool        previousPresent = false;
    std::string previousSource;
    bool        applied = false;
};


bool isInspectableExtension( const wxString& aExtension )
{
    return aExtension == wxS( "kicad_sch" ) || aExtension == wxS( "kicad_pcb" )
           || aExtension == wxS( "kicad_sym" ) || aExtension == wxS( "kicad_mod" );
}


bool canonicalizeExisting( wxFileName& aPath, bool aDirectory = false )
{
    std::error_code         error;
    const std::string       utf8Path( aPath.GetFullPath().ToUTF8() );
    const std::u8string     filesystemPath(
            reinterpret_cast<const char8_t*>( utf8Path.data() ), utf8Path.size() );
    std::filesystem::path   canonical =
            std::filesystem::canonical( std::filesystem::path( filesystemPath ), error );

    if( error )
        return false;

    const std::u8string utf8Canonical = canonical.generic_u8string();
    wxString canonicalPath = wxString::FromUTF8(
            reinterpret_cast<const char*>( utf8Canonical.data() ), utf8Canonical.size() );

    if( aDirectory )
        aPath.AssignDir( canonicalPath );
    else
        aPath.Assign( canonicalPath );

    return true;
}


bool resolveProjectLibraryTable( const wxString& aProjectPath, const std::string& aName,
                                 wxFileName& aResolved, std::string& aError )
{
    if( aName != "sym-lib-table" && aName != "fp-lib-table" )
    {
        aError = "project library table name is not supported";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    wxFileName candidate( root.GetFullPath(), wxString::FromUTF8( aName ) );

    if( wxDirExists( candidate.GetFullPath() ) )
    {
        aError = "project library table path is a directory";
        return false;
    }

    if( candidate.FileExists() )
    {
        wxFileName canonical = candidate;

        if( !canonicalizeExisting( canonical ) )
        {
            aError = "existing project library table could not be resolved";
            return false;
        }

        wxString canonicalPath = canonical.GetFullPath();
        wxString candidatePath = candidate.GetFullPath();

#ifdef __WXMSW__
        canonicalPath.MakeLower();
        candidatePath.MakeLower();
#endif

        if( canonicalPath != candidatePath )
        {
            aError = "project library table cannot be a symbolic link";
            return false;
        }
    }

    aResolved = candidate;
    return true;
}


bool readOptionalTextFile( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                           std::string& aError )
{
    aPresent = aPath.FileExists();
    aSource.clear();

    if( !aPresent )
        return true;

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open the project library table";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length < 0 || length > static_cast<wxFileOffset>( MAX_PROJECT_LIBRARY_TABLE_BYTES ) )
    {
        aError = "project library table exceeds 1 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( aSource.data(), aSource.size() ) != length )
    {
        aSource.clear();
        aError = "could not read the complete project library table";
        return false;
    }

    return true;
}


bool installTextFileAtomically( const wxFileName& aPath, bool aPresent,
                                const std::string& aSource, std::string& aError )
{
    if( !aPresent )
    {
        if( aPath.FileExists() && !wxRemoveFile( aPath.GetFullPath() ) )
        {
            aError = "could not remove the project library table during rollback";
            return false;
        }

        if( aPath.FileExists() )
        {
            aError = "project library table remained after rollback";
            return false;
        }

        return true;
    }

    if( aSource.size() > MAX_PROJECT_LIBRARY_TABLE_BYTES )
    {
        aError = "project library table exceeds 1 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( aSource.data(), aSource.size() ) != aSource.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write the project library table";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install the project library table";
        return false;
    }

    bool        installedPresent = false;
    std::string installedSource;

    if( !readOptionalTextFile( aPath, installedPresent, installedSource, aError )
        || !installedPresent || installedSource != aSource )
    {
        aError = "project library table verification failed after installation";
        return false;
    }

    return true;
}


bool resolveProjectSchematic( const wxString& aProjectPath, const std::string& aRelativePath,
                              wxFileName& aResolved, std::string& aError )
{
    wxString relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.empty() || aRelativePath.size() > 4096
        || aRelativePath.find( '\0' ) != std::string::npos || candidate.IsAbsolute()
        || candidate.GetExt() != wxS( "kicad_sch" ) )
    {
        aError = "schematic path must be a project-relative .kicad_sch file";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName parent = wxFileName::DirName( candidate.GetPath() );

    if( !canonicalizeExisting( parent, true ) )
    {
        aError = "schematic parent directory does not exist";
        return false;
    }

    wxString parentPath = parent.GetPathWithSep();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    parentPath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !parentPath.StartsWith( rootPath ) && parent.GetFullPath() != root.GetFullPath() )
    {
        aError = "schematic path resolves outside the active project";
        return false;
    }

    aResolved.Assign( parent.GetFullPath(), candidate.GetFullName() );

    if( aResolved.FileExists() )
    {
        wxFileName canonical = aResolved;

        if( !canonicalizeExisting( canonical ) )
        {
            aError = "existing schematic path could not be resolved";
            return false;
        }

        wxString resolvedPath = aResolved.GetFullPath();
        wxString canonicalPath = canonical.GetFullPath();

#ifdef __WXMSW__
        resolvedPath.MakeLower();
        canonicalPath.MakeLower();
#endif

        if( resolvedPath != canonicalPath )
        {
            aError = "managed schematic path cannot be a symbolic link";
            return false;
        }
    }

    return true;
}


bool inventoryProjectSymbolLibraries( const wxString& aProjectPath,
                                      const nlohmann::json& aCompilerIr,
                                      nlohmann::json& aSources, std::string& aError )
{
    constexpr size_t MAX_SYMBOL_LIBRARY_BYTES = 16 * 1024 * 1024;
    constexpr size_t MAX_SYMBOL_INVENTORY_BYTES = 32 * 1024 * 1024;
    aSources = nlohmann::json::object();
    size_t totalBytes = 0;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "libraries" )
        || !aCompilerIr["libraries"].is_array() || !aCompilerIr.contains( "schematic" )
        || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array() )
    {
        aError = "compiled KDS library inventory is malformed";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    std::set<std::string> usedNicknames;

    for( const nlohmann::json& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "symbol" )
            || !component["symbol"].is_string() || !component.contains( "units" )
            || !component["units"].is_array() || component["units"].empty() )
        {
            continue;
        }

        const std::string libraryId = component["symbol"].get<std::string>();
        const size_t separator = libraryId.find( ':' );

        if( separator != std::string::npos )
            usedNicknames.emplace( libraryId.substr( 0, separator ) );
    }

    for( const nlohmann::json& library : aCompilerIr["libraries"] )
    {
        if( !library.is_object() || library.value( "kind", "" ) != "symbol"
            || library.value( "table", "" ) != "project"
            || !usedNicknames.contains( library.value( "id", "" ) ) )
        {
            continue;
        }

        const std::string nickname = library.value( "id", "" );
        const std::string uri = library.value( "uri", "" );
        constexpr std::string_view prefix = "${KIPRJMOD}/";

        if( nickname.empty() || !uri.starts_with( prefix )
            || uri.size() <= prefix.size() || !uri.ends_with( ".kicad_sym" ) )
        {
            aError = "project symbol library declaration is malformed";
            return false;
        }

        const std::string relativePath = uri.substr( prefix.size() );
        wxFileName candidate( wxString::FromUTF8( relativePath ) );

        if( candidate.IsAbsolute() )
        {
            aError = "project symbol library path must be relative";
            return false;
        }

        candidate.MakeAbsolute( root.GetFullPath() );
        candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

        if( !candidate.FileExists() )
        {
            aError = "project symbol library does not exist: " + relativePath;
            return false;
        }

        wxFileName canonical = candidate;

        if( !canonicalizeExisting( canonical ) )
        {
            aError = "project symbol library could not be resolved: " + relativePath;
            return false;
        }

        wxString candidatePath = candidate.GetFullPath();
        wxString canonicalPath = canonical.GetFullPath();
        wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
        candidatePath.MakeLower();
        canonicalPath.MakeLower();
        rootPath.MakeLower();
#endif

        if( candidatePath != canonicalPath || !canonicalPath.StartsWith( rootPath ) )
        {
            aError = "project symbol library must be a confined regular file, not a symlink";
            return false;
        }

        wxFile input;

        if( !input.Open( candidate.GetFullPath() ) )
        {
            aError = "could not open project symbol library: " + relativePath;
            return false;
        }

        const wxFileOffset length = input.Length();

        if( length <= 0 || length > static_cast<wxFileOffset>( MAX_SYMBOL_LIBRARY_BYTES )
            || static_cast<size_t>( length ) > MAX_SYMBOL_INVENTORY_BYTES - totalBytes )
        {
            aError = "project symbol library inventory exceeds bounded size limits";
            return false;
        }

        std::string source( static_cast<size_t>( length ), '\0' );

        const auto bytesRead = input.Read( source.data(), source.size() );

        if( bytesRead < 0 || static_cast<size_t>( bytesRead ) != source.size() )
        {
            aError = "could not read complete project symbol library: " + relativePath;
            return false;
        }

        totalBytes += source.size();

        if( aSources.contains( nickname ) )
        {
            aError = "project symbol library nickname occurs more than once: " + nickname;
            return false;
        }

        aSources[nickname] = std::move( source );
    }

    return true;
}


bool inventoryProjectFootprints( const wxString& aProjectPath,
                                 const nlohmann::json& aCompilerIr,
                                 nlohmann::json& aSources, std::string& aError )
{
    constexpr size_t MAX_FOOTPRINT_BYTES = 4 * 1024 * 1024;
    constexpr size_t MAX_FOOTPRINT_INVENTORY_BYTES = 32 * 1024 * 1024;
    aSources = nlohmann::json::object();

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "libraries" )
        || !aCompilerIr["libraries"].is_array() || !aCompilerIr.contains( "schematic" )
        || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "components" )
        || !aCompilerIr["schematic"]["components"].is_array()
        || !aCompilerIr.contains( "pcb" ) || !aCompilerIr["pcb"].is_array() )
    {
        aError = "compiled KDS footprint inventory is malformed";
        return false;
    }

    std::set<std::string> placedReferences;

    for( const nlohmann::json& statement : aCompilerIr["pcb"] )
    {
        if( statement.is_object() && statement.value( "kind", "" ) == "place"
            && statement.contains( "component" ) && statement["component"].is_string() )
        {
            placedReferences.emplace( statement["component"].get<std::string>() );
        }
    }

    std::map<std::string, std::set<std::string>> usedEntries;

    for( const nlohmann::json& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string()
            || !placedReferences.contains( component["reference"].get<std::string>() )
            || !component.contains( "footprint" ) || !component["footprint"].is_string() )
        {
            continue;
        }

        const std::string libraryId = component["footprint"].get<std::string>();
        const size_t separator = libraryId.find( ':' );

        if( separator == std::string::npos || separator == 0
            || separator + 1 == libraryId.size()
            || libraryId.find( ':', separator + 1 ) != std::string::npos )
        {
            aError = "placed component contains an invalid footprint library ID";
            return false;
        }

        const std::string nickname = libraryId.substr( 0, separator );
        const std::string entry = libraryId.substr( separator + 1 );

        if( entry == "." || entry == ".." || entry.find( '/' ) != std::string::npos
            || entry.find( '\\' ) != std::string::npos
            || entry.find( '\0' ) != std::string::npos )
        {
            aError = "placed footprint entry name cannot contain a path";
            return false;
        }

        usedEntries[nickname].emplace( entry );
    }

    if( usedEntries.empty() )
        return true;

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    std::set<std::string> inventoriedNicknames;
    size_t totalBytes = 0;

    for( const nlohmann::json& library : aCompilerIr["libraries"] )
    {
        if( !library.is_object() || library.value( "kind", "" ) != "footprint"
            || library.value( "table", "" ) != "project"
            || !usedEntries.contains( library.value( "id", "" ) ) )
        {
            continue;
        }

        const std::string nickname = library.value( "id", "" );
        const std::string uri = library.value( "uri", "" );
        constexpr std::string_view prefix = "${KIPRJMOD}/";

        if( !inventoriedNicknames.emplace( nickname ).second )
        {
            aError = "project footprint library nickname occurs more than once: " + nickname;
            return false;
        }

        if( nickname.empty() || !uri.starts_with( prefix )
            || uri.size() <= prefix.size() || !uri.ends_with( ".pretty" ) )
        {
            aError = "project footprint library declaration is malformed";
            return false;
        }

        const std::string relativePath = uri.substr( prefix.size() );
        wxFileName directory = wxFileName::DirName( wxString::FromUTF8( relativePath ) );

        if( directory.IsAbsolute() )
        {
            aError = "project footprint library path must be relative";
            return false;
        }

        directory.MakeAbsolute( root.GetFullPath() );
        directory.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

        if( !directory.DirExists() )
        {
            aError = "project footprint library does not exist: " + relativePath;
            return false;
        }

        wxFileName canonicalDirectory = directory;

        if( !canonicalizeExisting( canonicalDirectory, true ) )
        {
            aError = "project footprint library could not be resolved: " + relativePath;
            return false;
        }

        wxString directoryPath = directory.GetFullPath();
        wxString canonicalDirectoryPath = canonicalDirectory.GetFullPath();
        wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
        directoryPath.MakeLower();
        canonicalDirectoryPath.MakeLower();
        rootPath.MakeLower();
#endif

        if( directoryPath != canonicalDirectoryPath
            || !( canonicalDirectoryPath + wxFileName::GetPathSeparator() ).StartsWith( rootPath ) )
        {
            aError = "project footprint library must be a confined directory, not a symlink";
            return false;
        }

        for( const std::string& entry : usedEntries.at( nickname ) )
        {
            wxFileName candidate( directory.GetFullPath(),
                                  wxString::FromUTF8( entry + ".kicad_mod" ) );

            if( !candidate.FileExists() )
            {
                aError = "project footprint does not exist: " + nickname + ":" + entry;
                return false;
            }

            wxFileName canonical = candidate;

            if( !canonicalizeExisting( canonical ) )
            {
                aError = "project footprint could not be resolved: " + nickname + ":" + entry;
                return false;
            }

            wxString candidatePath = candidate.GetFullPath();
            wxString canonicalPath = canonical.GetFullPath();
            wxString libraryPath = canonicalDirectory.GetPathWithSep();

#ifdef __WXMSW__
            candidatePath.MakeLower();
            canonicalPath.MakeLower();
            libraryPath.MakeLower();
#endif

            if( candidatePath != canonicalPath || !canonicalPath.StartsWith( libraryPath ) )
            {
                aError = "project footprint must be a confined regular file, not a symlink";
                return false;
            }

            wxFile input;

            if( !input.Open( candidate.GetFullPath() ) )
            {
                aError = "could not open project footprint: " + nickname + ":" + entry;
                return false;
            }

            const wxFileOffset length = input.Length();

            if( length <= 0 || length > static_cast<wxFileOffset>( MAX_FOOTPRINT_BYTES )
                || static_cast<size_t>( length )
                           > MAX_FOOTPRINT_INVENTORY_BYTES - totalBytes )
            {
                aError = "project footprint inventory exceeds bounded size limits";
                return false;
            }

            std::string source( static_cast<size_t>( length ), '\0' );

            if( input.Read( source.data(), source.size() ) != length )
            {
                aError = "could not read complete project footprint: " + nickname + ":" + entry;
                return false;
            }

            totalBytes += source.size();
            aSources[nickname + ":" + entry] = std::move( source );
        }
    }

    return true;
}


bool readOptionalSchematic( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                            std::string& aError )
{
    aPresent = aPath.FileExists();
    aSource.clear();

    if( !aPresent )
        return true;

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open managed schematic";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_SCHEMATIC_FILE_BYTES ) )
    {
        aError = "managed schematic must contain 1 byte to 16 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( file.Read( aSource.data(), aSource.size() ) != length )
    {
        aSource.clear();
        aError = "could not read the complete managed schematic";
        return false;
    }

    return true;
}


bool installSchematicAtomically( const wxFileName& aPath, bool aPresent,
                                 const std::string& aSource, std::string& aError )
{
    if( !aPresent )
    {
        if( aPath.FileExists() && !wxRemoveFile( aPath.GetFullPath() ) )
        {
            aError = "could not remove newly created schematic during rollback";
            return false;
        }

        if( aPath.FileExists() )
        {
            aError = "newly created schematic remained after rollback";
            return false;
        }

        return true;
    }

    if( aSource.empty() || aSource.size() > MAX_SCHEMATIC_FILE_BYTES )
    {
        aError = "managed schematic must contain 1 byte to 16 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( aSource.data(), aSource.size() ) != aSource.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write managed schematic temporary data";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install managed schematic";
        return false;
    }

    bool installedPresent = false;
    std::string installedSource;

    if( !readOptionalSchematic( aPath, installedPresent, installedSource, aError )
        || !installedPresent || installedSource != aSource )
    {
        aError = "managed schematic verification failed after installation";
        return false;
    }

    return true;
}


std::string directSchematicScalar( const KICHAD::LOSSLESS_SEXPR_DOCUMENT& aDocument,
                                   size_t aParent, const std::string& aHead )
{
    size_t match = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

    for( size_t child : aDocument.Nodes().at( aParent ).children )
    {
        if( aDocument.Nodes().at( child ).kind
                    != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || aDocument.ListHead( child ) != aHead )
        {
            continue;
        }

        if( match != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
            return {};

        match = child;
    }

    if( match == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
        return {};

    const auto& node = aDocument.Nodes().at( match );

    if( node.children.size() != 2
        || aDocument.Nodes().at( node.children[1] ).kind
                   == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
    {
        return {};
    }

    return aDocument.AtomText( node.children[1] );
}


bool schematicScreenUuid( const std::string& aSource, std::string& aUuid,
                          std::string& aError )
{
    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( aSource, &aError );

    if( !document || document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "kicad_sch" )
    {
        if( aError.empty() )
            aError = "managed schematic must contain one kicad_sch root";

        return false;
    }

    aUuid = directSchematicScalar( *document, document->Roots().front(), "uuid" );

    if( aUuid.empty() )
    {
        aError = "managed schematic must contain exactly one screen UUID";
        return false;
    }

    return true;
}


bool findNativeKiCadCli( wxFileName& aCli )
{
    wxFileName executable( wxStandardPaths::Get().GetExecutablePath() );
#ifdef __WXMSW__
    const wxString cliName = wxS( "kicad-cli.exe" );
#else
    const wxString cliName = wxS( "kicad-cli" );
#endif
    wxFileName sibling( executable.GetPath(), cliName );

    if( sibling.FileExists() && sibling.IsFileExecutable() )
    {
        aCli = sibling;
        return true;
    }

    wxString runFromBuild;

    if( !wxGetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), &runFromBuild )
        || runFromBuild != wxS( "1" ) )
    {
        return false;
    }

    wxFileName root = wxFileName::DirName( executable.GetPath() );

    for( int depth = 0; depth < 6 && root.GetDirCount() > 0; ++depth )
    {
        wxFileName candidate = root;
        candidate.AppendDir( wxS( "kicad" ) );
        candidate.SetFullName( cliName );

        if( candidate.FileExists() && candidate.IsFileExecutable() )
        {
            aCli = candidate;
            return true;
        }

        root.RemoveLastDir();
    }

    return false;
}


bool validateNativeSchematicHierarchy( const wxFileName& aRootSchematic,
                                       std::string& aError )
{
    namespace bp = boost::process;

    wxFileName cli;

    if( !findNativeKiCadCli( cli ) )
    {
        aError = "the sibling kicad-cli native schematic validator is unavailable";
        return false;
    }

    const wxString token = KIID().AsString();
    wxFileName output( wxFileName::GetTempDir(), wxS( "kichad-kds-" ) + token + wxS( ".net" ) );
    wxFileName stdoutLog( wxFileName::GetTempDir(),
                           wxS( "kichad-kds-" ) + token + wxS( ".stdout" ) );
    wxFileName stderrLog( wxFileName::GetTempDir(),
                           wxS( "kichad-kds-" ) + token + wxS( ".stderr" ) );
    bool finished = false;
    int exitCode = -1;

    try
    {
        bp::child process(
                cli.GetFullPath().ToStdString(),
                bp::args( std::vector<std::string>{ "sch", "export", "netlist", "--output",
                                                    output.GetFullPath().ToStdString(),
                                                    aRootSchematic.GetFullPath().ToStdString() } ),
                bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                bp::std_err > stderrLog.GetFullPath().ToStdString() );
        const auto deadline = std::chrono::steady_clock::now()
                              + MAX_SCHEMATIC_VALIDATION_WAIT;
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                aError = "native schematic validation process failed: "
                         + processError.message();
        }
        else
        {
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = std::string( "could not run native schematic validation: " ) + error.what();
    }

    if( aError.empty() && ( !finished || exitCode != 0 || !output.FileExists() ) )
    {
        aError = !finished ? "native schematic validation exceeded 30 seconds"
                           : "native KiCad rejected the generated schematic hierarchy";

        if( stderrLog.FileExists() )
        {
            wxFile errorFile( stderrLog.GetFullPath(), wxFile::read );
            const wxFileOffset length = errorFile.IsOpened() ? errorFile.Length() : 0;

            if( length > 0 )
            {
                const size_t bounded = std::min<size_t>( static_cast<size_t>( length ), 4096 );
                std::string detail( bounded, '\0' );

                if( errorFile.Read( detail.data(), detail.size() )
                    == static_cast<wxFileOffset>( detail.size() ) )
                {
                    aError += ": " + detail;
                }
            }
        }
    }

    if( output.FileExists() )
        wxRemoveFile( output.GetFullPath() );

    if( stdoutLog.FileExists() )
        wxRemoveFile( stdoutLog.GetFullPath() );

    if( stderrLog.FileExists() )
        wxRemoveFile( stderrLog.GetFullPath() );

    return aError.empty();
}


bool runNativeKiCadCheck( const std::string& aCheck, const wxFileName& aInput,
                          std::string& aReport, std::string& aError )
{
    namespace bp = boost::process;

    wxFileName cli;

    if( !findNativeKiCadCli( cli ) )
    {
        aError = "the sibling kicad-cli verification backend is unavailable";
        return false;
    }

    wxFileName temporaryRoot = wxFileName::DirName( wxFileName::GetTempDir() );
    temporaryRoot.AppendDir( wxS( "kichad-verify-" ) + KIID().AsString() );

    if( !wxFileName::Mkdir( temporaryRoot.GetFullPath(), 0700 ) )
    {
        aError = "could not create a private native verification directory";
        return false;
    }

    wxFileName reportPath( temporaryRoot.GetFullPath(), wxS( "report.json" ) );
    wxFileName stdoutLog( temporaryRoot.GetFullPath(), wxS( "stdout.log" ) );
    wxFileName stderrLog( temporaryRoot.GetFullPath(), wxS( "stderr.log" ) );
    std::vector<std::string> arguments = aCheck == "erc"
                                                 ? std::vector<std::string>{ "sch", "erc" }
                                                 : std::vector<std::string>{ "pcb", "drc" };
    arguments.insert( arguments.end(), { "--format", "json", "--units", "mm",
                                         "--severity-all" } );

    if( aCheck == "drc" )
        arguments.push_back( "--schematic-parity" );

    arguments.insert( arguments.end(),
                      { "--output", reportPath.GetFullPath().ToStdString(),
                        aInput.GetFullPath().ToStdString() } );

    bool finished = false;
    int  exitCode = -1;

    try
    {
        bp::child process( cli.GetFullPath().ToStdString(), bp::args( arguments ),
                           bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                           bp::std_err > stderrLog.GetFullPath().ToStdString() );
        const auto deadline = std::chrono::steady_clock::now() + MAX_NATIVE_CHECK_WAIT;
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                aError = "native verification process failed: " + processError.message();
        }
        else
        {
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = std::string( "could not run native KiCad verification: " ) + error.what();
    }

    if( aError.empty() && ( !finished || exitCode != 0 || !reportPath.FileExists() ) )
    {
        aError = !finished ? "native KiCad verification exceeded 60 seconds"
                           : "native KiCad could not complete the requested check";

        if( stderrLog.FileExists() )
        {
            wxFile errorFile( stderrLog.GetFullPath(), wxFile::read );
            const wxFileOffset length = errorFile.IsOpened() ? errorFile.Length() : 0;

            if( length > 0 )
            {
                const size_t bounded = std::min<size_t>( static_cast<size_t>( length ), 4096 );
                std::string detail( bounded, '\0' );

                if( errorFile.Read( detail.data(), detail.size() )
                    == static_cast<wxFileOffset>( detail.size() ) )
                {
                    aError += ": " + detail;
                }
            }
        }
    }

    if( aError.empty() )
    {
        wxFile reportFile( reportPath.GetFullPath(), wxFile::read );
        const wxFileOffset length = reportFile.IsOpened() ? reportFile.Length() : wxInvalidOffset;

        if( length <= 0 || length > MAX_VERIFICATION_REPORT_BYTES )
        {
            aError = "native verification report must contain 1 byte to 64 MiB";
        }
        else
        {
            aReport.resize( static_cast<size_t>( length ) );

            if( reportFile.Read( aReport.data(), aReport.size() ) != length )
                aError = "could not read the complete native verification report";
        }
    }

    wxFileName::Rmdir( temporaryRoot.GetFullPath(), wxPATH_RMDIR_RECURSIVE );

    return aError.empty();
}


std::string joinCommaSeparated( const JSON& aValues )
{
    std::string joined;

    for( const JSON& value : aValues )
    {
        if( !joined.empty() )
            joined += ',';

        joined += value.get<std::string>();
    }

    return joined;
}


bool runNativeFabricationCommand( const wxFileName& aCli,
                                  const std::vector<std::string>& aArguments,
                                  const wxFileName& aLogDirectory, size_t aIndex,
                                  const std::string& aKind, std::string& aError )
{
    namespace bp = boost::process;

    wxFileName stdoutLog( aLogDirectory.GetFullPath(),
                          wxString::Format( wxS( "%zu.stdout" ), aIndex ) );
    wxFileName stderrLog( aLogDirectory.GetFullPath(),
                          wxString::Format( wxS( "%zu.stderr" ), aIndex ) );
    bool finished = false;
    int exitCode = -1;

    try
    {
        bp::child process( aCli.GetFullPath().ToStdString(), bp::args( aArguments ),
                           bp::std_out > stdoutLog.GetFullPath().ToStdString(),
                           bp::std_err > stderrLog.GetFullPath().ToStdString() );
        const auto deadline = std::chrono::steady_clock::now() + MAX_NATIVE_FABRICATION_WAIT;
        std::error_code processError;

        while( process.running( processError ) && !processError
               && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }

        finished = !process.running( processError ) && !processError;

        if( !finished )
        {
            process.terminate();
            process.wait();

            if( processError )
                aError = "native " + aKind + " export process failed: "
                         + processError.message();
        }
        else
        {
            process.wait();
            exitCode = process.exit_code();
        }
    }
    catch( const std::exception& error )
    {
        aError = "could not run native " + aKind + " export: " + error.what();
    }

    if( aError.empty() && ( !finished || exitCode != 0 ) )
    {
        aError = !finished ? "native " + aKind + " export exceeded 120 seconds"
                           : "native KiCad could not complete the " + aKind + " export";

        if( stderrLog.FileExists() )
        {
            wxFile errorFile( stderrLog.GetFullPath(), wxFile::read );
            const wxFileOffset length = errorFile.IsOpened() ? errorFile.Length() : 0;

            if( length > 0 )
            {
                const size_t bounded = std::min<size_t>( static_cast<size_t>( length ), 4096 );
                std::string detail( bounded, '\0' );

                if( errorFile.Read( detail.data(), detail.size() )
                    == static_cast<wxFileOffset>( detail.size() ) )
                {
                    aError += ": " + detail;
                }
            }
        }
    }

    return aError.empty();
}


bool runNativeKiCadFabrication( const wxFileName& aBoard, const JSON& aPlan,
                                const wxFileName& aStaging, std::string& aError )
{
    wxFileName cli;

    if( !findNativeKiCadCli( cli ) )
    {
        aError = "the sibling kicad-cli fabrication backend is unavailable";
        return false;
    }

    const std::filesystem::path staging( aStaging.GetFullPath().ToStdString() );
    const std::filesystem::path logs = staging / ".native-logs";
    std::error_code filesystemError;
    std::filesystem::create_directories( logs, filesystemError );

    if( filesystemError )
    {
        aError = "could not create private native fabrication logs";
        return false;
    }

    wxFileName logDirectory = wxFileName::DirName( wxString::FromUTF8( logs.string() ) );
    size_t commandIndex = 0;

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        const std::string kind = job.at( "kind" ).get<std::string>();

        if( kind == "bom" )
            continue;

        const std::filesystem::path output =
                staging / job.at( "relativePath" ).get<std::string>();
        const std::filesystem::path parent =
                job.at( "directory" ).get<bool>() ? output : output.parent_path();
        std::filesystem::create_directories( parent, filesystemError );

        if( filesystemError )
        {
            aError = "could not create private " + kind + " staging directory";
            break;
        }

        std::vector<std::string> arguments;

        if( kind == "gerbers" )
        {
            arguments = { "pcb", "export", "gerbers", "--output", output.string(),
                          "--layers", joinCommaSeparated( job.at( "layers" ) ),
                          "--exclude-value", "--subtract-soldermask", "--precision", "6",
                          "--no-protel-ext", "--check-zones",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "drill" )
        {
            arguments = { "pcb", "export", "drill", "--output", output.string(),
                          "--format", "excellon", "--drill-origin", "absolute",
                          "--excellon-zeros-format", "decimal", "--excellon-oval-format",
                          "alternate", "--excellon-units", "mm", "--excellon-separate-th",
                          "--generate-map", "--map-format", "pdf", "--generate-report",
                          "--report-path", ( output / "drill-report.rpt" ).string(),
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "ipcd356" )
        {
            arguments = { "pcb", "export", "ipcd356", "--output", output.string(),
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "ipc2581" )
        {
            arguments = { "pcb", "export", "ipc2581", "--output", output.string(),
                          "--precision", "6", "--version", "C", "--units", "mm",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "odbpp" )
        {
            arguments = { "pcb", "export", "odb", "--output", output.string(),
                          "--precision", "4", "--compression", "zip", "--units", "mm",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "pick_place" )
        {
            arguments = { "pcb", "export", "pos", "--output", output.string(),
                          "--side", "both", "--format", "csv", "--units", "mm",
                          "--exclude-dnp", aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "step" )
        {
            arguments = { "pcb", "export", "step", "--output", output.string(),
                          "--force", "--no-dnp", "--subst-models", "--include-tracks",
                          "--include-pads", "--include-zones", "--include-inner-copper",
                          "--include-silkscreen", "--include-soldermask",
                          aBoard.GetFullPath().ToStdString() };
        }
        else if( kind == "pdf" )
        {
            arguments = { "pcb", "export", "pdf", "--output", output.string(),
                          "--layers", joinCommaSeparated( job.at( "layers" ) ),
                          "--mode-multipage", "--black-and-white", "--subtract-soldermask",
                          "--check-zones", "--no-property-popups",
                          aBoard.GetFullPath().ToStdString() };
        }
        else
        {
            aError = "fabrication plan contains an unsupported native job";
            break;
        }

        if( !runNativeFabricationCommand( cli, arguments, logDirectory, commandIndex++, kind,
                                          aError ) )
        {
            break;
        }
    }

    std::filesystem::remove_all( logs, filesystemError );
    return aError.empty();
}


JSON buildFabricationPlan( const JSON& aIr, const std::string& aFileStem )
{
    static constexpr const char* CHECK_ORDER[] = {
        "erc", "drc", "sourcing", "fabrication"
    };
    static constexpr const char* OUTPUT_ORDER[] = {
        "gerbers", "drill", "ipcd356", "ipc2581", "odbpp", "pick_place", "bom", "step",
        "pdf"
    };
    static const std::set<std::string> PRODUCTION_OUTPUTS = {
        "gerbers", "drill", "ipcd356", "pick_place", "bom"
    };
    std::set<std::string> checks;
    std::set<std::string> outputs;

    for( const JSON& check : aIr.at( "checks" ) )
        checks.emplace( check.value( "kind", "" ) );

    for( const JSON& output : aIr.at( "outputs" ) )
        outputs.emplace( output.value( "kind", "" ) );

    JSON issues = JSON::array();

    for( const char* required : CHECK_ORDER )
    {
        if( !checks.contains( required ) )
        {
            issues.push_back( { { "type", "missing_check" },
                                { "severity", "error" },
                                { "description",
                                  "Production fabrication intent is missing check "
                                          + std::string( required ) },
                                { "check", required } } );
        }
    }

    for( const std::string& required : PRODUCTION_OUTPUTS )
    {
        if( !outputs.contains( required ) )
        {
            issues.push_back( { { "type", "missing_output" },
                                { "severity", "error" },
                                { "description",
                                  "Production fabrication intent is missing output " + required },
                                { "output", required } } );
        }
    }

    const JSON* stackup = nullptr;

    for( const JSON& statement : aIr.at( "pcb" ) )
    {
        if( statement.value( "kind", "" ) == "stackup" )
        {
            stackup = &statement;
            break;
        }
    }

    JSON layers = JSON::array();

    if( stackup == nullptr )
    {
        issues.push_back( { { "type", "missing_stackup" },
                            { "severity", "error" },
                            { "description",
                              "Production fabrication requires one explicit KDS stackup" } } );
    }
    else
    {
        for( const JSON& entry : stackup->at( "layers" ) )
        {
            const std::string category = entry.value( "category", "" );
            std::string layer = entry.value( "layer", "" );

            if( category != "copper" && category != "soldermask"
                && category != "solderpaste" && category != "silkscreen" )
            {
                continue;
            }

            if( layer == "F.SilkS" )
                layer = "F.Silkscreen";
            else if( layer == "B.SilkS" )
                layer = "B.Silkscreen";

            layers.push_back( std::move( layer ) );
        }

        layers.push_back( "Edge.Cuts" );
    }

    JSON jobs = JSON::array();
    std::set<std::string> bomReferences;
    std::set<std::string> placementReferences;

    for( const JSON& component : aIr.at( "schematic" ).at( "components" ) )
    {
        if( !component.contains( "footprint" ) || !component.at( "footprint" ).is_string() )
            continue;

        const std::string reference = component.at( "reference" ).get<std::string>();
        bomReferences.emplace( reference );

        if( !component.value( "dnp", false ) )
            placementReferences.emplace( reference );
    }

    for( const char* kind : OUTPUT_ORDER )
    {
        if( !outputs.contains( kind ) )
            continue;

        JSON job = { { "kind", kind }, { "directory", false } };

        if( std::string_view( kind ) == "gerbers" )
        {
            job["relativePath"] = "gerbers";
            job["directory"] = true;
            job["layers"] = layers;
        }
        else if( std::string_view( kind ) == "drill" )
        {
            job["relativePath"] = "drill";
            job["directory"] = true;
        }
        else if( std::string_view( kind ) == "ipcd356" )
        {
            job["relativePath"] = "electrical-test/" + aFileStem + ".d356";
        }
        else if( std::string_view( kind ) == "ipc2581" )
        {
            job["relativePath"] = "manufacturing/" + aFileStem + ".ipc2581.xml";
        }
        else if( std::string_view( kind ) == "odbpp" )
        {
            job["relativePath"] = "manufacturing/" + aFileStem + ".odb.zip";
        }
        else if( std::string_view( kind ) == "pick_place" )
        {
            job["relativePath"] = "assembly/" + aFileStem + "-positions.csv";
        }
        else if( std::string_view( kind ) == "bom" )
        {
            job["relativePath"] = "assembly/" + aFileStem + "-bom.csv";
        }
        else if( std::string_view( kind ) == "step" )
        {
            job["relativePath"] = "model/" + aFileStem + ".step";
        }
        else if( std::string_view( kind ) == "pdf" )
        {
            job["relativePath"] = "documentation/" + aFileStem + ".pdf";
            job["layers"] = layers;
        }

        jobs.push_back( std::move( job ) );
    }

    return { { "profile", "kichad-production-10.0.4-v4" },
             { "targetDirectory", "fabrication" },
             { "fileStem", aFileStem },
             { "productionReady", issues.empty() },
             { "issues", std::move( issues ) },
             { "expectedBomReferences", bomReferences },
             { "expectedPlacementReferences", placementReferences },
             { "jobs", std::move( jobs ) } };
}


std::string csvField( const std::string& aValue )
{
    std::string escaped;
    escaped.reserve( aValue.size() + 2 );
    escaped.push_back( '"' );

    for( char character : aValue )
    {
        if( character == '"' )
            escaped.push_back( '"' );

        escaped.push_back( character );
    }

    escaped.push_back( '"' );
    return escaped;
}


bool writeFabricationBom( const JSON& aIr, const wxFileName& aStaging,
                          const JSON& aPlan, size_t& aRows, std::string& aError )
{
    const JSON* bomJob = nullptr;

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        if( job.at( "kind" ) == "bom" )
        {
            bomJob = &job;
            break;
        }
    }

    if( bomJob == nullptr )
        return true;

    std::map<std::string, const JSON*> sourcing;

    for( const JSON& source : aIr.at( "sourcing" ) )
        sourcing.emplace( source.at( "component" ).get<std::string>(), &source );

    std::vector<const JSON*> components;

    for( const JSON& component : aIr.at( "schematic" ).at( "components" ) )
    {
        if( component.contains( "footprint" ) && !component.at( "footprint" ).is_null() )
            components.push_back( &component );
    }

    std::sort( components.begin(), components.end(),
               []( const JSON* aLeft, const JSON* aRight )
               {
                   return aLeft->at( "reference" ).get<std::string>()
                          < aRight->at( "reference" ).get<std::string>();
               } );
    const std::filesystem::path path =
            std::filesystem::path( aStaging.GetFullPath().ToStdString() )
            / bomJob->at( "relativePath" ).get<std::string>();
    std::error_code filesystemError;
    std::filesystem::create_directories( path.parent_path(), filesystemError );

    if( filesystemError )
    {
        aError = "could not create the private BOM staging directory";
        return false;
    }

    std::ofstream output( path, std::ios::binary | std::ios::trunc );

    if( !output )
    {
        aError = "could not create the KDS fabrication BOM";
        return false;
    }

    output << "Reference,Value,Footprint,Quantity,Manufacturer,MPN,Datasheet,Lifecycle,"
              "Supplier,SKU,Product URL,Available,Verified On,Unit Price,DNP\n";

    for( const JSON* component : components )
    {
        const std::string reference = component->at( "reference" ).get<std::string>();
        const auto source = sourcing.find( reference );

        if( source == sourcing.end() )
        {
            aError = "physical component " + reference + " has no KDS sourcing record";
            return false;
        }

        const JSON& evidence = *source->second;
        const auto text = [&]( const char* aField )
        {
            return evidence.contains( aField ) ? evidence.at( aField ).get<std::string>()
                                                : std::string();
        };
        output << csvField( reference ) << ','
               << csvField( component->at( "value" ).get<std::string>() ) << ','
               << csvField( component->at( "footprint" ).get<std::string>() ) << ','
               << evidence.at( "quantity" ).get<int64_t>() << ','
               << csvField( evidence.at( "manufacturer" ).get<std::string>() ) << ','
               << csvField( evidence.at( "mpn" ).get<std::string>() ) << ','
               << csvField( evidence.at( "datasheet" ).get<std::string>() ) << ','
               << csvField( evidence.at( "lifecycle" ).get<std::string>() ) << ','
               << csvField( evidence.at( "supplier" ).get<std::string>() ) << ','
               << csvField( evidence.at( "sku" ).get<std::string>() ) << ','
               << csvField( evidence.at( "product_url" ).get<std::string>() ) << ','
               << evidence.at( "available" ).get<int64_t>() << ','
               << csvField( evidence.at( "verified_on" ).get<std::string>() ) << ','
               << csvField( text( "unit_price" ) ) << ','
               << ( component->value( "dnp", false ) ? "true" : "false" ) << '\n';
        ++aRows;
    }

    output.flush();

    if( !output )
    {
        aError = "could not write the complete KDS fabrication BOM";
        return false;
    }

    return true;
}


bool readFileEdges( const std::filesystem::path& aPath, std::string& aPrefix,
                    std::string& aSuffix )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
        return false;

    constexpr std::streamsize EDGE_BYTES = 8192;
    aPrefix.resize( static_cast<size_t>( EDGE_BYTES ) );
    input.read( aPrefix.data(), EDGE_BYTES );
    aPrefix.resize( static_cast<size_t>( input.gcount() ) );
    input.clear();
    input.seekg( 0, std::ios::end );
    const std::streamoff length = input.tellg();
    const std::streamoff start = std::max<std::streamoff>( 0, length - EDGE_BYTES );
    input.seekg( start, std::ios::beg );
    aSuffix.resize( static_cast<size_t>( length - start ) );
    input.read( aSuffix.data(), static_cast<std::streamsize>( aSuffix.size() ) );
    return input.good() || input.eof();
}


bool validateIpc2581Artifact( const std::filesystem::path& aPath, const JSON& aPlan,
                              std::string& aError )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
    {
        aError = "could not read IPC-2581 artifact";
        return false;
    }

    std::array<char, 64 * 1024> buffer;
    std::string                 scanWindow;

    while( input )
    {
        input.read( buffer.data(), static_cast<std::streamsize>( buffer.size() ) );
        const std::streamsize bytes = input.gcount();

        if( bytes <= 0 )
            break;

        scanWindow.append( buffer.data(), static_cast<size_t>( bytes ) );

        if( scanWindow.find( "<!DOCTYPE" ) != std::string::npos
            || scanWindow.find( "<!ENTITY" ) != std::string::npos )
        {
            aError = "IPC-2581 artifact contains a forbidden document or entity declaration";
            return false;
        }

        constexpr size_t DECLARATION_OVERLAP = 16;

        if( scanWindow.size() > DECLARATION_OVERLAP )
            scanWindow.erase( 0, scanWindow.size() - DECLARATION_OVERLAP );
    }

    if( input.bad() )
    {
        aError = "could not inspect the complete IPC-2581 artifact";
        return false;
    }

    wxXmlDocument document;
    const std::string nativePath = aPath.string();

    if( !document.Load( wxString::FromUTF8( nativePath.c_str() ) ) )
    {
        aError = "IPC-2581 artifact is not well-formed XML";
        return false;
    }

    wxXmlNode* root = document.GetRoot();

    if( !root || root->GetName() != wxS( "IPC-2581" )
        || root->GetAttribute( wxS( "revision" ) ) != wxS( "C" )
        || root->GetAttribute( wxS( "xmlns" ) ) != wxS( "http://webstds.ipc.org/2581" )
        || root->GetAttribute( wxS( "xsi:schemaLocation" ) )
                   != wxS( "http://webstds.ipc.org/2581 "
                           "http://webstds.ipc.org/2581/IPC-2581C.xsd" ) )
    {
        aError = "IPC-2581 artifact has the wrong revision or root schema";
        return false;
    }

    static const std::set<wxString> ALLOWED_TOP_LEVEL = {
        wxS( "Content" ), wxS( "LogisticHeader" ), wxS( "HistoryRecord" ),
        wxS( "Bom" ), wxS( "Ecad" ), wxS( "Avl" )
    };
    std::map<wxString, size_t> topLevelCounts;
    wxXmlNode*                 content = nullptr;

    for( wxXmlNode* child = root->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() != wxXML_ELEMENT_NODE )
            continue;

        if( !ALLOWED_TOP_LEVEL.contains( child->GetName() ) )
        {
            aError = "IPC-2581 artifact contains an unexpected top-level element";
            return false;
        }

        if( ++topLevelCounts[child->GetName()] > 1 )
        {
            aError = "IPC-2581 artifact contains a duplicate top-level section";
            return false;
        }

        if( child->GetName() == wxS( "Content" ) )
            content = child;
    }

    if( topLevelCounts[wxS( "Content" )] != 1
        || topLevelCounts[wxS( "LogisticHeader" )] != 1
        || topLevelCounts[wxS( "HistoryRecord" )] != 1
        || topLevelCounts[wxS( "Ecad" )] != 1 )
    {
        aError = "IPC-2581 artifact is missing a required top-level section";
        return false;
    }

    const wxString expectedStep = wxString::FromUTF8( aPlan.at( "fileStem" )
                                                               .get<std::string>()
                                                               .c_str() );
    bool contentStepMatches = false;

    for( wxXmlNode* child = content->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() == wxXML_ELEMENT_NODE && child->GetName() == wxS( "StepRef" )
            && child->GetAttribute( wxS( "name" ) ) == expectedStep )
        {
            contentStepMatches = true;
        }
    }

    if( !contentStepMatches )
    {
        aError = "IPC-2581 Content does not reference the planned board step";
        return false;
    }

    constexpr size_t MAX_IPC2581_XML_NODES = 2'000'000;
    constexpr size_t MAX_IPC2581_XML_DEPTH = 256;
    std::vector<std::pair<wxXmlNode*, size_t>> pending = { { root, 0 } };
    std::set<std::string> componentReferences;
    std::set<std::string> bomReferences;
    size_t nodeCount = 0;
    size_t cadHeaders = 0;
    size_t boardSteps = 0;
    size_t stackups = 0;
    size_t conductorLayers = 0;
    size_t outlineLayers = 0;
    size_t softwarePackages = 0;

    while( !pending.empty() )
    {
        const auto [first, depth] = pending.back();
        pending.pop_back();

        if( depth > MAX_IPC2581_XML_DEPTH )
        {
            aError = "IPC-2581 artifact exceeds the XML nesting limit";
            return false;
        }

        for( wxXmlNode* node = first; node; node = node->GetNext() )
        {
            if( ++nodeCount > MAX_IPC2581_XML_NODES )
            {
                aError = "IPC-2581 artifact exceeds the XML node limit";
                return false;
            }

            if( node->GetChildren() )
                pending.emplace_back( node->GetChildren(), depth + 1 );

            if( node->GetType() != wxXML_ELEMENT_NODE )
                continue;

            const wxString name = node->GetName();

            if( name == wxS( "SoftwarePackage" ) )
            {
                ++softwarePackages;

                if( node->GetAttribute( wxS( "name" ) ) != wxS( "KiCad" )
                    || node->GetAttribute( wxS( "revision" ) ) != wxS( "10.0.4" )
                    || node->GetAttribute( wxS( "vendor" ) ) != wxS( "KiCad EDA" ) )
                {
                    aError = "IPC-2581 artifact has the wrong native producer identity";
                    return false;
                }
            }
            else if( name == wxS( "CadHeader" ) )
            {
                ++cadHeaders;

                if( node->GetAttribute( wxS( "units" ) ) != wxS( "MILLIMETER" ) )
                {
                    aError = "IPC-2581 artifact is not expressed in millimetres";
                    return false;
                }
            }
            else if( name == wxS( "Step" ) )
            {
                if( node->GetAttribute( wxS( "name" ) ) == expectedStep
                    && node->GetAttribute( wxS( "type" ) ) == wxS( "BOARD" ) )
                {
                    ++boardSteps;
                }
            }
            else if( name == wxS( "Stackup" ) )
            {
                ++stackups;
            }
            else if( name == wxS( "Layer" ) )
            {
                if( node->GetAttribute( wxS( "layerFunction" ) ) == wxS( "CONDUCTOR" ) )
                    ++conductorLayers;
                else if( node->GetAttribute( wxS( "layerFunction" ) )
                         == wxS( "BOARD_OUTLINE" ) )
                    ++outlineLayers;
            }
            else if( name == wxS( "Component" ) )
            {
                const std::string reference =
                        node->GetAttribute( wxS( "refDes" ) ).ToStdString();

                if( reference.empty() || !componentReferences.emplace( reference ).second )
                {
                    aError = "IPC-2581 artifact has an empty or duplicate component reference";
                    return false;
                }
            }
            else if( name == wxS( "RefDes" ) )
            {
                const std::string reference =
                        node->GetAttribute( wxS( "name" ) ).ToStdString();

                if( reference.empty() || !bomReferences.emplace( reference ).second )
                {
                    aError = "IPC-2581 artifact has an empty or duplicate BOM reference";
                    return false;
                }
            }
        }
    }

    if( softwarePackages != 1 || cadHeaders != 1 || boardSteps != 1 || stackups != 1
        || conductorLayers < 2 || outlineLayers != 1 )
    {
        aError = "IPC-2581 artifact is missing its native board, stackup, or producer structure";
        return false;
    }

    if( componentReferences != bomReferences )
    {
        aError = "IPC-2581 component and native BOM reference sets differ";
        return false;
    }

    for( const JSON& expected : aPlan.at( "expectedBomReferences" ) )
    {
        if( !componentReferences.contains( expected.get<std::string>() ) )
        {
            aError = "IPC-2581 artifact is missing a compiled KDS component reference";
            return false;
        }
    }

    return true;
}


bool validateOdbArchive( const std::filesystem::path& aPath, const JSON& aPlan,
                         std::string& aError )
{
    constexpr size_t MAX_ODB_ARCHIVE_ENTRIES = 20'000;
    constexpr uintmax_t MAX_ODB_CONTROL_FILE_BYTES = 32ULL * 1024ULL * 1024ULL;
    constexpr size_t MAX_ODB_PATH_BYTES = 1024;
    constexpr size_t MAX_ODB_PATH_PARTS = 32;
    static const std::set<std::string> ALLOWED_ROOTS = {
        "matrix", "steps", "input", "symbols", "fonts", "misc", "wheels", "user"
    };
    static const std::set<std::string> REQUIRED_DIRECTORIES = {
        "matrix/", "steps/", "steps/pcb/", "steps/pcb/layers/",
        "steps/pcb/netlists/", "steps/pcb/netlists/cadnet/", "steps/pcb/eda/",
        "fonts/", "misc/"
    };
    static const std::set<std::string> REQUIRED_FILES = {
        "matrix/matrix",
        "steps/pcb/stephdr",
        "steps/pcb/profile",
        "steps/pcb/eda/data",
        "steps/pcb/netlists/cadnet/netlist",
        "steps/pcb/layers/f.cu/features",
        "steps/pcb/layers/b.cu/features",
        "steps/pcb/layers/edge.cuts/features",
        "fonts/standard",
        "misc/info"
    };
    const auto safePath = [&]( const std::string& aName, bool aDirectory )
    {
        if( aName.empty() || aName.size() > MAX_ODB_PATH_BYTES || aName.front() == '/'
            || aName.find( '\\' ) != std::string::npos
            || aName.find( ':' ) != std::string::npos
            || aName.find( "//" ) != std::string::npos
            || aName.ends_with( '/' ) != aDirectory )
        {
            return false;
        }

        for( unsigned char character : aName )
        {
            if( character < 0x20 || character > 0x7e )
                return false;
        }

        size_t parts = 0;
        size_t start = 0;
        std::string root;

        while( start < aName.size() )
        {
            const size_t end = aName.find( '/', start );
            const size_t length = ( end == std::string::npos ? aName.size() : end ) - start;
            const std::string_view part( aName.data() + start, length );

            if( part.empty() || part == "." || part == ".." || ++parts > MAX_ODB_PATH_PARTS )
                return false;

            if( root.empty() )
                root.assign( part );

            if( end == std::string::npos )
                break;

            start = end + 1;
        }

        return ALLOWED_ROOTS.contains( root );
    };
    const auto lowerAscii = []( std::string aValue )
    {
        std::transform( aValue.begin(), aValue.end(), aValue.begin(),
                        []( unsigned char aCharacter )
                        {
                            return static_cast<char>( std::tolower( aCharacter ) );
                        } );
        return aValue;
    };
    const std::string nativePath = aPath.string();
    wxFFileInputStream archiveInput( wxString::FromUTF8( nativePath.c_str() ) );

    if( !archiveInput.IsOk() )
    {
        aError = "could not read ODB++ archive";
        return false;
    }

    wxZipInputStream archive( archiveInput, wxConvUTF8 );
    const int advertisedEntries = archive.GetTotalEntries();

    if( !archive.IsOk() || advertisedEntries <= 0
        || static_cast<size_t>( advertisedEntries ) > MAX_ODB_ARCHIVE_ENTRIES
        || !archive.GetComment().empty() )
    {
        aError = "ODB++ ZIP has invalid archive metadata or entry count";
        return false;
    }

    std::set<std::string> archivePaths;
    std::set<std::string> caseFoldedPaths;
    std::map<std::string, std::string> inspectedFiles;
    uintmax_t totalBytes = 0;
    size_t entryCount = 0;
    std::array<char, 64 * 1024> readBuffer;

    while( auto entry = std::unique_ptr<wxZipEntry>( archive.GetNextEntry() ) )
    {
        if( ++entryCount > MAX_ODB_ARCHIVE_ENTRIES )
        {
            aError = "ODB++ ZIP exceeds the bounded entry count";
            return false;
        }

        const bool directory = entry->IsDir();
        std::string path = entry->GetInternalName().ToStdString();

        if( directory && !path.ends_with( '/' ) )
            path.push_back( '/' );

        const int flags = entry->GetFlags();
        constexpr int ALLOWED_FLAGS = wxZIP_DEFLATE_MASK | wxZIP_SUMS_FOLLOW
                                      | wxZIP_LANG_ENC_UTF8;

        if( !safePath( path, directory ) )
        {
            aError = "ODB++ ZIP contains an unsafe entry path: " + path;
            return false;
        }

        if( !archivePaths.emplace( path ).second
            || !caseFoldedPaths.emplace( lowerAscii( path ) ).second )
        {
            aError = "ODB++ ZIP contains a duplicate or case-ambiguous entry: " + path;
            return false;
        }

        if( ( flags & ~ALLOWED_FLAGS ) != 0
            || ( flags & ( wxZIP_ENCRYPTED | wxZIP_STRONG_ENC ) ) != 0 )
        {
            aError = "ODB++ ZIP contains unsupported or encrypted flags: " + path;
            return false;
        }

        if( ( entry->GetMethod() != wxZIP_METHOD_STORE
              && entry->GetMethod() != wxZIP_METHOD_DEFLATE )
            || !entry->GetComment().empty() || entry->GetExtraLen() != 0
            || entry->GetLocalExtraLen() != 0 )
        {
            aError = "ODB++ ZIP contains unsupported compression or entry metadata: " + path;
            return false;
        }

        const wxFileOffset size = entry->GetSize();
        const wxFileOffset compressedSize = entry->GetCompressedSize();

        if( size < 0 || compressedSize < 0
            || static_cast<uintmax_t>( size ) > MAX_FABRICATION_FILE_BYTES
            || static_cast<uintmax_t>( compressedSize ) > MAX_FABRICATION_FILE_BYTES
            || totalBytes > MAX_FABRICATION_TOTAL_BYTES - static_cast<uintmax_t>( size )
            || ( directory && size != 0 ) )
        {
            aError = "ODB++ ZIP exceeds the bounded entry or expanded package size";
            return false;
        }

        if( directory )
            continue;

        const bool inspect = REQUIRED_FILES.contains( path ) || path.ends_with( "/components" );

        if( inspect && static_cast<uintmax_t>( size ) > MAX_ODB_CONTROL_FILE_BYTES )
        {
            aError = "ODB++ control or component file exceeds its inspection limit";
            return false;
        }

        std::string content;

        if( inspect )
            content.reserve( static_cast<size_t>( size ) );

        uintmax_t actualBytes = 0;

        while( true )
        {
            archive.Read( readBuffer.data(), readBuffer.size() );
            const size_t read = archive.LastRead();

            if( read == 0 )
                break;

            if( actualBytes > MAX_FABRICATION_FILE_BYTES - read
                || totalBytes > MAX_FABRICATION_TOTAL_BYTES - actualBytes - read )
            {
                aError = "ODB++ ZIP expanded data exceeds the bounded package size";
                return false;
            }

            actualBytes += read;

            if( inspect )
                content.append( readBuffer.data(), read );
        }

        if( actualBytes != static_cast<uintmax_t>( size ) || !archive.CloseEntry() )
        {
            aError = "ODB++ ZIP entry is truncated or failed its integrity check";
            return false;
        }

        totalBytes += actualBytes;

        if( inspect )
            inspectedFiles.emplace( path, std::move( content ) );
    }

    if( entryCount != static_cast<size_t>( advertisedEntries ) || !archive.Eof() )
    {
        aError = "ODB++ ZIP central directory differs from its readable entries";
        return false;
    }

    for( const std::string& required : REQUIRED_DIRECTORIES )
    {
        if( !archivePaths.contains( required ) )
        {
            aError = "ODB++ ZIP is missing a required directory";
            return false;
        }
    }

    for( const std::string& required : REQUIRED_FILES )
    {
        if( !inspectedFiles.contains( required ) )
        {
            aError = "ODB++ ZIP is missing a required manufacturing file";
            return false;
        }
    }

    const std::string& matrix = inspectedFiles.at( "matrix/matrix" );
    const std::string& info = inspectedFiles.at( "misc/info" );
    const std::string& stepHeader = inspectedFiles.at( "steps/pcb/stephdr" );
    const std::string& profile = inspectedFiles.at( "steps/pcb/profile" );
    const std::string& eda = inspectedFiles.at( "steps/pcb/eda/data" );
    const std::string& netlist = inspectedFiles.at( "steps/pcb/netlists/cadnet/netlist" );
    const auto validFeatureFile = [&]( const char* aName )
    {
        const std::string& source = inspectedFiles.at( aName );
        return source.starts_with( "UNITS=MM\n" )
               && source.find( "#Num Features" ) != std::string::npos;
    };

    if( matrix.find( "STEP {" ) == std::string::npos
        || matrix.find( "NAME=PCB" ) == std::string::npos
        || matrix.find( "NAME=F.CU" ) == std::string::npos
        || matrix.find( "NAME=B.CU" ) == std::string::npos
        || matrix.find( "NAME=EDGE.CUTS" ) == std::string::npos
        || !info.starts_with( "JOB_NAME=job\nUNITS=MM\nODB_VERSION_MAJOR=8\n"
                              "ODB_VERSION_MINOR=1\nODB_SOURCE=KiCad EDA\n" )
        || info.find( "SAVE_APP=KiCad EDA 10.0.4" ) == std::string::npos
        || stepHeader.find( "LEFT_ACTIVE=" ) == std::string::npos
        || stepHeader.find( "X_ORIGIN=" ) == std::string::npos
        || !stepHeader.ends_with( "UNITS=MM\n" )
        || !profile.starts_with( "UNITS=MM\n" )
        || profile.find( "#Num Features" ) == std::string::npos
        || eda.find( "HDR KiCad EDA 10.0.4" ) == std::string::npos
        || eda.find( "UNITS=MM\n" ) == std::string::npos
        || !netlist.starts_with( "H optimize " )
        || netlist.find( "#Netlist points" ) == std::string::npos
        || inspectedFiles.at( "fonts/standard" ).find( "XSIZE" ) == std::string::npos
        || !validFeatureFile( "steps/pcb/layers/f.cu/features" )
        || !validFeatureFile( "steps/pcb/layers/b.cu/features" )
        || !validFeatureFile( "steps/pcb/layers/edge.cuts/features" ) )
    {
        aError = "ODB++ ZIP failed its native 8.1 millimetre manufacturing structure checks";
        return false;
    }

    std::set<std::string> componentReferences;

    for( const auto& [path, source] : inspectedFiles )
    {
        if( !path.ends_with( "/components" ) )
            continue;

        std::istringstream lines( source );
        std::string line;

        while( std::getline( lines, line ) )
        {
            if( !line.starts_with( "CMP " ) )
                continue;

            std::istringstream fields( line );
            std::string record;
            std::string index;
            std::string x;
            std::string y;
            std::string rotation;
            std::string mirror;
            std::string reference;
            std::string package;

            if( !( fields >> record >> index >> x >> y >> rotation >> mirror >> reference
                   >> package )
                || record != "CMP" || reference.empty()
                || !componentReferences.emplace( reference ).second )
            {
                aError = "ODB++ ZIP has a malformed or duplicate component reference";
                return false;
            }
        }
    }

    for( const JSON& expected : aPlan.at( "expectedBomReferences" ) )
    {
        if( !componentReferences.contains( expected.get<std::string>() ) )
        {
            aError = "ODB++ ZIP is missing a compiled KDS component reference";
            return false;
        }
    }

    return true;
}


bool readCsvReferences( const std::filesystem::path& aPath,
                        std::set<std::string>& aReferences, std::string& aError )
{
    std::ifstream input( aPath, std::ios::binary );
    std::string line;

    if( !input || !std::getline( input, line ) )
    {
        aError = "could not read fabrication CSV header";
        return false;
    }

    while( std::getline( input, line ) )
    {
        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.empty() )
            continue;

        std::string reference;

        if( line.front() == '"' )
        {
            bool closed = false;

            for( size_t index = 1; index < line.size(); ++index )
            {
                if( line[index] != '"' )
                {
                    reference.push_back( line[index] );
                    continue;
                }

                if( index + 1 < line.size() && line[index + 1] == '"' )
                {
                    reference.push_back( '"' );
                    ++index;
                    continue;
                }

                closed = index + 1 < line.size() && line[index + 1] == ',';
                break;
            }

            if( !closed )
            {
                aError = "fabrication CSV has a malformed quoted reference";
                return false;
            }
        }
        else
        {
            const size_t comma = line.find( ',' );

            if( comma == std::string::npos )
            {
                aError = "fabrication CSV row has no reference column";
                return false;
            }

            reference = line.substr( 0, comma );
        }

        if( reference.empty() || !aReferences.emplace( std::move( reference ) ).second )
        {
            aError = "fabrication CSV has an empty or duplicate reference";
            return false;
        }
    }

    if( input.bad() )
    {
        aError = "could not read the complete fabrication CSV";
        return false;
    }

    return true;
}


bool hashFabricationFile( const std::filesystem::path& aPath, std::string& aDigest )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
        return false;

    std::array<picosha2::byte_t, picosha2::k_digest_size> digest{};
    picosha2::hash256( input, digest.begin(), digest.end() );
    aDigest = picosha2::bytes_to_hex_string( digest );
    return !input.bad() && aDigest.size() == 64;
}


bool validateExactNativeFormat( const wxFileName& aPath, const std::string& aRoot,
                                const std::string& aVersion, std::string& aError )
{
    wxFile input( aPath.GetFullPath(), wxFile::read );

    if( !input.IsOpened() )
    {
        aError = "could not open native KiCad input " + aPath.GetFullName().ToStdString();
        return false;
    }

    const wxFileOffset length = input.Length();

    if( length <= 0 )
    {
        aError = "native KiCad input is empty: " + aPath.GetFullName().ToStdString();
        return false;
    }

    const size_t headerBytes =
            std::min<size_t>( static_cast<size_t>( length ), 16 * 1024 );
    std::string header( headerBytes, '\0' );

    if( input.Read( header.data(), header.size() )
        != static_cast<wxFileOffset>( header.size() ) )
    {
        aError = "could not read native KiCad input header";
        return false;
    }

    const size_t first = header.find_first_not_of( " \t\r\n" );
    const std::string root = "(" + aRoot;
    const std::string version = "(version " + aVersion + ")";

    if( first == std::string::npos || header.compare( first, root.size(), root ) != 0
        || header.find( version, first + root.size() ) == std::string::npos )
    {
        aError = aPath.GetFullName().ToStdString()
                 + " is not in the exact native KiCad 10.0.4 " + aRoot
                 + " format (expected version " + aVersion + ")";
        return false;
    }

    return true;
}


bool validateBoardFabricationIntent( const wxFileName& aBoard, const JSON& aIr,
                                     const JSON& aPlan, std::string& aError )
{
    wxFile input( aBoard.GetFullPath(), wxFile::read );

    if( !input.IsOpened() )
    {
        aError = "could not open the native board for fabrication-intent validation";
        return false;
    }

    const wxFileOffset length = input.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_STATE_BYTES ) )
    {
        aError = "native board must contain 1 byte to 64 MiB for fabrication";
        return false;
    }

    std::string source( static_cast<size_t>( length ), '\0' );

    if( input.Read( source.data(), source.size() ) != length )
    {
        aError = "could not read the complete native board for fabrication";
        return false;
    }

    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( source ), &aError );

    if( !document || document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "kicad_pcb" )
    {
        if( aError.empty() )
            aError = "native board has no unique kicad_pcb root";

        return false;
    }

    const auto directList = [&]( size_t aParent, const std::string& aHead )
    {
        size_t match = KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

        for( size_t child : document->Nodes().at( aParent ).children )
        {
            if( document->Nodes().at( child ).kind
                        != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
                || document->ListHead( child ) != aHead )
            {
                continue;
            }

            if( match != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
                return KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE;

            match = child;
        }

        return match;
    };
    const auto scalar = [&]( size_t aParent, const std::string& aHead )
    {
        const size_t field = directList( aParent, aHead );

        if( field == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
            || document->Nodes().at( field ).children.size() < 2
            || document->Nodes().at( document->Nodes().at( field ).children[1] ).kind
                       == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
        {
            return std::string();
        }

        return document->AtomText( document->Nodes().at( field ).children[1] );
    };
    const auto numberMatches = []( const std::string& aText, double aExpected )
    {
        JSON number = JSON::parse( aText, nullptr, false );

        return !number.is_discarded() && number.is_number()
               && std::isfinite( number.get<double>() )
               && std::abs( number.get<double>() - aExpected ) <= 1e-9;
    };
    const size_t root = document->Roots().front();
    const size_t general = directList( root, "general" );
    const size_t layerTable = directList( root, "layers" );
    const size_t setup = directList( root, "setup" );
    const size_t stackup = setup == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
                                   ? KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
                                   : directList( setup, "stackup" );

    if( general == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
        || layerTable == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
        || stackup == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE )
    {
        aError = "native board is missing its general, layer-table, or physical stackup data";
        return false;
    }

    const JSON* desiredStackup = nullptr;

    for( const JSON& statement : aIr.at( "pcb" ) )
    {
        if( statement.value( "kind", "" ) == "stackup" )
        {
            desiredStackup = &statement;
            break;
        }
    }

    if( desiredStackup == nullptr )
    {
        aError = "compiled KDS has no physical stackup to validate";
        return false;
    }

    const double expectedBoardThickness =
            desiredStackup->at( "thicknessNm" ).get<double>() / 1000000.0;

    if( !numberMatches( scalar( general, "thickness" ), expectedBoardThickness ) )
    {
        aError = "native board thickness does not match the compiled KDS stackup";
        return false;
    }

    std::set<std::string> enabledLayers;

    for( size_t child : document->Nodes().at( layerTable ).children )
    {
        const auto& node = document->Nodes().at( child );

        if( node.kind != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || node.children.size() < 3 )
        {
            continue;
        }

        enabledLayers.emplace( document->AtomText( node.children[1] ) );

        if( node.children.size() >= 4
            && document->Nodes().at( node.children[3] ).kind
                       != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
        {
            enabledLayers.emplace( document->AtomText( node.children[3] ) );
        }
    }

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        if( !job.contains( "layers" ) )
            continue;

        for( const JSON& layer : job.at( "layers" ) )
        {
            if( !enabledLayers.contains( layer.get<std::string>() ) )
            {
                aError = "native board does not enable KDS fabrication layer "
                         + layer.get<std::string>();
                return false;
            }
        }
    }

    std::vector<size_t> nativeLayers;

    for( size_t child : document->Nodes().at( stackup ).children )
    {
        if( document->Nodes().at( child ).kind
                    == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            && document->ListHead( child ) == "layer" )
        {
            nativeLayers.push_back( child );
        }
    }

    const JSON& desiredLayers = desiredStackup->at( "layers" );

    if( nativeLayers.size() != desiredLayers.size() )
    {
        aError = "native board physical stackup layer count does not match compiled KDS";
        return false;
    }

    for( size_t index = 0; index < nativeLayers.size(); ++index )
    {
        const size_t nativeLayer = nativeLayers[index];
        const auto& node = document->Nodes().at( nativeLayer );
        const JSON& desired = desiredLayers[index];
        const std::string category = desired.at( "category" ).get<std::string>();
        const std::string identity = node.children.size() >= 2
                                             ? document->AtomText( node.children[1] )
                                             : std::string();
        const bool identityMatches = category == "dielectric"
                                             ? identity.starts_with( "dielectric " )
                                             : identity == desired.value( "layer", "" );

        if( !identityMatches
            || scalar( nativeLayer, "type" )
                       != desired.at( "typeName" ).get<std::string>() )
        {
            aError = "native board physical stackup order or layer type differs from KDS";
            return false;
        }

        const int64_t thicknessNm = desired.at( "thicknessNm" ).get<int64_t>();

        if( thicknessNm > 0 )
        {
            const size_t thickness = directList( nativeLayer, "thickness" );

            if( thickness == KICHAD::LOSSLESS_SEXPR_DOCUMENT::NO_NODE
                || document->Nodes().at( thickness ).children.size() < 2
                || !numberMatches(
                        document->AtomText( document->Nodes().at( thickness ).children[1] ),
                        static_cast<double>( thicknessNm ) / 1000000.0 ) )
            {
                aError = "native board physical layer thickness differs from KDS";
                return false;
            }

            if( category == "dielectric" )
            {
                const auto& thicknessNode = document->Nodes().at( thickness );
                const bool nativeLocked =
                        thicknessNode.children.size() == 3
                        && document->AtomText( thicknessNode.children[2] ) == "locked";

                if( nativeLocked != desired.at( "locked" ).get<bool>() )
                {
                    aError = "native board dielectric thickness lock differs from KDS";
                    return false;
                }
            }
        }

        for( const auto& [nativeName, desiredName] :
             std::array<std::pair<const char*, const char*>, 2>{
                     std::pair{ "material", "material" },
                     std::pair{ "color", "color" } } )
        {
            const std::string desiredText = desired.value( desiredName, "" );

            if( !desiredText.empty() && scalar( nativeLayer, nativeName ) != desiredText )
            {
                aError = std::string( "native board physical layer " ) + nativeName
                         + " differs from KDS";
                return false;
            }
        }

        for( const auto& [nativeName, desiredName] :
             std::array<std::pair<const char*, const char*>, 2>{
                     std::pair{ "epsilon_r", "epsilonR" },
                     std::pair{ "loss_tangent", "lossTangent" } } )
        {
            const double desiredNumber = desired.value( desiredName, 0.0 );

            if( desiredNumber > 0.0
                && !numberMatches( scalar( nativeLayer, nativeName ), desiredNumber ) )
            {
                aError = std::string( "native board physical layer " ) + nativeName
                         + " differs from KDS";
                return false;
            }
        }
    }

    const std::string expectedImpedance =
            desiredStackup->at( "impedanceControlled" ).get<bool>() ? "yes" : "no";
    const std::string expectedConnector = desiredStackup->at( "edgeConnector" );
    const std::string nativeConnector = scalar( stackup, "edge_connector" );
    const std::string nativePlating = scalar( stackup, "edge_plating" );

    if( scalar( stackup, "copper_finish" )
                    != desiredStackup->at( "finish" ).get<std::string>()
        || scalar( stackup, "dielectric_constraints" ) != expectedImpedance
        || ( expectedConnector == "none" ? !nativeConnector.empty()
                                          : nativeConnector != expectedConnector )
        || ( desiredStackup->at( "edgePlating" ).get<bool>() ? nativePlating != "yes"
                                                              : !nativePlating.empty() ) )
    {
        aError = "native board fabrication policies differ from compiled KDS stackup";
        return false;
    }

    std::map<std::string, std::string> expectedFootprints;

    for( const JSON& component : aIr.at( "schematic" ).at( "components" ) )
    {
        if( component.contains( "footprint" ) && component.at( "footprint" ).is_string() )
        {
            expectedFootprints.emplace( component.at( "reference" ).get<std::string>(),
                                        component.at( "footprint" ).get<std::string>() );
        }
    }

    std::map<std::string, std::string> nativeFootprints;

    for( size_t child : document->Nodes().at( root ).children )
    {
        if( document->Nodes().at( child ).kind
                    != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
            || document->ListHead( child ) != "footprint" )
        {
            continue;
        }

        const auto& footprintNode = document->Nodes().at( child );
        bool boardOnly = false;
        std::string reference;

        for( size_t footprintChild : footprintNode.children )
        {
            if( document->Nodes().at( footprintChild ).kind
                != KICHAD::LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST )
            {
                continue;
            }

            const std::string head = document->ListHead( footprintChild );
            const auto& field = document->Nodes().at( footprintChild );

            if( head == "attr" )
            {
                for( size_t value = 1; value < field.children.size(); ++value )
                    boardOnly = boardOnly || document->AtomText( field.children[value] )
                                                     == "board_only";
            }
            else if( head == "property" && field.children.size() >= 3
                     && document->AtomText( field.children[1] ) == "Reference" )
            {
                reference = document->AtomText( field.children[2] );
            }
        }

        if( boardOnly )
            continue;

        if( footprintNode.children.size() < 2 || reference.empty()
            || !nativeFootprints
                        .emplace( reference, document->AtomText( footprintNode.children[1] ) )
                        .second )
        {
            aError = "native board has an unreferenced or duplicate schematic footprint";
            return false;
        }
    }

    if( nativeFootprints != expectedFootprints )
    {
        aError = "native board schematic-footprint inventory differs from compiled KDS";
        return false;
    }

    return true;
}


bool validateFabricationArtifacts( const wxFileName& aStaging, const JSON& aPlan,
                                   JSON& aArtifacts, std::string& aError )
{
    const std::filesystem::path root( aStaging.GetFullPath().ToStdString() );
    std::map<std::string, size_t> counts;
    std::set<std::string> actualGerberFiles;
    std::set<std::string> jobGerberFiles;
    std::set<std::string> bomReferences;
    std::set<std::string> placementReferences;
    std::set<std::string> allowedDirectories;
    size_t files = 0;
    uintmax_t totalBytes = 0;
    std::error_code filesystemError;
    aArtifacts = JSON::array();

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        std::filesystem::path directory =
                job.at( "directory" ).get<bool>()
                        ? std::filesystem::path( job.at( "relativePath" ).get<std::string>() )
                        : std::filesystem::path( job.at( "relativePath" ).get<std::string>() )
                                  .parent_path();

        while( !directory.empty() )
        {
            allowedDirectories.emplace( directory.generic_string() );
            directory = directory.parent_path();
        }
    }

    for( std::filesystem::recursive_directory_iterator iterator( root, filesystemError ), end;
         !filesystemError && iterator != end; iterator.increment( filesystemError ) )
    {
        const std::filesystem::file_status status = iterator->symlink_status( filesystemError );

        if( filesystemError )
            break;

        if( std::filesystem::is_symlink( status ) )
        {
            aError = "fabrication staging contains a symbolic link";
            return false;
        }

        if( std::filesystem::is_directory( status ) )
        {
            const std::filesystem::path relative =
                    std::filesystem::relative( iterator->path(), root, filesystemError );

            if( filesystemError || !allowedDirectories.contains( relative.generic_string() ) )
            {
                aError = "fabrication staging contains an unexpected directory";
                return false;
            }

            continue;
        }

        if( !std::filesystem::is_regular_file( status ) )
        {
            aError = "fabrication staging contains a non-regular file";
            return false;
        }

        const uintmax_t bytes = iterator->file_size( filesystemError );

        if( filesystemError || bytes == 0 || bytes > MAX_FABRICATION_FILE_BYTES
            || ++files > MAX_FABRICATION_FILES
            || totalBytes > MAX_FABRICATION_TOTAL_BYTES - bytes )
        {
            aError = "fabrication artifacts exceed the bounded file or package limits";
            return false;
        }

        totalBytes += bytes;
        const std::filesystem::path relative =
                std::filesystem::relative( iterator->path(), root, filesystemError );

        if( filesystemError || relative.empty() || relative.is_absolute() )
        {
            aError = "fabrication artifact path is not project-relative";
            return false;
        }

        const std::string path = relative.generic_string();
        const std::string extension = iterator->path().extension().string();
        std::string kind;
        std::string prefix;
        std::string suffix;

        if( !readFileEdges( iterator->path(), prefix, suffix ) )
        {
            aError = "could not inspect fabrication artifact " + path;
            return false;
        }

        if( path.starts_with( "gerbers/" ) && extension == ".gbr" )
        {
            kind = "gerbers";
            actualGerberFiles.emplace( iterator->path().filename().string() );

            if( prefix.find( "G04" ) == std::string::npos
                || prefix.find( "TF.GenerationSoftware,KiCad" ) == std::string::npos
                || suffix.find( "M02*" ) == std::string::npos )
            {
                aError = "Gerber artifact failed native signature validation: " + path;
                return false;
            }
        }
        else if( path.starts_with( "gerbers/" ) && extension == ".gbrjob" )
        {
            kind = "gerber_job";
            std::ifstream jobInput( iterator->path(), std::ios::binary );
            JSON job = JSON::parse( jobInput, nullptr, false );

            if( job.is_discarded() || !job.is_object() || !job.contains( "Header" )
                || !job["Header"].is_object()
                || !job["Header"].contains( "GenerationSoftware" )
                || !job["Header"]["GenerationSoftware"].is_object()
                || job["Header"]["GenerationSoftware"].value( "Vendor", "" ) != "KiCad"
                || job["Header"]["GenerationSoftware"].value( "Application", "" )
                           != "Pcbnew"
                || !job.contains( "FilesAttributes" )
                || !job["FilesAttributes"].is_array() )
            {
                aError = "Gerber job artifact failed native schema validation";
                return false;
            }

            std::set<std::string> jobFiles;

            for( const JSON& attribute : job["FilesAttributes"] )
            {
                const std::string jobPath =
                        attribute.is_object() && attribute.contains( "Path" )
                                && attribute["Path"].is_string()
                                ? attribute["Path"].get<std::string>()
                                : std::string();
                const std::filesystem::path jobFile( jobPath );

                if( !attribute.is_object() || !attribute.contains( "Path" )
                    || !attribute["Path"].is_string()
                    || jobFile.empty() || jobFile.is_absolute() || jobFile.has_parent_path()
                    || jobFile.extension() != ".gbr"
                    || !jobFiles.emplace( jobPath ).second )
                {
                    aError = "Gerber job contains an invalid or duplicate file entry";
                    return false;
                }
            }

            if( jobFiles.size() == 0 )
            {
                aError = "Gerber job contains no production layer files";
                return false;
            }

            jobGerberFiles = std::move( jobFiles );
        }
        else if( path.starts_with( "drill/" ) && extension == ".drl" )
        {
            kind = "drill_file";

            if( !prefix.starts_with( "M48" ) || suffix.find( "M30" ) == std::string::npos )
            {
                aError = "Excellon artifact failed signature validation: " + path;
                return false;
            }
        }
        else if( path.starts_with( "drill/" ) && extension == ".pdf" )
        {
            kind = "drill_map";

            if( !prefix.starts_with( "%PDF-" ) || suffix.find( "%%EOF" ) == std::string::npos )
            {
                aError = "drill map failed PDF signature validation: " + path;
                return false;
            }
        }
        else if( path == "drill/drill-report.rpt" )
        {
            kind = "drill_report";

            if( prefix.find( "Drill report" ) == std::string::npos )
            {
                aError = "drill report failed native signature validation";
                return false;
            }
        }
        else if( path.starts_with( "electrical-test/" ) && extension == ".d356" )
        {
            kind = "ipcd356";

            if( !prefix.starts_with( "P  CODE 00" )
                || prefix.find( "P  UNITS CUST 0" ) == std::string::npos
                || ( !suffix.ends_with( "999\n" ) && !suffix.ends_with( "999\r\n" ) ) )
            {
                aError = "IPC-D-356 artifact failed native signature validation";
                return false;
            }
        }
        else if( path.starts_with( "manufacturing/" )
                 && path.ends_with( ".ipc2581.xml" ) )
        {
            kind = "ipc2581";

            if( !validateIpc2581Artifact( iterator->path(), aPlan, aError ) )
                return false;
        }
        else if( path.starts_with( "manufacturing/" ) && path.ends_with( ".odb.zip" ) )
        {
            kind = "odbpp";

            if( !prefix.starts_with( "PK\x03\x04" )
                || suffix.find( "PK\x05\x06" ) == std::string::npos )
            {
                aError = "ODB++ artifact failed ZIP signature validation";
                return false;
            }

            if( !validateOdbArchive( iterator->path(), aPlan, aError ) )
                return false;
        }
        else if( path.starts_with( "assembly/" ) && path.ends_with( "-positions.csv" ) )
        {
            kind = "pick_place";

            if( !prefix.starts_with( "Ref,Val,Package,PosX,PosY,Rot,Side" ) )
            {
                aError = "position artifact has the wrong KiCad CSV schema";
                return false;
            }

            if( !readCsvReferences( iterator->path(), placementReferences, aError ) )
                return false;
        }
        else if( path.starts_with( "assembly/" ) && path.ends_with( "-bom.csv" ) )
        {
            kind = "bom";

            if( !prefix.starts_with( "Reference,Value,Footprint,Quantity,Manufacturer,MPN," ) )
            {
                aError = "BOM artifact has the wrong KDS sourcing schema";
                return false;
            }

            if( !readCsvReferences( iterator->path(), bomReferences, aError ) )
                return false;
        }
        else if( path.starts_with( "model/" ) && extension == ".step" )
        {
            kind = "step";

            if( !prefix.starts_with( "ISO-10303-21;" )
                || suffix.find( "END-ISO-10303-21;" ) == std::string::npos )
            {
                aError = "STEP artifact failed signature validation";
                return false;
            }
        }
        else if( path.starts_with( "documentation/" ) && extension == ".pdf" )
        {
            kind = "pdf";

            if( !prefix.starts_with( "%PDF-" ) || suffix.find( "%%EOF" ) == std::string::npos )
            {
                aError = "documentation artifact failed PDF signature validation";
                return false;
            }
        }
        else
        {
            aError = "fabrication staging contains an unexpected artifact: " + path;
            return false;
        }

        std::string digest;

        if( !hashFabricationFile( iterator->path(), digest ) )
        {
            aError = "could not hash fabrication artifact " + path;
            return false;
        }

        ++counts[kind];
        aArtifacts.push_back( { { "kind", kind },
                                { "path", path },
                                { "bytes", bytes },
                                { "sha256", std::move( digest ) } } );
    }

    if( filesystemError )
    {
        aError = "could not enumerate the complete fabrication staging directory";
        return false;
    }

    if( actualGerberFiles != jobGerberFiles )
    {
        aError = "Gerber job file does not enumerate the exact generated layer files";
        return false;
    }

    std::set<std::string> expectedBomReferences;
    std::set<std::string> expectedPlacementReferences;

    for( const JSON& reference : aPlan.at( "expectedBomReferences" ) )
        expectedBomReferences.emplace( reference.get<std::string>() );

    for( const JSON& reference : aPlan.at( "expectedPlacementReferences" ) )
        expectedPlacementReferences.emplace( reference.get<std::string>() );

    if( bomReferences != expectedBomReferences
        || placementReferences != expectedPlacementReferences )
    {
        aError = "fabrication BOM or placement references differ from compiled KDS";
        return false;
    }

    std::sort( aArtifacts.begin(), aArtifacts.end(),
               []( const JSON& aLeft, const JSON& aRight )
               {
                   return aLeft.at( "path" ).get<std::string>()
                          < aRight.at( "path" ).get<std::string>();
               } );

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        const std::string kind = job.at( "kind" ).get<std::string>();

        if( kind == "gerbers"
            && ( counts["gerbers"] != job.at( "layers" ).size()
                 || counts["gerber_job"] != 1 ) )
        {
            aError = "Gerber export did not produce one layer file per layer and one job file";
        }
        else if( kind == "drill"
                 && ( counts["drill_file"] == 0 || counts["drill_map"] == 0
                      || counts["drill_report"] != 1 ) )
            aError = "drill export is missing Excellon, map, or report artifacts";
        else if( kind == "ipcd356" && counts["ipcd356"] != 1 )
            aError = "IPC-D-356 export did not produce exactly one electrical-test artifact";
        else if( kind == "ipc2581" && counts["ipc2581"] != 1 )
            aError = "IPC-2581 export did not produce exactly one manufacturing artifact";
        else if( kind == "odbpp" && counts["odbpp"] != 1 )
            aError = "ODB++ export did not produce exactly one manufacturing archive";
        else if( kind == "pick_place" && counts["pick_place"] != 1 )
            aError = "position export did not produce exactly one CSV artifact";
        else if( kind == "bom" && counts["bom"] != 1 )
            aError = "BOM export did not produce exactly one CSV artifact";
        else if( kind == "step" && counts["step"] != 1 )
            aError = "STEP export did not produce exactly one model artifact";
        else if( kind == "pdf" && counts["pdf"] != 1 )
            aError = "documentation export did not produce exactly one PDF artifact";

        if( !aError.empty() )
            return false;
    }

    return true;
}


bool validateProjectLibraryTable( const std::string& aKind, const std::string& aSource,
                                  size_t& aRows, std::string& aError )
{
    if( aSource.empty() || aSource.size() > MAX_PROJECT_LIBRARY_TABLE_BYTES )
    {
        aError = "project library table must contain 1 byte to 1 MiB";
        return false;
    }

    LIBRARY_TABLE_PARSER parser;
    auto                 parsed = parser.ParseBuffer( aSource );

    if( !parsed.has_value() )
    {
        aError = "native library parser rejected the generated table: "
                 + parsed.error().description.ToStdString();
        return false;
    }

    const LIBRARY_TABLE_TYPE expected = aKind == "symbol" ? LIBRARY_TABLE_TYPE::SYMBOL
                                                           : LIBRARY_TABLE_TYPE::FOOTPRINT;

    if( parsed->type != expected || parsed->version != "7"
        || parsed->rows.size() > MAX_PROJECT_LIBRARY_ROWS )
    {
        aError = "generated project library table has the wrong type, version, or row count";
        return false;
    }

    std::set<std::string> nicknames;

    for( const LIBRARY_TABLE_ROW_IR& row : parsed->rows )
    {
        const bool safeUri = row.uri.starts_with( "${KIPRJMOD}/" )
                             && row.uri.find( "/../" ) == std::string::npos
                             && !row.uri.ends_with( "/.." );
        const bool suffix = aKind == "symbol" ? row.uri.ends_with( ".kicad_sym" )
                                               : row.uri.ends_with( ".pretty" );

        if( row.nickname.empty() || row.nickname.size() > 128
            || !nicknames.emplace( row.nickname ).second || row.type != "KiCad"
            || !safeUri || !suffix || !row.options.empty() || !row.description.empty()
            || row.disabled || row.hidden )
        {
            aError = "generated project library table contains an invalid native row";
            return false;
        }
    }

    aRows = parsed->rows.size();
    return true;
}


std::string expectedRootHead( const wxString& aExtension )
{
    if( aExtension == wxS( "kicad_sch" ) )
        return "kicad_sch";

    if( aExtension == wxS( "kicad_pcb" ) )
        return "kicad_pcb";

    if( aExtension == wxS( "kicad_sym" ) )
        return "kicad_symbol_lib";

    if( aExtension == wxS( "kicad_mod" ) )
        return "footprint";

    return {};
}


bool resolveProjectFile( const wxString& aProjectPath, const std::string& aRelativePath,
                         wxFileName& aResolved, std::string& aError )
{
    wxString   relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.size() > 4096 || aRelativePath.find( '\0' ) != std::string::npos
        || relative.IsEmpty() || candidate.IsAbsolute() )
    {
        aError = "path must be project-relative";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !candidate.FileExists() )
    {
        aError = "file does not exist";
        return false;
    }

    if( !canonicalizeExisting( candidate ) )
    {
        aError = "path could not be resolved";
        return false;
    }

    wxString candidatePath = candidate.GetFullPath();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    candidatePath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !candidatePath.StartsWith( rootPath ) )
    {
        aError = "path resolves outside the active project";
        return false;
    }

    if( !isInspectableExtension( candidate.GetExt() ) )
    {
        aError = "file type is not a supported KiCad s-expression format";
        return false;
    }

    aResolved = candidate;
    return true;
}


bool resolveProjectSidecar( const wxString& aProjectPath, const std::string& aRelativePath,
                            wxFileName& aResolved, std::string& aError )
{
    wxString   relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.size() > 4096 || aRelativePath.find( '\0' ) != std::string::npos
        || relative.IsEmpty() || candidate.IsAbsolute()
        || candidate.GetExt() != wxS( "kicad_kds" ) )
    {
        aError = "path must be a project-relative .kicad_kds file";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    wxFileName parent = wxFileName::DirName( candidate.GetPath() );

    if( !canonicalizeExisting( parent, true ) )
    {
        aError = "sidecar parent directory does not exist";
        return false;
    }

    wxString parentPath = parent.GetPathWithSep();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    parentPath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !parentPath.StartsWith( rootPath ) && parent.GetFullPath() != root.GetFullPath() )
    {
        aError = "path resolves outside the active project";
        return false;
    }

    aResolved.Assign( parent.GetFullPath(), candidate.GetFullName() );

    if( aResolved.FileExists() )
    {
        wxFileName canonicalTarget = aResolved;

        if( !canonicalizeExisting( canonicalTarget ) )
        {
            aError = "existing sidecar path could not be resolved";
            return false;
        }

        wxString targetPath = canonicalTarget.GetFullPath();

#ifdef __WXMSW__
        targetPath.MakeLower();
#endif

        if( !targetPath.StartsWith( rootPath ) )
        {
            aError = "existing sidecar resolves outside the active project";
            return false;
        }

        aResolved = canonicalTarget;
    }

    return true;
}


bool readDesignScriptSidecar( const wxFileName& aFile, std::string& aSource,
                              std::string& aError )
{
    wxFile file( aFile.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open the KiChad Design Script sidecar";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_SCRIPT_BYTES ) )
    {
        aError = "KiChad Design Script sidecars must contain 1 byte to 1 MiB";
        return false;
    }

    aSource.assign( static_cast<size_t>( length ), '\0' );

    if( file.Read( aSource.data(), static_cast<size_t>( length ) ) != length )
    {
        aError = "could not read the complete KiChad Design Script sidecar";
        aSource.clear();
        return false;
    }

    return true;
}


bool readJsonFile( const wxFileName& aPath, nlohmann::json& aDocument, std::string& aError )
{
    if( !aPath.FileExists() )
    {
        aDocument = nullptr;
        return true;
    }

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open KiChad managed-state data";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_STATE_BYTES ) )
    {
        aError = "KiChad managed-state data must contain 1 byte to 64 MiB";
        return false;
    }

    std::string text( static_cast<size_t>( length ), '\0' );

    if( file.Read( text.data(), text.size() ) != length )
    {
        aError = "could not read complete KiChad managed-state data";
        return false;
    }

    aDocument = nlohmann::json::parse( text, nullptr, false );

    if( aDocument.is_discarded() )
    {
        aError = "KiChad managed-state data is not valid JSON";
        return false;
    }

    return true;
}


bool writeJsonAtomically( const wxFileName& aPath, const nlohmann::json& aDocument,
                          std::string& aError )
{
    const std::string serialized = aDocument.dump( 2 ) + "\n";

    if( serialized.size() > MAX_DESIGN_STATE_BYTES )
    {
        aError = "KiChad managed-state data exceeds 64 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( serialized.data(), serialized.size() ) != serialized.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write KiChad managed-state data";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install KiChad managed-state data";
        return false;
    }

    nlohmann::json installed;

    if( !readJsonFile( aPath, installed, aError ) || installed != aDocument )
    {
        aError = "KiChad managed-state verification failed after installation";
        return false;
    }

    return true;
}


bool mergeRecoveryJournal( const nlohmann::json& aJournal,
                           const KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT& aContext,
                           nlohmann::json& aPreviousState, std::string& aError )
{
    if( aJournal.is_null() )
        return true;

    if( !aJournal.is_object() || aJournal.value( "format", "" ) != "kichad-kds-apply-journal"
        || aJournal.value( "version", 0 ) != 1
        || aJournal.value( "sourcePath", "" ) != aContext.sourcePath
        || aJournal.value( "boardPath", "" ) != aContext.boardPath
        || aJournal.value( "projectName", "" ) != aContext.projectName
        || !aJournal.contains( "previousState" ) || !aJournal.contains( "preparedState" )
        || !aJournal["preparedState"].is_object()
        || !aJournal["preparedState"].contains( "managedPcbItems" )
        || !aJournal["preparedState"]["managedPcbItems"].is_array() )
    {
        aError = "KiChad apply journal is malformed or belongs to a different design";
        return false;
    }

    nlohmann::json merged = aJournal["preparedState"];
    std::map<std::string, nlohmann::json> items;

    for( const nlohmann::json& item : merged["managedPcbItems"] )
    {
        if( !item.is_object() || !item.contains( "itemId" ) || !item["itemId"].is_string()
            || !items.emplace( item["itemId"].get<std::string>(), item ).second )
        {
            aError = "KiChad apply journal contains invalid prepared ownership";
            return false;
        }
    }

    const nlohmann::json& previous = aJournal["previousState"];

    if( !previous.is_null() )
    {
        if( !previous.is_object() || !previous.contains( "managedPcbItems" )
            || !previous["managedPcbItems"].is_array() )
        {
            aError = "KiChad apply journal contains invalid previous ownership";
            return false;
        }

        for( const nlohmann::json& item : previous["managedPcbItems"] )
        {
            if( !item.is_object() || !item.contains( "itemId" ) || !item["itemId"].is_string() )
            {
                aError = "KiChad apply journal contains invalid previous ownership";
                return false;
            }

            const std::string itemId = item["itemId"].get<std::string>();
            auto              existing = items.find( itemId );

            if( existing != items.end() && existing->second != item )
            {
                aError = "KiChad apply journal contains conflicting ownership";
                return false;
            }

            items.emplace( itemId, item );
        }
    }

    merged["sourcePath"] = aContext.sourcePath;
    merged["boardPath"] = aContext.boardPath;
    merged["projectName"] = aContext.projectName;
    merged["sourceSha256"] = aContext.sourceSha256;
    merged["managedPcbItems"] = nlohmann::json::array();

    for( const auto& [itemId, item] : items )
        merged["managedPcbItems"].push_back( item );

    std::map<std::string, nlohmann::json> schematicItems;
    const auto mergeSchematicItems = [&]( const nlohmann::json& aState ) -> bool
    {
        if( !aState.is_object() || !aState.contains( "managedSchematicItems" ) )
            return true;

        if( !aState["managedSchematicItems"].is_array() )
            return false;

        for( const nlohmann::json& item : aState["managedSchematicItems"] )
        {
            if( !item.is_object() || !item.contains( "file" ) || !item["file"].is_string()
                || !item.contains( "kind" ) || !item["kind"].is_string()
                || !item.contains( "logicalId" ) || !item["logicalId"].is_string()
                || !item.contains( "uuid" ) || !item["uuid"].is_string() )
            {
                return false;
            }

            const std::string key = item["file"].get<std::string>() + "\n"
                                    + item["uuid"].get<std::string>();
            auto existing = schematicItems.find( key );

            if( existing != schematicItems.end() && existing->second != item )
                return false;

            schematicItems.emplace( key, item );
        }

        return true;
    };

    if( !mergeSchematicItems( aJournal["preparedState"] )
        || !mergeSchematicItems( previous ) )
    {
        aError = "KiChad apply journal contains invalid schematic ownership";
        return false;
    }

    merged["managedSchematicItems"] = nlohmann::json::array();

    for( const auto& entry : schematicItems )
        merged["managedSchematicItems"].push_back( entry.second );

    aPreviousState = std::move( merged );
    return true;
}


kiapi::common::types::KiCadObjectType pcbObjectType( const std::string& aItemType )
{
    using namespace kiapi::common::types;

    if( aItemType == "footprint" )
        return KOT_PCB_FOOTPRINT;
    if( aItemType == "trace" )
        return KOT_PCB_TRACE;
    if( aItemType == "via" )
        return KOT_PCB_VIA;
    if( aItemType == "arc" )
        return KOT_PCB_ARC;
    if( aItemType == "zone" || aItemType == "rule_area" )
        return KOT_PCB_ZONE;
    if( aItemType == "shape" )
        return KOT_PCB_SHAPE;
    if( aItemType == "text" )
        return KOT_PCB_TEXT;
    if( aItemType == "dimension" )
        return KOT_PCB_DIMENSION;

    return KOT_UNKNOWN;
}


std::unique_ptr<google::protobuf::Message> newPcbItem( const std::string& aItemType )
{
    using namespace kiapi::board::types;

    if( aItemType == "footprint" )
        return std::make_unique<FootprintInstance>();
    if( aItemType == "trace" )
        return std::make_unique<Track>();
    if( aItemType == "via" )
        return std::make_unique<Via>();
    if( aItemType == "arc" )
        return std::make_unique<Arc>();
    if( aItemType == "zone" || aItemType == "rule_area" )
        return std::make_unique<Zone>();
    if( aItemType == "shape" )
        return std::make_unique<BoardGraphicShape>();
    if( aItemType == "text" )
        return std::make_unique<BoardText>();
    if( aItemType == "dimension" )
        return std::make_unique<Dimension>();

    return {};
}


std::string protobufFieldType( const google::protobuf::FieldDescriptor& aField )
{
    using TYPE = google::protobuf::FieldDescriptor::Type;

    switch( aField.type() )
    {
    case TYPE::TYPE_DOUBLE:   return "double";
    case TYPE::TYPE_FLOAT:    return "float";
    case TYPE::TYPE_INT64:    return "int64";
    case TYPE::TYPE_UINT64:   return "uint64";
    case TYPE::TYPE_INT32:    return "int32";
    case TYPE::TYPE_FIXED64:  return "fixed64";
    case TYPE::TYPE_FIXED32:  return "fixed32";
    case TYPE::TYPE_BOOL:     return "bool";
    case TYPE::TYPE_STRING:   return "string";
    case TYPE::TYPE_GROUP:    return "group";
    case TYPE::TYPE_MESSAGE:  return "message";
    case TYPE::TYPE_BYTES:    return "bytes";
    case TYPE::TYPE_UINT32:   return "uint32";
    case TYPE::TYPE_ENUM:     return "enum";
    case TYPE::TYPE_SFIXED32: return "sfixed32";
    case TYPE::TYPE_SFIXED64: return "sfixed64";
    case TYPE::TYPE_SINT32:   return "sint32";
    case TYPE::TYPE_SINT64:   return "sint64";
    }

    return "unknown";
}


nlohmann::json describePcbMessage( const std::string& aItemType,
                                   const std::string& aMessagePath, std::string& aError )
{
    std::unique_ptr<google::protobuf::Message> message = newPcbItem( aItemType );

    if( !message )
    {
        aError = "unsupported PCB item type";
        return {};
    }

    const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
    std::string                         normalizedPath;
    size_t                              begin = 0;
    size_t                              depth = 0;

    while( begin < aMessagePath.size() )
    {
        size_t end = aMessagePath.find( '.', begin );

        if( end == std::string::npos )
            end = aMessagePath.size();

        std::string segment = aMessagePath.substr( begin, end - begin );

        if( segment.empty() || ++depth > 8 )
        {
            aError = "pcb.messagePath must contain at most 8 non-empty field names";
            return {};
        }

        const google::protobuf::FieldDescriptor* selected = nullptr;

        for( int i = 0; i < descriptor->field_count(); ++i )
        {
            const google::protobuf::FieldDescriptor* field = descriptor->field( i );

            if( field->name() == segment || field->json_name() == segment )
            {
                selected = field;
                break;
            }
        }

        if( !selected || selected->cpp_type()
                                 != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            aError = "pcb.messagePath does not identify a message-typed field";
            return {};
        }

        if( !normalizedPath.empty() )
            normalizedPath += '.';

        normalizedPath += selected->json_name();
        descriptor = selected->message_type();
        begin = end + 1;
    }

    nlohmann::json fields = nlohmann::json::array();

    for( int i = 0; i < descriptor->field_count(); ++i )
    {
        const google::protobuf::FieldDescriptor* field = descriptor->field( i );
        nlohmann::json fieldDescription = {
            { "name", field->json_name() },
            { "protoName", field->name() },
            { "number", field->number() },
            { "type", protobufFieldType( *field ) },
            { "cardinality", field->is_repeated() ? "repeated" : "singular" }
        };

        if( field->containing_oneof() )
            fieldDescription["oneof"] = field->containing_oneof()->name();

        if( field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            fieldDescription["messageType"] = field->message_type()->full_name();
            fieldDescription["expandWith"] = normalizedPath.empty()
                                                     ? field->json_name()
                                                     : normalizedPath + '.' + field->json_name();
        }
        else if( field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM )
        {
            nlohmann::json values = nlohmann::json::array();

            for( int valueIndex = 0; valueIndex < field->enum_type()->value_count(); ++valueIndex )
            {
                const google::protobuf::EnumValueDescriptor* value =
                        field->enum_type()->value( valueIndex );
                values.push_back( { { "name", value->name() }, { "number", value->number() } } );
            }

            fieldDescription["enumType"] = field->enum_type()->full_name();
            fieldDescription["values"] = std::move( values );
        }

        if( field->type() == google::protobuf::FieldDescriptor::TYPE_INT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_UINT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_SINT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_FIXED64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_SFIXED64 )
        {
            fieldDescription["jsonEncoding"] = "decimal string";
        }

        fields.emplace_back( std::move( fieldDescription ) );
    }

    return { { "itemType", aItemType },
             { "messagePath", normalizedPath },
             { "messageType", descriptor->full_name() },
             { "fields", std::move( fields ) },
             { "units", "Coordinates and distances are nanometers" } };
}


bool parsePcbItem( const std::string& aItemType, const nlohmann::json& aJson,
                   std::unique_ptr<google::protobuf::Message>& aMessage, std::string& aError )
{
    if( !aJson.is_object() )
    {
        aError = "each PCB item must be a JSON object";
        return false;
    }

    aMessage = newPcbItem( aItemType );

    if( !aMessage )
    {
        aError = "unsupported PCB item type";
        return false;
    }

    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = false;
    google::protobuf::util::Status status =
            google::protobuf::util::JsonStringToMessage( aJson.dump(), aMessage.get(), options );

    if( !status.ok() )
    {
        aError = status.ToString();
        return false;
    }

    return true;
}


std::string pcbItemId( const google::protobuf::Any& aItem, const std::string& aItemType )
{
    std::unique_ptr<google::protobuf::Message> message = newPcbItem( aItemType );

    if( !message || !aItem.UnpackTo( message.get() ) )
        return {};

    const google::protobuf::FieldDescriptor* idField =
            message->GetDescriptor()->FindFieldByName( "id" );

    if( !idField || idField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        return {};

    const google::protobuf::Message& idMessage =
            message->GetReflection()->GetMessage( *message, idField );
    const google::protobuf::FieldDescriptor* valueField =
            idMessage.GetDescriptor()->FindFieldByName( "value" );

    if( !valueField || valueField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING )
        return {};

    return idMessage.GetReflection()->GetString( idMessage, valueField );
}


class KICHAD_IPC_COMMIT_GUARD
{
public:
    KICHAD_IPC_COMMIT_GUARD( const KICHAD_IPC_CLIENT& aClient,
                             const KICHAD_IPC_TARGET& aTarget ) :
            m_client( aClient ),
            m_target( aTarget ),
            m_active( false )
    {}

    ~KICHAD_IPC_COMMIT_GUARD()
    {
        if( m_active )
        {
            std::string ignored;
            m_client.EndCommit( m_target, m_id, false, "", ignored );
        }
    }

    bool Begin( std::string& aError )
    {
        m_active = m_client.BeginCommit( m_target, m_id, aError );

        if( !m_active )
        {
            // EndCommit deliberately accepts an empty ID for CMA_DROP.  Recover a transaction
            // whose previous response was lost before refusing the new mutation.
            std::string recoveryError;

            if( m_client.EndCommit( m_target, "", false, "", recoveryError ) )
                m_active = m_client.BeginCommit( m_target, m_id, aError );
        }

        return m_active;
    }

    bool Commit( const std::string& aMessage, std::string& aError )
    {
        if( !m_active || !m_client.EndCommit( m_target, m_id, true, aMessage, aError ) )
            return false;

        m_active = false;
        return true;
    }

    bool Drop( std::string& aError )
    {
        if( !m_active )
            return true;

        if( !m_client.EndCommit( m_target, m_id, false, "", aError ) )
            return false;

        m_active = false;
        return true;
    }

private:
    const KICHAD_IPC_CLIENT& m_client;
    const KICHAD_IPC_TARGET& m_target;
    std::string              m_id;
    bool                     m_active;
};


std::string pcbAnyType( const google::protobuf::Any& aItem )
{
    using namespace kiapi::board::types;

    if( aItem.Is<BoardGraphicShape>() )
        return "shape";
    if( aItem.Is<Track>() )
        return "trace";
    if( aItem.Is<Arc>() )
        return "arc";
    if( aItem.Is<Via>() )
        return "via";
    if( aItem.Is<BoardText>() )
        return "text";
    if( aItem.Is<Dimension>() )
        return "dimension";
    if( aItem.Is<FootprintInstance>() )
        return "footprint";
    if( aItem.Is<Zone>() )
    {
        Zone zone;

        if( aItem.UnpackTo( &zone ) && zone.type() == ZT_RULE_AREA )
            return "rule_area";

        return "zone";
    }

    return "other:" + aItem.type_url();
}


std::string pcbAnyId( const google::protobuf::Any& aItem )
{
    std::string itemType = pcbAnyType( aItem );

    if( itemType.starts_with( "other:" ) )
    {
        size_t separator = aItem.type_url().rfind( '/' );
        std::string typeName = separator == std::string::npos
                                       ? aItem.type_url()
                                       : aItem.type_url().substr( separator + 1 );
        const google::protobuf::Descriptor* descriptor =
                google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName( typeName );

        if( !descriptor )
            return {};

        const google::protobuf::Message* prototype =
                google::protobuf::MessageFactory::generated_factory()->GetPrototype( descriptor );

        if( !prototype )
            return {};

        std::unique_ptr<google::protobuf::Message> message( prototype->New() );

        if( !aItem.UnpackTo( message.get() ) )
            return {};

        const google::protobuf::FieldDescriptor* idField = descriptor->FindFieldByName( "id" );

        if( !idField || idField->cpp_type()
                               != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            return {};
        }

        const google::protobuf::Message& idMessage =
                message->GetReflection()->GetMessage( *message, idField );
        const google::protobuf::FieldDescriptor* valueField =
                idMessage.GetDescriptor()->FindFieldByName( "value" );

        if( !valueField || valueField->cpp_type()
                                  != google::protobuf::FieldDescriptor::CPPTYPE_STRING )
        {
            return {};
        }

        return idMessage.GetReflection()->GetString( idMessage, valueField );
    }

    return pcbItemId( aItem, itemType );
}


bool queryPcbInventory( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const std::set<std::string>& aIds, nlohmann::json& aInventory,
                        std::string& aError )
{
    aInventory = nlohmann::json::array();

    if( aIds.empty() )
        return true;

    kiapi::common::commands::GetItemsById request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

    for( const std::string& id : aIds )
        request.add_items()->set_value( id );

    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::GetItemsResponse items;

    if( !response.message().UnpackTo( &items )
        || items.status() != kiapi::common::types::IRS_OK )
    {
        aError = "KiCad returned an invalid managed-item inventory";
        return false;
    }

    std::set<std::string> returnedIds;

    for( const google::protobuf::Any& item : items.items() )
    {
        std::string itemId = pcbAnyId( item );

        if( itemId.empty() || !aIds.contains( itemId ) || !returnedIds.emplace( itemId ).second )
        {
            aError = "KiCad returned an unexpected managed-item identity";
            return false;
        }

        aInventory.push_back( { { "itemId", itemId }, { "itemType", pcbAnyType( item ) } } );
    }

    return true;
}


bool queryPcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                      kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    kiapi::board::commands::GetBoardStackup request;
    request.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardStackupResponse stackupResponse;

    if( !response.message().UnpackTo( &stackupResponse )
        || !stackupResponse.has_stackup() )
    {
        aError = "KiCad returned an invalid board stackup";
        return false;
    }

    aStackup.CopyFrom( stackupResponse.stackup() );
    return true;
}


bool updatePcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                       const kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    kiapi::board::commands::UpdateBoardStackup request;
    request.mutable_board()->CopyFrom( aTarget.document );
    request.mutable_stackup()->CopyFrom( aStackup );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardStackupResponse stackupResponse;

    if( !response.message().UnpackTo( &stackupResponse )
        || !stackupResponse.has_stackup() )
    {
        aError = "KiCad returned an invalid updated board stackup";
        return false;
    }

    return true;
}


bool queryPcbRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                    kiapi::board::BoardDesignRules& aRules, std::string& aError )
{
    kiapi::board::commands::GetBoardDesignRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardDesignRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse ) || !rulesResponse.has_rules() )
    {
        aError = "KiCad returned invalid board design rules";
        return false;
    }

    aRules.CopyFrom( rulesResponse.rules() );
    return true;
}


bool updatePcbRules( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                     const kiapi::board::BoardDesignRules& aRules, std::string& aError )
{
    kiapi::board::commands::UpdateBoardDesignRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    request.mutable_rules()->CopyFrom( aRules );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardDesignRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse ) || !rulesResponse.has_rules() )
    {
        aError = "KiCad returned invalid updated board design rules";
        return false;
    }

    return true;
}


bool queryPcbCustomRules( const KICHAD_IPC_CLIENT& aClient,
                          const KICHAD_IPC_TARGET& aTarget, bool& aPresent,
                          std::string& aSource, std::string& aError )
{
    kiapi::board::commands::GetBoardCustomRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardCustomRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse ) )
    {
        aError = "KiCad returned invalid board custom rules";
        return false;
    }

    aPresent = rulesResponse.present();
    aSource = rulesResponse.source();
    return true;
}


bool updatePcbCustomRules( const KICHAD_IPC_CLIENT& aClient,
                           const KICHAD_IPC_TARGET& aTarget, bool aPresent,
                           const std::string& aSource, std::string& aError )
{
    kiapi::board::commands::UpdateBoardCustomRules request;
    request.mutable_board()->CopyFrom( aTarget.document );
    request.set_present( aPresent );
    request.set_source( aSource );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardCustomRulesResponse rulesResponse;

    if( !response.message().UnpackTo( &rulesResponse )
        || rulesResponse.present() != aPresent || rulesResponse.source() != aSource )
    {
        aError = "KiCad returned invalid updated board custom rules";
        return false;
    }

    return true;
}


bool queryNetClassSettings(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::NetClassSettings& aSettings, std::string& aError )
{
    kiapi::common::commands::GetNetClassSettings request;
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::NetClassSettingsResponse settingsResponse;

    if( !response.message().UnpackTo( &settingsResponse )
        || !settingsResponse.has_settings() )
    {
        aError = "KiCad returned invalid netclass settings";
        return false;
    }

    aSettings.CopyFrom( settingsResponse.settings() );
    return true;
}


bool updateNetClassSettings(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::NetClassSettings& aSettings, std::string& aError )
{
    kiapi::common::commands::UpdateNetClassSettings request;
    request.mutable_settings()->CopyFrom( aSettings );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::NetClassSettingsResponse settingsResponse;

    if( !response.message().UnpackTo( &settingsResponse )
        || !settingsResponse.has_settings() )
    {
        aError = "KiCad returned invalid updated netclass settings";
        return false;
    }

    return true;
}


bool queryPcbFootprintInventory( const KICHAD_IPC_CLIENT& aClient,
                                 const KICHAD_IPC_TARGET& aTarget,
                                 const std::set<std::string>& aReferences,
                                 nlohmann::json& aInventory, std::string& aError )
{
    if( aReferences.empty() )
        return true;

    kiapi::common::commands::GetItems request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
    request.add_types( kiapi::common::types::KOT_PCB_FOOTPRINT );

    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::GetItemsResponse items;

    if( !response.message().UnpackTo( &items )
        || items.status() != kiapi::common::types::IRS_OK
        || items.items_size() > static_cast<int>( MAX_PCB_FOOTPRINTS ) )
    {
        aError = "KiCad returned an invalid or excessive footprint inventory";
        return false;
    }

    std::set<std::string> returnedIds;

    for( const google::protobuf::Any& packed : items.items() )
    {
        kiapi::board::types::FootprintInstance footprint;

        if( !packed.UnpackTo( &footprint ) )
        {
            aError = "KiCad returned a non-footprint in the footprint inventory";
            return false;
        }

        const std::string reference = footprint.reference_field().text().text().text();

        if( !aReferences.contains( reference ) )
            continue;

        const std::string itemId = footprint.id().value();

        if( !KIID::SniffTest( wxString::FromUTF8( itemId ) )
            || !returnedIds.emplace( itemId ).second )
        {
            aError = "KiCad returned an invalid or duplicate footprint identity";
            return false;
        }

        aInventory.push_back(
                { { "itemId", itemId },
                  { "itemType", "footprint" },
                  { "reference", reference },
                  { "schematicLinked", !footprint.attributes().not_in_schematic()
                                                && footprint.symbol_path().path_size() > 0 } } );
    }

    return true;
}


bool refillPcbZones( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                     const std::set<std::string>& aExpectedZoneIds, std::string& aError )
{
    kiapi::board::commands::RefillZones refill;
    refill.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, refill, response, aError ) )
        return false;

    // RefillZones is accepted synchronously but KiCad performs the fill on the editor thread and
    // reports AS_BUSY in the interim. Poll the authoritative zone objects until every desired KDS
    // zone is present and filled. This also closes the race before the queued fill action starts.
    std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
    const auto deadline = std::chrono::steady_clock::now() + MAX_ZONE_REFILL_WAIT;
    std::string lastError;

    do
    {
        kiapi::common::commands::GetItems query;
        query.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
        query.add_types( kiapi::common::types::KOT_PCB_ZONE );
        kiapi::common::ApiResponse queryResponse;

        if( aClient.Call( aTarget, query, queryResponse, lastError ) )
        {
            kiapi::common::commands::GetItemsResponse items;

            if( !queryResponse.message().UnpackTo( &items )
                || items.status() != kiapi::common::types::IRS_OK
                || items.items_size() > static_cast<int>( MAX_PCB_ZONES ) )
            {
                aError = "KiCad returned an invalid or excessive zone inventory after refill";
                return false;
            }

            std::set<std::string> filled;

            for( const google::protobuf::Any& packed : items.items() )
            {
                kiapi::board::types::Zone zone;

                if( !packed.UnpackTo( &zone ) )
                {
                    aError = "KiCad returned a non-zone in the zone refill inventory";
                    return false;
                }

                if( aExpectedZoneIds.contains( zone.id().value() ) && zone.filled() )
                    filled.emplace( zone.id().value() );
            }

            if( filled.size() == aExpectedZoneIds.size() )
                return true;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
    } while( std::chrono::steady_clock::now() < deadline );

    aError = "KiCad did not finish filling every managed KDS zone within 30 seconds";

    if( !lastError.empty() )
        aError += ": " + lastError;

    return false;
}


bool validateCreateUpdateResponse( const google::protobuf::RepeatedPtrField<
                                           kiapi::common::commands::ItemCreationResult>& aResults,
                                   const std::vector<const nlohmann::json*>& aActions,
                                   std::string& aError )
{
    if( aResults.size() != static_cast<int>( aActions.size() ) )
    {
        aError = "KiCad returned an incomplete create-items response";
        return false;
    }

    for( int i = 0; i < aResults.size(); ++i )
    {
        if( aResults[i].status().code() != kiapi::common::commands::ISC_OK
            || pcbAnyId( aResults[i].item() ) != aActions[i]->at( "itemId" ).get<std::string>() )
        {
            aError = aResults[i].status().error_message();

            if( aError.empty() )
                aError = "KiCad rejected or changed a deterministic PCB item UUID";

            return false;
        }
    }

    return true;
}


bool validateCreateUpdateResponse( const google::protobuf::RepeatedPtrField<
                                           kiapi::common::commands::ItemUpdateResult>& aResults,
                                   const std::vector<const nlohmann::json*>& aActions,
                                   std::string& aError )
{
    if( aResults.size() != static_cast<int>( aActions.size() ) )
    {
        aError = "KiCad returned an incomplete update-items response";
        return false;
    }

    for( int i = 0; i < aResults.size(); ++i )
    {
        if( aResults[i].status().code() != kiapi::common::commands::ISC_OK
            || pcbAnyId( aResults[i].item() ) != aActions[i]->at( "itemId" ).get<std::string>() )
        {
            aError = aResults[i].status().error_message();

            if( aError.empty() )
                aError = "KiCad rejected or changed a deterministic PCB item UUID";

            return false;
        }
    }

    return true;
}


bool createFootprintFromSource( const KICHAD_IPC_CLIENT& aClient,
                                const KICHAD_IPC_TARGET& aTarget,
                                const nlohmann::json& aAction,
                                const nlohmann::json& aFootprintSources,
                                std::string& aError )
{
    const nlohmann::json& instance = aAction.at( "instance" );
    const std::string libraryId = instance.at( "libraryId" ).get<std::string>();

    if( !aFootprintSources.is_object() || !aFootprintSources.contains( libraryId )
        || !aFootprintSources[libraryId].is_string() )
    {
        aError = "placed footprint " + libraryId
                 + " is not available from a confined project-local library";
        return false;
    }

    const size_t separator = libraryId.find( ':' );
    kiapi::board::types::FootprintInstance requested;
    requested.mutable_id()->set_value( aAction.at( "itemId" ).get<std::string>() );
    requested.mutable_definition()->mutable_id()->set_library_nickname(
            libraryId.substr( 0, separator ) );
    requested.mutable_definition()->mutable_id()->set_entry_name(
            libraryId.substr( separator + 1 ) );
    requested.mutable_reference_field()->mutable_text()->mutable_text()->set_text(
            aAction.at( "component" ).get<std::string>() );
    requested.mutable_value_field()->mutable_text()->mutable_text()->set_text(
            instance.at( "value" ).get<std::string>() );
    requested.mutable_attributes()->set_not_in_schematic( false );
    requested.mutable_attributes()->set_do_not_populate( instance.at( "dnp" ).get<bool>() );

    for( const nlohmann::json& id : instance.at( "symbolPath" ) )
    {
        requested.mutable_symbol_path()->add_path()->set_value( id.get<std::string>() );
    }

    requested.set_symbol_sheet_name( instance.at( "symbolSheetName" ).get<std::string>() );
    requested.set_symbol_sheet_filename(
            instance.at( "symbolSheetFilename" ).get<std::string>() );
    requested.mutable_position()->set_x_nm(
            aAction.at( "position" ).at( "xNm" ).get<int64_t>() );
    requested.mutable_position()->set_y_nm(
            aAction.at( "position" ).at( "yNm" ).get<int64_t>() );
    requested.mutable_orientation()->set_value_degrees(
            aAction.at( "rotationDegrees" ).get<double>() );
    requested.set_layer( aAction.at( "side" ) == "front"
                                 ? kiapi::board::types::BL_F_Cu
                                 : kiapi::board::types::BL_B_Cu );
    requested.set_locked( aAction.at( "locked" ).get<bool>()
                                  ? kiapi::common::types::LS_LOCKED
                                  : kiapi::common::types::LS_UNLOCKED );

    for( auto net = instance.at( "padNets" ).begin();
         net != instance.at( "padNets" ).end(); ++net )
    {
        kiapi::board::types::Pad pad;
        pad.set_number( net.key() );
        pad.mutable_net()->set_name( net.value().get<std::string>() );
        requested.mutable_definition()->add_items()->PackFrom( pad );
    }

    kiapi::common::commands::ParseAndCreateItemsFromString parse;
    parse.mutable_document()->CopyFrom( aTarget.document );
    parse.set_contents( aFootprintSources[libraryId].get<std::string>() );
    parse.mutable_item()->PackFrom( requested );
    kiapi::common::ApiResponse parseResponse;

    if( !aClient.Call( aTarget, parse, parseResponse, aError ) )
        return false;

    kiapi::common::commands::CreateItemsResponse created;

    if( !parseResponse.message().UnpackTo( &created )
        || created.status() != kiapi::common::types::IRS_OK
        || created.created_items_size() != 1
        || created.created_items( 0 ).status().code() != kiapi::common::commands::ISC_OK )
    {
        aError = created.created_items_size() == 1
                         ? created.created_items( 0 ).status().error_message()
                         : "KiCad could not create exactly one footprint from " + libraryId;
        return false;
    }

    kiapi::board::types::FootprintInstance footprint;

    if( !created.created_items( 0 ).item().UnpackTo( &footprint )
        || footprint.id().value() != requested.id().value()
        || footprint.reference_field().text().text().text()
                   != aAction.at( "component" ).get<std::string>()
        || footprint.definition().id().library_nickname()
                   != requested.definition().id().library_nickname()
        || footprint.definition().id().entry_name()
                   != requested.definition().id().entry_name()
        || footprint.symbol_path().path_size() != requested.symbol_path().path_size()
        || footprint.position().x_nm() != requested.position().x_nm()
        || footprint.position().y_nm() != requested.position().y_nm()
        || footprint.layer() != requested.layer() )
    {
        aError = "KiCad returned a mismatched parsed footprint instance";
        return false;
    }

    return true;
}


bool executePcbActions( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const nlohmann::json& aActions,
                        const nlohmann::json& aFootprintSources, std::string& aError )
{
    std::vector<const nlohmann::json*> creates;
    std::vector<const nlohmann::json*> footprintCreates;
    std::map<std::pair<std::string, std::string>,
             std::vector<const nlohmann::json*>> updates;
    std::vector<const nlohmann::json*> deletes;

    for( const nlohmann::json& action : aActions )
    {
        const std::string kind = action.at( "action" ).get<std::string>();

        if( kind == "create" )
            creates.emplace_back( &action );
        else if( kind == "create_footprint" )
            footprintCreates.emplace_back( &action );
        else if( kind == "update" )
            updates[{ action.at( "itemType" ).get<std::string>(),
                      action.at( "fieldMask" ).dump() }].emplace_back( &action );
        else
            deletes.emplace_back( &action );
    }

    for( const nlohmann::json* action : footprintCreates )
    {
        if( !createFootprintFromSource( aClient, aTarget, *action,
                                        aFootprintSources, aError ) )
        {
            return false;
        }
    }

    for( size_t begin = 0; begin < creates.size(); begin += 200 )
    {
        const size_t end = std::min( begin + 200, creates.size() );
        kiapi::common::commands::CreateItems request;
        request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
        std::vector<const nlohmann::json*> batch( creates.begin() + begin, creates.begin() + end );

        for( const nlohmann::json* action : batch )
        {
            std::unique_ptr<google::protobuf::Message> item;

            if( !parsePcbItem( action->at( "itemType" ).get<std::string>(),
                               action->at( "item" ), item, aError ) )
            {
                return false;
            }

            request.add_items()->PackFrom( *item );
        }

        kiapi::common::ApiResponse response;

        if( !aClient.Call( aTarget, request, response, aError ) )
            return false;

        kiapi::common::commands::CreateItemsResponse created;

        if( !response.message().UnpackTo( &created )
            || created.status() != kiapi::common::types::IRS_OK
            || !validateCreateUpdateResponse( created.created_items(), batch, aError ) )
        {
            if( aError.empty() )
                aError = "KiCad returned an invalid create-items response";

            return false;
        }
    }

    for( const auto& [key, actions] : updates )
    {
        const std::string& itemType = key.first;

        for( size_t begin = 0; begin < actions.size(); begin += 200 )
        {
            const size_t end = std::min( begin + 200, actions.size() );
            std::vector<const nlohmann::json*> batch( actions.begin() + begin,
                                                       actions.begin() + end );
            kiapi::common::commands::UpdateItems request;
            request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

            for( const nlohmann::json& field : batch.front()->at( "fieldMask" ) )
                request.mutable_header()->mutable_field_mask()->add_paths( field.get<std::string>() );

            for( const nlohmann::json* action : batch )
            {
                std::unique_ptr<google::protobuf::Message> item;

                if( !parsePcbItem( itemType, action->at( "item" ), item, aError ) )
                    return false;

                request.add_items()->PackFrom( *item );
            }

            kiapi::common::ApiResponse response;

            if( !aClient.Call( aTarget, request, response, aError ) )
                return false;

            kiapi::common::commands::UpdateItemsResponse updated;

            if( !response.message().UnpackTo( &updated )
                || updated.status() != kiapi::common::types::IRS_OK
                || !validateCreateUpdateResponse( updated.updated_items(), batch, aError ) )
            {
                if( aError.empty() )
                    aError = "KiCad returned an invalid update-items response";

                return false;
            }
        }
    }

    for( size_t begin = 0; begin < deletes.size(); begin += 500 )
    {
        const size_t end = std::min( begin + 500, deletes.size() );
        kiapi::common::commands::DeleteItems request;
        request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

        for( size_t i = begin; i < end; ++i )
            request.add_item_ids()->set_value( deletes[i]->at( "itemId" ).get<std::string>() );

        kiapi::common::ApiResponse response;

        if( !aClient.Call( aTarget, request, response, aError ) )
            return false;

        kiapi::common::commands::DeleteItemsResponse deleted;

        if( !response.message().UnpackTo( &deleted )
            || deleted.status() != kiapi::common::types::IRS_OK
            || deleted.deleted_items_size() != static_cast<int>( end - begin ) )
        {
            aError = "KiCad returned an invalid delete-items response";
            return false;
        }

        for( int i = 0; i < deleted.deleted_items_size(); ++i )
        {
            if( deleted.deleted_items( i ).status() != kiapi::common::commands::IDS_OK
                || deleted.deleted_items( i ).id().value()
                           != deletes[begin + static_cast<size_t>( i )]
                                      ->at( "itemId" ).get<std::string>() )
            {
                aError = "KiCad rejected or changed a managed PCB deletion";
                return false;
            }
        }
    }

    return true;
}

} // namespace


CODEX_TOOL_REGISTRY::CODEX_TOOL_REGISTRY( std::function<wxString()> aProjectPathProvider,
                                          std::function<bool()> aMutationGuard,
                                          std::function<wxString()> aIpcSocketDirectoryProvider,
                                          std::function<bool( const wxFileName&, std::string& )>
                                                  aSchematicValidator,
                                          NATIVE_CHECK_RUNNER aNativeCheckRunner,
                                          NATIVE_FABRICATION_RUNNER aNativeFabricationRunner ) :
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_mutationGuard( std::move( aMutationGuard ) ),
        m_ipcSocketDirectoryProvider( std::move( aIpcSocketDirectoryProvider ) ),
        m_schematicValidator( std::move( aSchematicValidator ) ),
        m_nativeCheckRunner( std::move( aNativeCheckRunner ) ),
        m_nativeFabricationRunner( std::move( aNativeFabricationRunner ) )
{}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Specs() const
{
    JSON specs = JSON::array();

    JSON projectSchema = { { "type", "object" },
                           { "additionalProperties", false },
                           { "required", JSON::array( { "operation" } ) } };
    projectSchema["properties"]["operation"] =
            { { "type", "string" }, { "enum", JSON::array( { "context" } ) } };

    specs.push_back( { { "type", "function" },
                       { "name", "project" },
                       { "description",
                         "Read the active KiChad project context. Use operation 'context' to "
                         "discover the stable KiCad version, design files, and whether a turn "
                         "snapshot allows mutation." },
                       { "inputSchema", std::move( projectSchema ) } } );

    JSON inspectSchema = { { "type", "object" },
                           { "additionalProperties", false },
                           { "required", JSON::array( { "operation", "path" } ) } };
    inspectSchema["properties"]["operation"] =
            { { "type", "string" }, { "enum", JSON::array( { "summary", "find" } ) } };
    inspectSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative KiCad design file path." } };
    inspectSchema["properties"]["head"] =
            { { "type", "string" }, { "maxLength", 128 },
              { "description", "List head required by operation 'find'." } };
    inspectSchema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 },
              { "description", "Maximum matches returned; defaults to 20." } };

    specs.push_back( { { "type", "function" },
                       { "name", "inspect" },
                       { "description",
                         "Inspect a KiCad 10 schematic, board, symbol library, or footprint "
                         "without changing it. Use 'summary' for structural counts or 'find' for "
                         "bounded raw expressions with a particular list head." },
                       { "inputSchema", std::move( inspectSchema ) } } );

    JSON designSchema = { { "type", "object" },
                          { "additionalProperties", false },
                          { "required", JSON::array( { "operation" } ) } };
    designSchema["properties"]["operation"] =
            { { "type", "string" },
              { "enum",
                JSON::array( { "describe", "read", "compile", "preview", "save", "apply" } ) } };
    designSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative reusable .kicad_kds sidecar path." } };
    designSchema["properties"]["source"] =
            { { "type", "string" }, { "maxLength", MAX_DESIGN_SCRIPT_BYTES },
              { "description", "Inline KiChad Design Script s-expression source." } };
    designSchema["properties"]["boardPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative .kicad_pcb target required by apply." } };
    designSchema["properties"]["expectedSha256"] =
            { { "type", "string" }, { "minLength", 64 }, { "maxLength", 64 },
              { "description",
                "Required for stale-write protection and to apply the exact compiled revision." } };
    specs.push_back( { { "type", "function" },
                       { "name", "design" },
                       { "description",
                         "Describe, read, compile, preview, atomically save, or transactionally "
                         "apply a reusable KiChad Design "
                         "Script sidecar. KDS programs declare the complete project—schematic, "
                         "libraries, PCB intent, sourcing, verification, and fabrication outputs—"
                         "for deterministic execution by KiChad compiler backends." },
                       { "inputSchema", std::move( designSchema ) } } );

    JSON pcbSchema = { { "type", "object" },
                       { "additionalProperties", false },
                       { "required", JSON::array( { "operation", "path" } ) } };
    pcbSchema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", JSON::array( { "status", "describe", "get", "mutate" } ) } };
    pcbSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative .kicad_pcb path." } };
    pcbSchema["properties"]["itemType"] =
            { { "type", "string" },
              { "enum", JSON::array(
                                { "footprint", "trace", "via", "arc", "zone", "rule_area",
                                  "shape", "text", "dimension" } ) },
              { "description", "KiCad 10 protobuf board item type." } };
    pcbSchema["properties"]["messagePath"] =
            { { "type", "string" }, { "maxLength", 512 },
              { "description",
                "Dot-separated message field path to expand with operation 'describe'." } };
    pcbSchema["properties"]["action"] =
            { { "type", "string" },
              { "enum", JSON::array( { "create", "update", "delete" } ) } };
    pcbSchema["properties"]["items"] =
            { { "type", "array" }, { "minItems", 1 }, { "maxItems", 200 },
              { "items", { { "type", "object" } } },
              { "description", "Items encoded with official protobuf JSON field names." } };
    pcbSchema["properties"]["ids"] =
            { { "type", "array" }, { "minItems", 1 }, { "maxItems", 500 },
              { "items", { { "type", "string" }, { "maxLength", 36 } } } };
    pcbSchema["properties"]["fieldMask"] =
            { { "type", "array" }, { "maxItems", 32 },
              { "items", { { "type", "string" }, { "maxLength", 128 } } },
              { "description",
                "Protobuf protoName field paths; required for update. UUID fields are immutable." } };
    pcbSchema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } };
    pcbSchema["properties"]["commitMessage"] =
            { { "type", "string" }, { "maxLength", 256 } };

    specs.push_back( { { "type", "function" },
                       { "name", "pcb" },
                       { "description",
                         "Use the supported KiCad 10 protobuf IPC API for the live PCB Editor. "
                         "Use 'status' to verify the editor, 'describe' to discover exact typed "
                         "fields, 'get' to read live items, or 'mutate' to create, field-mask "
                         "update, or delete items atomically. "
                         "Coordinates and distances in protobuf JSON are nanometers." },
                       { "inputSchema", std::move( pcbSchema ) } } );

    JSON verifySchema = { { "type", "object" },
                          { "additionalProperties", false },
                          { "required", JSON::array( { "operation", "path" } ) } };
    verifySchema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", JSON::array( { "erc", "drc", "sourcing" } ) } };
    verifySchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description",
                "Project-relative .kicad_sch for ERC, .kicad_pcb for DRC, or "
                ".kicad_kds for sourcing." } };
    verifySchema["properties"]["offset"] =
            { { "type", "integer" }, { "minimum", 0 }, { "maximum", 1000000 },
              { "description", "Zero-based violation offset; defaults to 0." } };
    verifySchema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 },
              { "description", "Maximum violations returned; defaults to 100." } };
    verifySchema["properties"]["maxAgeDays"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 90 },
              { "description",
                "Maximum sourcing-evidence age in days; defaults to 7." } };

    specs.push_back( { { "type", "function" },
                       { "name", "verify" },
                       { "description",
                         "Run read-only native design gates. ERC and DRC use the matching KiCad "
                         "10 backend; DRC also checks schematic parity. Sourcing checks the "
                         "single KDS evidence cache for completeness, orderability, and "
                         "freshness. Results have complete counts and bounded pageable issues." },
                       { "inputSchema", std::move( verifySchema ) } } );

    JSON fabricateSchema = { { "type", "object" },
                             { "additionalProperties", false },
                             { "required",
                               JSON::array( { "operation", "path", "boardPath",
                                              "schematicPath", "expectedSha256" } ) } };
    fabricateSchema["properties"]["operation"] =
            { { "type", "string" }, { "enum", JSON::array( { "plan", "export" } ) } };
    fabricateSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative canonical .kicad_kds sidecar." } };
    fabricateSchema["properties"]["boardPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative on-disk .kicad_pcb input." } };
    fabricateSchema["properties"]["schematicPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative root .kicad_sch input." } };
    fabricateSchema["properties"]["expectedSha256"] =
            { { "type", "string" }, { "minLength", 64 }, { "maxLength", 64 },
              { "description", "Exact compiled KDS revision to release." } };
    fabricateSchema["properties"]["allowWaivers"] =
            { { "type", "boolean" },
              { "description",
                "Allow explicitly ignored/excluded ERC or DRC checks in a confirmed export; "
                "defaults to false." } };

    specs.push_back( { { "type", "function" },
                       { "name", "fabricate" },
                       { "description",
                         "Plan or perform the final KiCad 10 fabrication export declared by the "
                         "exact KDS revision. Export reruns ERC, DRC, and sourcing gates, writes "
                         "a private staging package, validates every artifact, and atomically "
                         "installs it only after visible user confirmation." },
                       { "inputSchema", std::move( fabricateSchema ) } } );

    return specs;
}


bool CODEX_TOOL_REGISTRY::RequiresFinalConfirmation( const std::string& aTool,
                                                     const JSON& aArguments )
{
    return aTool == "fabricate" && aArguments.is_object()
           && aArguments.contains( "operation" ) && aArguments["operation"].is_string()
           && aArguments["operation"].get<std::string>() == "export";
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Handle( const std::string& aTool,
                                                        const JSON& aArguments ) const
{
    wxString socketDirectory =
            m_ipcSocketDirectoryProvider ? m_ipcSocketDirectoryProvider() : wxString();
    return HandleWithContext( aTool, aArguments, projectPath(),
                              m_mutationGuard && m_mutationGuard(), socketDirectory );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::HandleWithContext(
        const std::string& aTool, const JSON& aArguments, const wxString& aProjectPath,
        bool aMutationAvailable, const wxString& aIpcSocketDirectory,
        bool aFinalActionApproved ) const
{
    try
    {
        if( aTool == "project" )
            return handleProject( aArguments, aProjectPath, aMutationAvailable );

        if( aTool == "inspect" )
            return handleInspect( aArguments, aProjectPath );

        if( aTool == "design" )
            return handleDesign( aArguments, aProjectPath, aMutationAvailable,
                                 aIpcSocketDirectory );

        if( aTool == "pcb" )
            return handlePcb( aArguments, aProjectPath, aMutationAvailable, aIpcSocketDirectory );

        if( aTool == "verify" )
            return handleVerify( aArguments, aProjectPath );

        if( aTool == "fabricate" )
        {
            return handleFabricate( aArguments, aProjectPath, aMutationAvailable,
                                    aFinalActionApproved );
        }

        return failure( "unknown_tool", "The requested tool is not advertised by KiChad" );
    }
    catch( const std::exception& error )
    {
        return failure( "tool_failed", error.what() );
    }
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleDesign(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        const wxString& aIpcSocketDirectory ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() )
    {
        return failure( "invalid_arguments", "design.operation must be a string" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "describe" && operation != "read" && operation != "compile"
        && operation != "preview" && operation != "save" && operation != "apply" )
    {
        return failure( "invalid_arguments",
                        "design.operation must be 'describe', 'read', 'compile', 'preview', "
                        "'save', or 'apply'" );
    }

    if( operation == "describe" )
        return success( KICHAD::DESIGN_SCRIPT_COMPILER::Describe() );

    const bool hasSource = aArguments.contains( "source" ) && aArguments["source"].is_string();
    const bool hasPath = aArguments.contains( "path" ) && aArguments["path"].is_string();

    if( ( ( operation == "compile" || operation == "preview" ) && hasSource == hasPath )
        || ( operation == "save" && ( !hasSource || !hasPath ) )
        || ( ( operation == "read" || operation == "apply" ) && ( hasSource || !hasPath ) ) )
    {
        std::string message;

        if( operation == "read" || operation == "apply" )
            message = "design." + operation + " requires path and does not accept inline source";
        else if( operation == "save" )
            message = "design.save requires source and path";
        else
            message = "design.compile and design.preview require exactly one of source or path";

        return failure( "invalid_arguments", message );
    }

    if( ( aArguments.contains( "source" ) && !aArguments["source"].is_string() )
        || ( aArguments.contains( "path" ) && !aArguments["path"].is_string() )
        || ( aArguments.contains( "boardPath" ) && !aArguments["boardPath"].is_string() ) )
    {
        return failure( "invalid_arguments",
                        "design source, path, or boardPath has the wrong type" );
    }

    std::string source;
    wxFileName  sidecar;
    std::string pathError;

    if( hasPath )
    {
        const std::string relativePath = aArguments["path"].get<std::string>();

        if( !resolveProjectSidecar( aProjectPath, relativePath, sidecar, pathError ) )
            return failure( "invalid_path", pathError );

        if( ( operation == "read" || operation == "compile" || operation == "preview"
              || operation == "apply" )
            && !sidecar.FileExists() )
            return failure( "read_failed", "KiChad Design Script sidecar does not exist" );
    }

    if( hasSource )
        source = aArguments["source"].get<std::string>();
    else if( !readDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    if( source.empty() || source.size() > MAX_DESIGN_SCRIPT_BYTES
        || source.find( '\0' ) != std::string::npos )
    {
        return failure( "invalid_source",
                        "KiChad Design Script source must be UTF-8 text containing 1 byte to 1 MiB" );
    }

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( operation == "read" )
    {
        const bool invalidUtf8 = std::any_of(
                compiled.diagnostics.begin(), compiled.diagnostics.end(),
                []( const JSON& aDiagnostic )
                {
                    return aDiagnostic.value( "code", "" ) == "invalid_encoding";
                } );

        if( invalidUtf8 )
            return failure( "invalid_source", "KiChad Design Script source must be valid UTF-8" );

        return success( { { "operation", "read" },
                          { "path", aArguments["path"] },
                          { "source", source },
                          { "bytes", source.size() },
                          { "sourceSha256", compiled.sourceSha256 },
                          { "valid", compiled.ok },
                          { "diagnostics", compiled.diagnostics } } );
    }

    if( operation == "compile" )
    {
        JSON payload = { { "operation", "compile" },
                         { "valid", compiled.ok },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "plan", compiled.plan },
                         { "diagnostics", compiled.diagnostics } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( operation == "preview" )
    {
        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        JSON symbolLibrarySources;
        JSON footprintSources;

        if( !inventoryProjectSymbolLibraries( aProjectPath, compiled.ir,
                                              symbolLibrarySources, pathError ) )
        {
            return failure( "symbol_inventory_failed", pathError );
        }

        if( !inventoryProjectFootprints( aProjectPath, compiled.ir,
                                         footprintSources, pathError ) )
        {
            return failure( "footprint_inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT resolvedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                        compiled.ir, symbolLibrarySources );
        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPlanned;

        if( resolvedSymbols.ok )
        {
            schematicPlanned = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, JSON::object(), resolvedSymbols.symbols );
        }
        else
        {
            schematicPlanned.counts = resolvedSymbols.counts;
            schematicPlanned.diagnostics = resolvedSymbols.diagnostics;
        }
        JSON items = JSON::array();

        for( const JSON& plannedOperation : planned.operations )
        {
            if( items.size() == 500 )
                break;

            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                items.push_back( { { "action", "manage" },
                                   { "logicalId", plannedOperation["logicalId"] },
                                   { "itemType", plannedOperation["itemType"] },
                                   { "targetId", plannedOperation["itemId"] } } );
            }
            else if( action == "place_by_reference" )
            {
                items.push_back( { { "action", "place" },
                                   { "component", plannedOperation["component"] } } );
            }
            else if( action == "update_stackup" )
            {
                items.push_back( { { "action", "configure_stackup" },
                                   { "physicalLayers",
                                     plannedOperation["stackup"]["layers"].size() } } );
            }
            else if( action == "update_rules" )
            {
                items.push_back( { { "action", "configure_global_board_rules" } } );
            }
            else if( action == "update_net_classes" )
            {
                items.push_back(
                        { { "action", "configure_net_classes" },
                          { "classes", plannedOperation["settings"]["netClasses"].size() },
                          { "assignments",
                            plannedOperation["settings"]["assignments"].size() } } );
            }
            else if( action == "update_project_library_table" )
            {
                items.push_back(
                        { { "action", "configure_project_library_table" },
                          { "kind", plannedOperation["kind"] },
                          { "path", plannedOperation["path"] },
                          { "libraries", plannedOperation["entries"] } } );
            }
            else if( action == "update_custom_rules" )
            {
                items.push_back(
                        { { "action", "configure_custom_board_rules" },
                          { "rules", planned.counts.value( "customRules", 0 ) } } );
            }
            else
            {
                items.push_back( { { "action", "unsupported" },
                                   { "statementKind", plannedOperation["statementKind"] },
                                   { "reason", plannedOperation["reason"] } } );
            }
        }

        JSON boardPlan = { { "fullyLowered", planned.fullyLowered },
                           { "counts", std::move( planned.counts ) },
                           { "diagnostics", std::move( planned.diagnostics ) },
                           { "items", std::move( items ) },
                           { "itemsTruncated", planned.operations.size() > 500 } };

        JSON schematicFiles = JSON::array();

        if( !schematicPlanned.operations.empty() )
        {
            for( const JSON& file : schematicPlanned.operations[0]["files"] )
            {
                schematicFiles.push_back( { { "path", file["path"] },
                                            { "sheetId", file["sheetId"] },
                                            { "page", file["page"] },
                                            { "root", file["root"] },
                                            { "managedItems",
                                              file["items"].size()
                                                      + file["libSymbols"].size()
                                                      + file["busAliases"].size() } } );
            }
        }

        JSON schematicPlan = { { "fullyLowered", schematicPlanned.fullyLowered },
                               { "counts", std::move( schematicPlanned.counts ) },
                               { "diagnostics", std::move( schematicPlanned.diagnostics ) },
                               { "files", std::move( schematicFiles ) } };

        JSON payload = { { "operation", "preview" },
                         { "valid", true },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "compilerPlan", std::move( compiled.plan ) },
                         { "boardPlan", std::move( boardPlan ) },
                         { "schematicPlan", std::move( schematicPlan ) } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( operation == "apply" )
    {
        if( !aMutationAvailable )
        {
            return failure( "snapshot_required",
                            "A complete pre-turn project snapshot is required to apply KDS" );
        }

        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message );
        }

        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string()
            || aArguments["expectedSha256"].get<std::string>() != compiled.sourceSha256 )
        {
            return failure( "stale_source",
                            "design.apply requires the exact compiled source SHA-256" );
        }

        if( !aArguments.contains( "boardPath" ) || !aArguments["boardPath"].is_string() )
            return failure( "invalid_arguments", "design.apply requires boardPath" );

        const std::string boardRelativePath = aArguments["boardPath"].get<std::string>();
        wxFileName       board;

        if( !resolveProjectFile( aProjectPath, boardRelativePath, board, pathError )
            || board.GetExt() != wxS( "kicad_pcb" ) )
        {
            if( pathError.empty() )
                pathError = "boardPath must identify a project .kicad_pcb file";

            return failure( "invalid_path", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        JSON symbolLibrarySources;
        JSON footprintSources;

        if( !inventoryProjectSymbolLibraries( aProjectPath, compiled.ir,
                                              symbolLibrarySources, pathError ) )
        {
            return failure( "symbol_inventory_failed", pathError );
        }

        if( !inventoryProjectFootprints( aProjectPath, compiled.ir,
                                         footprintSources, pathError ) )
        {
            return failure( "footprint_inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT resolvedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                        compiled.ir, symbolLibrarySources );

        if( !resolvedSymbols.ok )
        {
            return failure(
                    "backend_incomplete",
                    resolvedSymbols.diagnostics.empty()
                            ? "one or more schematic symbols could not be exactly resolved"
                            : resolvedSymbols.diagnostics.front().value(
                                      "message", "schematic symbol resolution failed" ) );
        }

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPreplanned =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                        compiled.ir, JSON::object(), resolvedSymbols.symbols );

        if( !planned.fullyLowered || !schematicPreplanned.fullyLowered )
        {
            std::string message = !planned.fullyLowered
                                          ? "one or more board statements do not have an apply backend"
                                          : "one or more schematic statements do not have an apply backend";

            for( const JSON& action : planned.operations )
            {
                if( action.value( "action", "" ) == "unsupported" )
                {
                    message += ": " + action.value( "reason", "unsupported statement" );
                    break;
                }
            }

            if( !schematicPreplanned.fullyLowered
                && !schematicPreplanned.diagnostics.empty() )
            {
                message += ": " + schematicPreplanned.diagnostics.front().value(
                                           "message", "unsupported schematic statement" );
            }

            return failure( "backend_incomplete", message );
        }

        std::unique_ptr<kiapi::board::BoardStackup> desiredStackup;
        std::unique_ptr<kiapi::board::BoardDesignRules> desiredRules;
        std::unique_ptr<kiapi::common::project::NetClassSettings> desiredNetClasses;
        JSON desiredLibraryTables = JSON::array();
        std::set<std::string> desiredLibraryTableKinds;
        bool        hasDesiredCustomRules = false;
        bool        desiredCustomRulesPresent = false;
        std::string desiredCustomRulesSource;

        for( const JSON& action : planned.operations )
        {
            const std::string plannedAction = action.value( "action", "" );

            if( plannedAction != "update_stackup" && plannedAction != "update_rules"
                && plannedAction != "update_net_classes"
                && plannedAction != "update_custom_rules"
                && plannedAction != "update_project_library_table" )
                continue;

            if( plannedAction == "update_project_library_table" )
            {
                const std::string kind = action.value( "kind", "" );
                const std::string expectedPath = kind == "symbol" ? "sym-lib-table"
                                                   : kind == "footprint" ? "fp-lib-table"
                                                                          : "";

                if( expectedPath.empty() || !desiredLibraryTableKinds.emplace( kind ).second
                    || action.value( "path", "" ) != expectedPath
                    || !action.contains( "present" ) || !action["present"].is_boolean()
                    || !action["present"].get<bool>() || !action.contains( "source" )
                    || !action["source"].is_string() )
                {
                    return failure( "invalid_plan",
                                    "KDS produced an invalid project library table operation" );
                }

                size_t      rows = 0;
                std::string validationError;
                const std::string tableSource = action["source"].get<std::string>();

                if( !validateProjectLibraryTable( kind, tableSource, rows, validationError )
                    || !action.contains( "entries" ) || !action["entries"].is_number_unsigned()
                    || action["entries"].get<size_t>() != rows )
                {
                    return failure( "invalid_plan",
                                    validationError.empty()
                                            ? "KDS project library table row count is inconsistent"
                                            : validationError );
                }

                desiredLibraryTables.push_back( { { "kind", kind },
                                                  { "path", expectedPath },
                                                  { "source", tableSource },
                                                  { "rows", rows } } );
                continue;
            }

            if( plannedAction == "update_custom_rules" )
            {
                if( hasDesiredCustomRules || !action.contains( "customRules" )
                    || !action["customRules"].is_object()
                    || !action["customRules"].contains( "present" )
                    || !action["customRules"]["present"].is_boolean()
                    || !action["customRules"].contains( "source" )
                    || !action["customRules"]["source"].is_string()
                    || action["customRules"]["source"].get_ref<const std::string&>().size()
                               > MAX_DESIGN_SCRIPT_BYTES )
                {
                    return failure( "invalid_plan",
                                    "KDS produced invalid native custom rules" );
                }

                hasDesiredCustomRules = true;
                desiredCustomRulesPresent = action["customRules"]["present"].get<bool>();
                desiredCustomRulesSource =
                        action["customRules"]["source"].get<std::string>();

                if( desiredCustomRulesPresent != !desiredCustomRulesSource.empty() )
                {
                    return failure( "invalid_plan",
                                    "KDS produced inconsistent native custom rules" );
                }

                continue;
            }

            google::protobuf::util::JsonParseOptions options;
            options.ignore_unknown_fields = false;
            google::protobuf::util::Status status;

            if( plannedAction == "update_stackup" )
            {
                if( desiredStackup )
                    return failure( "invalid_plan", "KDS planned more than one board stackup" );

                desiredStackup = std::make_unique<kiapi::board::BoardStackup>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "stackup" ).dump(), desiredStackup.get(), options );
            }
            else if( plannedAction == "update_rules" )
            {
                if( desiredRules )
                    return failure( "invalid_plan", "KDS planned more than one global rule set" );

                desiredRules = std::make_unique<kiapi::board::BoardDesignRules>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "rules" ).dump(), desiredRules.get(), options );
            }
            else
            {
                if( desiredNetClasses )
                    return failure( "invalid_plan", "KDS planned more than one netclass table" );

                desiredNetClasses =
                        std::make_unique<kiapi::common::project::NetClassSettings>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "settings" ).dump(), desiredNetClasses.get(), options );
            }

            if( !status.ok() )
                return failure( "invalid_plan", "KDS produced invalid native design settings" );
        }

        if( !desiredLibraryTables.empty() && desiredLibraryTables.size() != 2 )
        {
            return failure( "invalid_plan",
                            "KDS must replace symbol and footprint project tables together" );
        }

        const std::string sourceRelativePath = aArguments["path"].get<std::string>();
        const std::string projectName = compiled.ir["project"]["name"].get<std::string>();
        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT reconcileContext = {
            sourceRelativePath, boardRelativePath, projectName, compiled.sourceSha256
        };
        wxFileName statePath = sidecar;
        statePath.SetExt( wxS( "kicad_kds_state" ) );
        wxFileName journalPath = sidecar;
        journalPath.SetExt( wxS( "kicad_kds_journal" ) );
        JSON previousState;
        JSON journal;

        if( !readJsonFile( statePath, previousState, pathError )
            || !readJsonFile( journalPath, journal, pathError )
            || !mergeRecoveryJournal( journal, reconcileContext, previousState, pathError ) )
        {
            return failure( "invalid_managed_state", pathError );
        }

        JSON previousSchematicItems = JSON::array();

        if( previousState.is_object() && previousState.contains( "managedSchematicItems" ) )
        {
            if( !previousState["managedSchematicItems"].is_array() )
            {
                return failure( "invalid_managed_state",
                                "managed schematic ownership must be an array" );
            }

            previousSchematicItems = previousState["managedSchematicItems"];
        }

        std::map<std::string, JSON> preliminarySchematicFiles;

        if( !schematicPreplanned.operations.empty() )
        {
            if( schematicPreplanned.operations.size() != 1
                || schematicPreplanned.operations[0].value( "action", "" )
                           != "reconcile_schematic_hierarchy" )
            {
                return failure( "invalid_plan", "KDS produced an invalid schematic operation" );
            }

            for( const JSON& file : schematicPreplanned.operations[0]["files"] )
                preliminarySchematicFiles.emplace( file["path"].get<std::string>(), file );
        }

        std::set<std::string> schematicInventoryPaths;

        for( const auto& entry : preliminarySchematicFiles )
            schematicInventoryPaths.emplace( entry.first );

        for( const JSON& item : previousSchematicItems )
        {
            if( !item.is_object() || !item.contains( "file" ) || !item["file"].is_string() )
            {
                return failure( "invalid_managed_state",
                                "managed schematic ownership contains an invalid file path" );
            }

            schematicInventoryPaths.emplace( item["file"].get<std::string>() );
        }

        JSON liveSchematicFiles = JSON::array();
        JSON existingScreenUuids = JSON::object();
        std::map<std::string, wxFileName> resolvedSchematicPaths;
        size_t schematicInventoryBytes = 0;

        for( const std::string& relativePath : schematicInventoryPaths )
        {
            wxFileName resolved;
            bool present = false;
            std::string nativeSource;

            if( !resolveProjectSchematic( aProjectPath, relativePath, resolved, pathError )
                || !readOptionalSchematic( resolved, present, nativeSource, pathError ) )
            {
                return failure( "schematic_inventory_failed", pathError );
            }

            if( nativeSource.size() > MAX_SCHEMATIC_INVENTORY_BYTES
                                             - schematicInventoryBytes )
            {
                return failure( "schematic_inventory_failed",
                                "managed schematic inventory exceeds 32 MiB" );
            }

            schematicInventoryBytes += nativeSource.size();

            resolvedSchematicPaths.emplace( relativePath, resolved );
            JSON live = { { "path", relativePath }, { "present", present } };

            if( present )
                live["source"] = nativeSource;

            liveSchematicFiles.emplace_back( std::move( live ) );

            if( present && preliminarySchematicFiles.contains( relativePath ) )
            {
                std::string screenUuid;

                if( !schematicScreenUuid( nativeSource, screenUuid, pathError ) )
                    return failure( "schematic_inventory_failed", pathError );

                existingScreenUuids[relativePath] = screenUuid;
            }
        }

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPlanned =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                        compiled.ir, existingScreenUuids, resolvedSymbols.symbols );

        if( !schematicPlanned.fullyLowered )
        {
            return failure(
                    "backend_incomplete",
                    schematicPlanned.diagnostics.empty()
                            ? "schematic hierarchy could not be lowered against live files"
                            : schematicPlanned.diagnostics.front().value(
                                      "message", "schematic hierarchy planning failed" ) );
        }

        JSON schematicOperation = {
            { "action", "reconcile_schematic_hierarchy" },
            { "project", projectName },
            { "rootFile", "" },
            { "files", JSON::array() },
            { "managedItems", JSON::array() }
        };

        if( !schematicPlanned.operations.empty() )
            schematicOperation = schematicPlanned.operations[0];

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT schematicReconciled =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                        schematicOperation, previousSchematicItems, liveSchematicFiles );

        if( !schematicReconciled.ok )
        {
            return failure(
                    "schematic_reconcile_failed",
                    schematicReconciled.diagnostics.empty()
                            ? "managed schematic reconciliation failed"
                            : schematicReconciled.diagnostics.front().value(
                                      "message", "managed schematic reconciliation failed" ) );
        }

        std::vector<SCHEMATIC_FILE_UPDATE> schematicUpdates;

        for( const JSON& action : schematicReconciled.fileActions )
        {
            const std::string relativePath = action["path"].get<std::string>();
            auto resolved = resolvedSchematicPaths.find( relativePath );

            if( resolved == resolvedSchematicPaths.end() )
                return failure( "invalid_plan", "schematic action has no bounded inventory" );

            SCHEMATIC_FILE_UPDATE update;
            update.relativePath = relativePath;
            update.path = resolved->second;
            update.source = action["source"].get<std::string>();
            update.previousPresent = action["previousPresent"].get<bool>();
            update.previousSource = action["previousSource"].get<std::string>();
            schematicUpdates.emplace_back( std::move( update ) );
        }

        JSON managedOperations = JSON::array();
        JSON reconcileOperations = JSON::array();
        std::set<std::string> placementReferences;

        for( const JSON& plannedOperation : planned.operations )
        {
            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                managedOperations.push_back( plannedOperation );
                reconcileOperations.push_back( plannedOperation );
            }
            else if( action == "place_by_reference" )
            {
                placementReferences.emplace( plannedOperation["component"].get<std::string>() );
                reconcileOperations.push_back( plannedOperation );
            }
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT preflight =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        managedOperations, previousState, JSON::array(), reconcileContext );

        if( !preflight.ok )
        {
            return failure( "reconcile_failed",
                            preflight.diagnostics.empty()
                                    ? "managed PCB state failed validation"
                                    : preflight.diagnostics.front().value(
                                              "message", "managed PCB state failed validation" ) );
        }

        std::set<std::string> relevantIds;

        for( const JSON& plannedOperation : managedOperations )
            relevantIds.emplace( plannedOperation["itemId"].get<std::string>() );

        for( const std::string& reference : placementReferences )
        {
            relevantIds.emplace( KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    projectName, "footprint", reference ) );
        }

        if( !previousState.is_null() )
        {
            for( const JSON& item : previousState["managedPcbItems"] )
                relevantIds.emplace( item["itemId"].get<std::string>() );
        }

        std::vector<PROJECT_LIBRARY_TABLE_UPDATE> libraryTableUpdates;

        for( const JSON& desired : desiredLibraryTables )
        {
            PROJECT_LIBRARY_TABLE_UPDATE update;
            update.kind = desired["kind"].get<std::string>();
            update.source = desired["source"].get<std::string>();
            update.rows = desired["rows"].get<size_t>();

            if( !resolveProjectLibraryTable( aProjectPath,
                                             desired["path"].get<std::string>(),
                                             update.path, pathError )
                || !readOptionalTextFile( update.path, update.previousPresent,
                                          update.previousSource, pathError ) )
            {
                return failure( "library_table_inventory_failed", pathError );
            }

            libraryTableUpdates.emplace_back( std::move( update ) );
        }

        KICHAD_IPC_CLIENT client( "org.kichad.codex.design", aIpcSocketDirectory );
        KICHAD_IPC_TARGET target;

        if( !client.FindOpenPcb( aProjectPath, board.GetFullPath(), target, pathError ) )
            return failure( "pcb_not_open", pathError );

        kiapi::board::BoardStackup previousStackup;
        kiapi::board::BoardDesignRules previousRules;
        kiapi::common::project::NetClassSettings previousNetClasses;
        bool        previousCustomRulesPresent = false;
        std::string previousCustomRulesSource;

        if( desiredStackup && !queryPcbStackup( client, target, previousStackup, pathError ) )
            return failure( "stackup_inventory_failed", pathError );

        if( desiredRules && !queryPcbRules( client, target, previousRules, pathError ) )
            return failure( "rules_inventory_failed", pathError );

        if( desiredNetClasses
            && !queryNetClassSettings( client, target, previousNetClasses, pathError ) )
        {
            return failure( "netclass_inventory_failed", pathError );
        }

        if( hasDesiredCustomRules
            && !queryPcbCustomRules( client, target, previousCustomRulesPresent,
                                     previousCustomRulesSource, pathError ) )
        {
            return failure( "custom_rules_inventory_failed", pathError );
        }

        JSON liveInventory;

        if( !queryPcbInventory( client, target, relevantIds, liveInventory, pathError ) )
            return failure( "inventory_failed", pathError );

        if( !queryPcbFootprintInventory( client, target, placementReferences,
                                         liveInventory, pathError ) )
        {
            return failure( "inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT reconciled =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        reconcileOperations, previousState, liveInventory, reconcileContext );

        if( !reconciled.ok )
        {
            return failure( "reconcile_failed",
                            reconciled.diagnostics.empty()
                                    ? "managed PCB reconciliation failed"
                                    : reconciled.diagnostics.front().value(
                                              "message", "managed PCB reconciliation failed" ) );
        }

        reconciled.nextState["managedSchematicItems"] = schematicReconciled.managedItems;

        bool zoneMutation = false;
        std::set<std::string> expectedZoneIds;

        for( const JSON& managedOperation : managedOperations )
        {
            if( managedOperation.value( "itemType", "" ) == "zone" )
                expectedZoneIds.emplace( managedOperation["itemId"].get<std::string>() );
        }

        for( const JSON& action : reconciled.actions )
        {
            if( action.value( "itemType", "" ) == "zone" )
                zoneMutation = true;
        }

        JSON applyJournal = { { "format", "kichad-kds-apply-journal" },
                              { "version", 1 },
                              { "sourcePath", sourceRelativePath },
                              { "boardPath", boardRelativePath },
                              { "projectName", projectName },
                              { "sourceSha256", compiled.sourceSha256 },
                              { "previousState", previousState },
                              { "preparedState", reconciled.nextState } };

        if( desiredStackup )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousStackup, &serializedPrevious, options );

            if( !status.ok() )
                return failure( "journal_failed", "could not serialize the prior board stackup" );

            applyJournal["previousStackup"] = JSON::parse( serializedPrevious );
        }

        if( desiredRules )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousRules, &serializedPrevious, options );

            if( !status.ok() )
                return failure( "journal_failed", "could not serialize the prior board rules" );

            applyJournal["previousRules"] = JSON::parse( serializedPrevious );
        }

        if( desiredNetClasses )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousNetClasses, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior netclass settings" );
            }

            applyJournal["previousNetClassSettings"] = JSON::parse( serializedPrevious );
        }

        if( hasDesiredCustomRules )
        {
            applyJournal["previousCustomRules"] = {
                { "present", previousCustomRulesPresent },
                { "source", previousCustomRulesSource }
            };
        }

        if( !libraryTableUpdates.empty() )
        {
            applyJournal["previousProjectLibraryTables"] = JSON::array();

            for( const PROJECT_LIBRARY_TABLE_UPDATE& update : libraryTableUpdates )
            {
                applyJournal["previousProjectLibraryTables"].push_back(
                        { { "kind", update.kind },
                          { "path", update.path.GetFullName().ToStdString() },
                          { "present", update.previousPresent },
                          { "sourceBase64",
                            wxBase64Encode( update.previousSource.data(),
                                            update.previousSource.size() )
                                    .ToStdString() } } );
            }
        }

        if( !schematicUpdates.empty() )
        {
            applyJournal["previousSchematicFiles"] = JSON::array();

            for( const SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
            {
                applyJournal["previousSchematicFiles"].push_back(
                        { { "path", update.relativePath },
                          { "present", update.previousPresent },
                          { "sourceBase64",
                            wxBase64Encode( update.previousSource.data(),
                                            update.previousSource.size() )
                                    .ToStdString() } } );
            }
        }

        if( !writeJsonAtomically( journalPath, applyJournal, pathError ) )
            return failure( "journal_failed", pathError );

        bool stackupApplied = false;

        if( desiredStackup )
        {
            if( !updatePcbStackup( client, target, *desiredStackup, pathError ) )
            {
                std::string rollbackError;

                if( !updatePcbStackup( client, target, previousStackup, rollbackError ) )
                    pathError += "; stackup rollback also failed: " + rollbackError;

                return failure( "stackup_apply_failed",
                                pathError + "; the apply journal was retained for safe recovery" );
            }

            stackupApplied = true;
        }

        auto rollbackStackup = [&]( std::string& aMessage )
        {
            if( !stackupApplied )
                return;

            std::string rollbackError;

            if( !updatePcbStackup( client, target, previousStackup, rollbackError ) )
                aMessage += "; stackup rollback also failed: " + rollbackError;
        };

        bool rulesApplied = false;

        if( desiredRules )
        {
            if( !updatePcbRules( client, target, *desiredRules, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !updatePcbRules( client, target, previousRules, rollbackError ) )
                    message += "; board-rules rollback also failed: " + rollbackError;

                rollbackStackup( message );
                return failure( "rules_apply_failed", message );
            }

            rulesApplied = true;
        }

        auto rollbackBoardSettings = [&]( std::string& aMessage )
        {
            if( rulesApplied )
            {
                std::string rollbackError;

                if( !updatePcbRules( client, target, previousRules, rollbackError ) )
                    aMessage += "; board-rules rollback also failed: " + rollbackError;
            }

            rollbackStackup( aMessage );
        };

        bool netClassesApplied = false;

        if( desiredNetClasses )
        {
            if( !updateNetClassSettings( client, target, *desiredNetClasses, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !updateNetClassSettings( client, target, previousNetClasses, rollbackError ) )
                    message += "; netclass rollback also failed: " + rollbackError;

                rollbackBoardSettings( message );
                return failure( "netclass_apply_failed", message );
            }

            netClassesApplied = true;
        }

        auto rollbackSettings = [&]( std::string& aMessage )
        {
            if( netClassesApplied )
            {
                std::string rollbackError;

                if( !updateNetClassSettings(
                            client, target, previousNetClasses, rollbackError ) )
                {
                    aMessage += "; netclass rollback also failed: " + rollbackError;
                }
            }

            rollbackBoardSettings( aMessage );
        };

        bool customRulesApplied = false;

        if( hasDesiredCustomRules )
        {
            if( !updatePcbCustomRules( client, target, desiredCustomRulesPresent,
                                       desiredCustomRulesSource, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !updatePcbCustomRules( client, target, previousCustomRulesPresent,
                                           previousCustomRulesSource, rollbackError ) )
                {
                    message += "; custom-rules rollback also failed: " + rollbackError;
                }

                rollbackSettings( message );
                return failure( "custom_rules_apply_failed", message );
            }

            customRulesApplied = true;
        }

        auto rollbackAllSettings = [&]( std::string& aMessage )
        {
            if( customRulesApplied )
            {
                std::string rollbackError;

                if( !updatePcbCustomRules( client, target, previousCustomRulesPresent,
                                           previousCustomRulesSource, rollbackError ) )
                {
                    aMessage += "; custom-rules rollback also failed: " + rollbackError;
                }
            }

            rollbackSettings( aMessage );
        };

        size_t libraryTablesApplied = 0;

        for( PROJECT_LIBRARY_TABLE_UPDATE& update : libraryTableUpdates )
        {
            if( !installTextFileAtomically( update.path, true, update.source, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !installTextFileAtomically( update.path, update.previousPresent,
                                                update.previousSource, rollbackError ) )
                {
                    message += "; current library-table rollback also failed: "
                               + rollbackError;
                }

                for( auto prior = libraryTableUpdates.rbegin();
                     prior != libraryTableUpdates.rend(); ++prior )
                {
                    if( !prior->applied )
                        continue;

                    rollbackError.clear();

                    if( !installTextFileAtomically( prior->path, prior->previousPresent,
                                                    prior->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( "library_table_apply_failed", message );
            }

            update.applied = true;
            ++libraryTablesApplied;
        }

        size_t schematicFilesApplied = 0;

        auto rollbackSchematicFiles = [&]( std::string& aMessage )
        {
            for( auto update = schematicUpdates.rbegin();
                 update != schematicUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !installSchematicAtomically( update->path, update->previousPresent,
                                                 update->previousSource, rollbackError ) )
                {
                    aMessage += "; schematic rollback also failed: " + rollbackError;
                }
            }
        };

        for( SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
        {
            if( !installSchematicAtomically( update.path, true, update.source, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !installSchematicAtomically( update.path, update.previousPresent,
                                                 update.previousSource, rollbackError ) )
                {
                    message += "; current schematic rollback also failed: " + rollbackError;
                }

                rollbackSchematicFiles( message );

                for( auto library = libraryTableUpdates.rbegin();
                     library != libraryTableUpdates.rend(); ++library )
                {
                    if( !library->applied )
                        continue;

                    rollbackError.clear();

                    if( !installTextFileAtomically( library->path, library->previousPresent,
                                                    library->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( "schematic_apply_failed", message );
            }

            update.applied = true;
            ++schematicFilesApplied;
        }

        if( schematicFilesApplied > 0 )
        {
            pathError.clear();
            std::vector<wxFileName> validationRoots;
            const std::string rootRelativePath = schematicOperation.value( "rootFile", "" );

            if( !rootRelativePath.empty() )
            {
                auto root = resolvedSchematicPaths.find( rootRelativePath );

                if( root == resolvedSchematicPaths.end() )
                    pathError = "planned root schematic has no bounded path";
                else
                    validationRoots.emplace_back( root->second );
            }
            else
            {
                for( const SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
                    validationRoots.emplace_back( update.path );
            }

            for( const wxFileName& validationRoot : validationRoots )
            {
                const bool nativeValid = pathError.empty()
                                         && ( m_schematicValidator
                                                      ? m_schematicValidator(
                                                                validationRoot, pathError )
                                                      : validateNativeSchematicHierarchy(
                                                                validationRoot, pathError ) );

                if( !nativeValid )
                {
                    std::string message = pathError
                                          + "; the apply journal was retained for safe recovery";
                    rollbackSchematicFiles( message );

                    for( auto library = libraryTableUpdates.rbegin();
                         library != libraryTableUpdates.rend(); ++library )
                    {
                        if( !library->applied )
                            continue;

                        std::string rollbackError;

                        if( !installTextFileAtomically(
                                    library->path, library->previousPresent,
                                    library->previousSource, rollbackError ) )
                        {
                            message += "; library-table rollback also failed: "
                                       + rollbackError;
                        }
                    }

                    rollbackAllSettings( message );
                    return failure( "schematic_validation_failed", message );
                }
            }
        }

        auto rollbackAllArtifacts = [&]( std::string& aMessage )
        {
            rollbackSchematicFiles( aMessage );

            for( auto update = libraryTableUpdates.rbegin();
                 update != libraryTableUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !installTextFileAtomically( update->path, update->previousPresent,
                                                update->previousSource, rollbackError ) )
                {
                    aMessage += "; library-table rollback also failed: " + rollbackError;
                }
            }

            rollbackAllSettings( aMessage );
        };

        if( reconciled.actions.empty() )
        {
            if( !writeJsonAtomically( statePath, reconciled.nextState, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                rollbackAllArtifacts( message );
                return failure( "state_write_failed", message );
            }

            const bool journalRemoved = wxRemoveFile( journalPath.GetFullPath() );
            JSON payload = { { "operation", "apply" },
                             { "path", sourceRelativePath },
                             { "boardPath", boardRelativePath },
                             { "sourceSha256", compiled.sourceSha256 },
                             { "counts", reconciled.counts },
                             { "transaction",
                               stackupApplied || rulesApplied || netClassesApplied
                                               || customRulesApplied || libraryTablesApplied > 0
                                               || schematicFilesApplied > 0
                                       ? "design settings applied"
                                       : "no board changes" },
                             { "stackupApplied", stackupApplied },
                             { "rulesApplied", rulesApplied },
                             { "netClassesApplied", netClassesApplied },
                             { "customRulesApplied", customRulesApplied },
                             { "libraryTablesApplied", libraryTablesApplied },
                             { "schematicFilesApplied", schematicFilesApplied },
                             { "schematicCounts", schematicReconciled.counts },
                             { "journalRetained", !journalRemoved } };
            return success( payload );
        }

        KICHAD_IPC_COMMIT_GUARD commit( client, target );

        if( !commit.Begin( pathError ) )
        {
            std::string message = pathError
                                  + "; the apply journal was retained for safe recovery";
            rollbackAllArtifacts( message );
            return failure( "transaction_failed", message );
        }

        if( !executePcbActions( client, target, reconciled.actions,
                                footprintSources, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackAllArtifacts( message );

            return failure( "apply_failed", message );
        }

        if( !commit.Commit( "Apply KiChad Design Script " + sourceRelativePath, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackAllArtifacts( message );

            return failure( "transaction_failed", message );
        }

        if( zoneMutation && !refillPcbZones( client, target, expectedZoneIds, pathError ) )
        {
            return failure( "zone_refill_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        if( !writeJsonAtomically( statePath, reconciled.nextState, pathError ) )
        {
            return failure( "state_write_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        const bool journalRemoved = wxRemoveFile( journalPath.GetFullPath() );
        JSON payload = { { "operation", "apply" },
                         { "path", sourceRelativePath },
                         { "boardPath", boardRelativePath },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "counts", reconciled.counts },
                         { "managedItems",
                           reconciled.nextState["managedPcbItems"].size() },
                         { "transaction", "committed" },
                         { "stackupApplied", stackupApplied },
                         { "rulesApplied", rulesApplied },
                         { "netClassesApplied", netClassesApplied },
                         { "customRulesApplied", customRulesApplied },
                         { "libraryTablesApplied", libraryTablesApplied },
                         { "schematicFilesApplied", schematicFilesApplied },
                         { "schematicCounts", schematicReconciled.counts },
                         { "zonesRefilled", zoneMutation ? expectedZoneIds.size() : 0 },
                         { "statePath", statePath.GetFullName().ToStdString() },
                         { "journalRetained", !journalRemoved } };
        return success( payload );
    }

    if( !aMutationAvailable )
        return failure( "snapshot_required", "A pre-turn project snapshot is required to save KDS" );

    if( !compiled.ok )
    {
        std::string message = "KiChad Design Script did not pass compilation";

        if( !compiled.diagnostics.empty() )
            message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

        return failure( "compile_failed", message );
    }

    if( sidecar.FileExists() )
    {
        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string() )
        {
            return failure( "stale_source",
                            "expectedSha256 is required when replacing an existing sidecar" );
        }

        std::string existing;

        if( !readDesignScriptSidecar( sidecar, existing, pathError ) )
            return failure( "read_failed", pathError );

        std::string existingSha256;
        picosha2::hash256_hex_string( existing, existingSha256 );

        if( aArguments["expectedSha256"].get<std::string>() != existingSha256 )
            return failure( "stale_source", "sidecar changed since it was loaded" );
    }

    const wxString temporaryPath =
            sidecar.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( source.data(), source.size() ) != source.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        return failure( "write_failed", "could not durably write the sidecar temporary file" );
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, sidecar.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        return failure( "write_failed", "could not atomically install the sidecar" );
    }

    std::string installed;

    if( !readDesignScriptSidecar( sidecar, installed, pathError ) || installed != source )
        return failure( "write_failed", "sidecar verification failed after atomic installation" );

    JSON payload = { { "operation", "save" },
                     { "path", aArguments["path"] },
                     { "bytes", source.size() },
                     { "sourceSha256", compiled.sourceSha256 },
                     { "valid", true },
                     { "plan", compiled.plan },
                     { "transaction", "snapshot-backed atomic save" } };
    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handlePcb(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        const wxString& aIpcSocketDirectory ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() || !aArguments.contains( "path" )
        || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "pcb.operation and pcb.path must be strings" );
    }

    if( aArguments.dump().size() > MAX_PCB_ARGUMENT_BYTES )
        return failure( "invalid_arguments", "pcb arguments are limited to 1 MiB" );

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "status" && operation != "describe" && operation != "get"
        && operation != "mutate" )
    {
        return failure( "invalid_arguments",
                        "pcb.operation must be 'status', 'describe', 'get', or 'mutate'" );
    }

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName       resolved;
    std::string      pathError;

    if( !resolveProjectFile( aProjectPath, relativePath, resolved, pathError )
        || resolved.GetExt() != wxS( "kicad_pcb" ) )
    {
        if( pathError.empty() )
            pathError = "pcb.path must identify a project .kicad_pcb file";

        return failure( "invalid_path", pathError );
    }

    if( operation == "mutate" && !aMutationAvailable )
    {
        return failure( "snapshot_required",
                        "A complete pre-turn project snapshot is required before PCB mutation" );
    }

    JSON payload = { { "operation", operation },
                     { "path", relativePath },
                     { "mutationAvailable", aMutationAvailable },
                     { "transport", "KiCad 10 protobuf IPC" } };

    std::string itemType;
    kiapi::common::types::KiCadObjectType objectType =
            kiapi::common::types::KOT_UNKNOWN;

    if( operation != "status" )
    {
        if( !aArguments.contains( "itemType" ) || !aArguments["itemType"].is_string() )
            return failure( "invalid_arguments", "pcb.itemType must be a supported string" );

        itemType = aArguments["itemType"].get<std::string>();
        objectType = pcbObjectType( itemType );

        if( objectType == kiapi::common::types::KOT_UNKNOWN )
            return failure( "invalid_arguments", "pcb.itemType is not supported" );
    }

    if( operation == "describe" )
    {
        std::string messagePath;

        if( aArguments.contains( "messagePath" ) )
        {
            if( !aArguments["messagePath"].is_string()
                || aArguments["messagePath"].get_ref<const std::string&>().size() > 512 )
            {
                return failure( "invalid_arguments",
                                "pcb.messagePath must be a string of at most 512 bytes" );
            }

            messagePath = aArguments["messagePath"].get<std::string>();
        }

        std::string descriptorError;
        JSON descriptor = describePcbMessage( itemType, messagePath, descriptorError );

        if( !descriptorError.empty() )
            return failure( "invalid_arguments", descriptorError );

        payload["schema"] = std::move( descriptor );
        payload["editorRequired"] = false;
        return success( payload );
    }

    KICHAD_IPC_CLIENT client( "org.kichad.codex-" + std::to_string( wxGetProcessId() ),
                              aIpcSocketDirectory );
    KICHAD_IPC_TARGET target;
    std::string       ipcError;
    bool editorOpen = client.FindOpenPcb( aProjectPath, resolved.GetFullPath(), target, ipcError );
    payload["editorOpen"] = editorOpen;

    if( !editorOpen )
    {
        if( operation == "status" )
        {
            payload["detail"] = ipcError;
            return success( payload );
        }

        return failure( "pcb_editor_unavailable", ipcError );
    }

    payload["boardFilename"] = target.document.board_filename();
    payload["projectName"] = target.document.project().name();

    if( operation == "status" )
        return success( payload );

    std::vector<std::string> fieldMask;

    if( aArguments.contains( "fieldMask" ) )
    {
        if( !aArguments["fieldMask"].is_array() || aArguments["fieldMask"].size() > 32 )
            return failure( "invalid_arguments", "pcb.fieldMask must contain at most 32 paths" );

        for( const JSON& field : aArguments["fieldMask"] )
        {
            if( !field.is_string() || field.get_ref<const std::string&>().empty()
                || field.get_ref<const std::string&>().size() > 128 )
            {
                return failure( "invalid_arguments", "pcb.fieldMask contains an invalid path" );
            }

            fieldMask.emplace_back( field.get<std::string>() );
        }

        std::unique_ptr<google::protobuf::Message> prototype = newPcbItem( itemType );

        for( const std::string& field : fieldMask )
        {
            if( !google::protobuf::util::FieldMaskUtil::GetFieldDescriptors(
                        prototype->GetDescriptor(), field, nullptr ) )
            {
                return failure( "invalid_arguments",
                                "pcb.fieldMask contains a path that is invalid for " + itemType );
            }
        }
    }

    if( operation == "get" )
    {
        int limit = 50;

        if( aArguments.contains( "limit" ) )
        {
            if( !aArguments["limit"].is_number_integer() )
                return failure( "invalid_arguments", "pcb.limit must be an integer" );

            limit = aArguments["limit"].get<int>();

            if( limit < 1 || limit > 200 )
                return failure( "invalid_arguments", "pcb.limit must be between 1 and 200" );
        }

        kiapi::common::commands::GetItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );
        request.add_types( objectType );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::GetItemsResponse itemsResponse;

        if( !response.message().UnpackTo( &itemsResponse )
            || itemsResponse.status() != kiapi::common::types::IRS_OK )
        {
            return failure( "ipc_failed", "KiCad returned an invalid get-items response" );
        }

        JSON   items = JSON::array();
        size_t resultBytes = 0;
        size_t totalItems = 0;
        bool   resultCapacityReached = false;

        for( const google::protobuf::Any& item : itemsResponse.items() )
        {
            if( ( itemType == "zone" || itemType == "rule_area" )
                && pcbAnyType( item ) != itemType )
            {
                continue;
            }

            ++totalItems;

            if( resultCapacityReached || items.size() >= static_cast<size_t>( limit ) )
            {
                resultCapacityReached = true;
                continue;
            }

            std::unique_ptr<google::protobuf::Message> message = newPcbItem( itemType );

            if( !message || !item.UnpackTo( message.get() ) )
                return failure( "ipc_failed", "KiCad returned an unexpected PCB item type" );

            if( !fieldMask.empty() )
            {
                google::protobuf::FieldMask mask;

                for( const std::string& field : fieldMask )
                    mask.add_paths( field );

                mask.add_paths( "id" );
                google::protobuf::util::FieldMaskUtil::TrimMessage( mask, message.get() );
            }

            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            std::string serialized;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString( *message, &serialized, options );

            if( !status.ok() )
                return failure( "ipc_failed", status.ToString() );

            if( resultBytes + serialized.size() > MAX_PCB_RESULT_BYTES )
            {
                resultCapacityReached = true;
                continue;
            }

            resultBytes += serialized.size();
            items.emplace_back( JSON::parse( serialized ) );
        }

        payload["itemType"] = itemType;
        payload["totalItems"] = totalItems;
        payload["items"] = std::move( items );
        payload["resultTruncated"] = payload["items"].size() < totalItems;
        return success( payload );
    }

    if( !aArguments.contains( "action" ) || !aArguments["action"].is_string() )
        return failure( "invalid_arguments", "pcb.action must be create, update, or delete" );

    const std::string action = aArguments["action"].get<std::string>();

    if( action != "create" && action != "update" && action != "delete" )
        return failure( "invalid_arguments", "pcb.action must be create, update, or delete" );

    std::string commitMessage = "Codex " + action + " PCB " + itemType;

    if( aArguments.contains( "commitMessage" ) )
    {
        if( !aArguments["commitMessage"].is_string()
            || aArguments["commitMessage"].get_ref<const std::string&>().size() > 256 )
        {
            return failure( "invalid_arguments", "pcb.commitMessage must be at most 256 bytes" );
        }

        if( !aArguments["commitMessage"].get_ref<const std::string&>().empty() )
            commitMessage = aArguments["commitMessage"].get<std::string>();
    }

    KICHAD_IPC_COMMIT_GUARD commit( client, target );

    if( action == "delete" )
    {
        if( !aArguments.contains( "ids" ) || !aArguments["ids"].is_array()
            || aArguments["ids"].empty() || aArguments["ids"].size() > 500 )
        {
            return failure( "invalid_arguments", "pcb.ids must contain 1 to 500 UUIDs" );
        }

        kiapi::common::commands::DeleteItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );

        for( const JSON& id : aArguments["ids"] )
        {
            if( !id.is_string() || !KIID::SniffTest( wxString::FromUTF8( id.get<std::string>() ) ) )
                return failure( "invalid_arguments", "pcb.ids contains an invalid KiCad UUID" );

            request.add_item_ids()->set_value( id.get<std::string>() );
        }

        if( !commit.Begin( ipcError ) )
            return failure( "transaction_failed", ipcError );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::DeleteItemsResponse deleted;

        if( !response.message().UnpackTo( &deleted )
            || deleted.status() != kiapi::common::types::IRS_OK
            || deleted.deleted_items_size() != request.item_ids_size() )
        {
            return failure( "ipc_failed", "KiCad rejected one or more PCB deletions" );
        }

        for( const kiapi::common::commands::ItemDeletionResult& item : deleted.deleted_items() )
        {
            if( item.status() != kiapi::common::commands::IDS_OK )
                return failure( "ipc_failed", "KiCad rejected one or more PCB deletions" );
        }

        if( !commit.Commit( commitMessage, ipcError ) )
            return failure( "transaction_failed", ipcError );

        payload["action"] = action;
        payload["itemType"] = itemType;
        payload["affectedItems"] = aArguments["ids"].size();
        payload["transaction"] = "committed";
        return success( payload );
    }

    if( !aArguments.contains( "items" ) || !aArguments["items"].is_array()
        || aArguments["items"].empty() || aArguments["items"].size() > 200 )
    {
        return failure( "invalid_arguments", "pcb.items must contain 1 to 200 objects" );
    }

    if( action == "update" && fieldMask.empty() )
        return failure( "invalid_arguments", "pcb.fieldMask is required for update" );

    if( action == "update" )
    {
        for( const std::string& field : fieldMask )
        {
            if( field == "id" || field.starts_with( "id." ) )
                return failure( "invalid_arguments", "pcb.fieldMask cannot update an item UUID" );
        }
    }

    if( action == "create" )
    {
        kiapi::common::commands::CreateItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );

        for( const JSON& item : aArguments["items"] )
        {
            std::unique_ptr<google::protobuf::Message> message;

            if( !parsePcbItem( itemType, item, message, ipcError ) )
                return failure( "invalid_arguments", ipcError );

            request.add_items()->PackFrom( *message );
        }

        if( !commit.Begin( ipcError ) )
            return failure( "transaction_failed", ipcError );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::CreateItemsResponse created;

        if( !response.message().UnpackTo( &created )
            || created.status() != kiapi::common::types::IRS_OK
            || created.created_items_size() != request.items_size() )
        {
            return failure( "ipc_failed", "KiCad returned an invalid create-items response" );
        }

        JSON ids = JSON::array();

        for( const kiapi::common::commands::ItemCreationResult& item : created.created_items() )
        {
            if( item.status().code() != kiapi::common::commands::ISC_OK )
                return failure( "ipc_failed", item.status().error_message() );

            std::string id = pcbItemId( item.item(), itemType );

            if( !id.empty() )
                ids.emplace_back( std::move( id ) );
        }

        if( !commit.Commit( commitMessage, ipcError ) )
            return failure( "transaction_failed", ipcError );

        payload["action"] = action;
        payload["itemType"] = itemType;
        payload["affectedItems"] = created.created_items_size();
        payload["itemIds"] = std::move( ids );
        payload["transaction"] = "committed";
        return success( payload );
    }

    kiapi::common::commands::UpdateItems request;
    request.mutable_header()->mutable_document()->CopyFrom( target.document );

    for( const std::string& field : fieldMask )
        request.mutable_header()->mutable_field_mask()->add_paths( field );

    for( const JSON& item : aArguments["items"] )
    {
        std::unique_ptr<google::protobuf::Message> message;

        if( !parsePcbItem( itemType, item, message, ipcError ) )
            return failure( "invalid_arguments", ipcError );

        request.add_items()->PackFrom( *message );
    }

    if( !commit.Begin( ipcError ) )
        return failure( "transaction_failed", ipcError );

    kiapi::common::ApiResponse response;

    if( !client.Call( target, request, response, ipcError ) )
        return failure( "ipc_failed", ipcError );

    kiapi::common::commands::UpdateItemsResponse updated;

    if( !response.message().UnpackTo( &updated )
        || updated.status() != kiapi::common::types::IRS_OK
        || updated.updated_items_size() != request.items_size() )
    {
        return failure( "ipc_failed", "KiCad returned an invalid update-items response" );
    }

    for( const kiapi::common::commands::ItemUpdateResult& item : updated.updated_items() )
    {
        if( item.status().code() != kiapi::common::commands::ISC_OK )
            return failure( "ipc_failed", item.status().error_message() );
    }

    if( !commit.Commit( commitMessage, ipcError ) )
        return failure( "transaction_failed", ipcError );

    payload["action"] = action;
    payload["itemType"] = itemType;
    payload["affectedItems"] = updated.updated_items_size();
    payload["transaction"] = "committed";
    return success( payload );
}


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

    if( !resolveProjectFile( root, relativePath, resolved, pathError ) )
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

    if( rootHead != expectedRootHead( resolved.GetExt() ) )
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
    JSON                      expressions = JSON::array();
    size_t                    resultBytes = 0;

    for( size_t i = 0; i < matches.size() && expressions.size() < static_cast<size_t>( limit ); ++i )
    {
        std::string raw = document->RawText( matches[i] );
        bool        truncated = raw.size() > MAX_EXPRESSION_BYTES;

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


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleSourcingVerify(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    int offset = 0;
    int limit = 100;
    int maxAgeDays = 7;

    const auto boundedInteger = [&]( const char* aName, int64_t aMinimum, int64_t aMaximum,
                                     int& aValue ) -> JSON
    {
        if( !aArguments.contains( aName ) )
            return nullptr;

        if( !aArguments[aName].is_number_integer() )
            return failure( "invalid_arguments", std::string( "verify." ) + aName
                                                         + " must be an integer" );

        const int64_t parsed = aArguments[aName].get<int64_t>();

        if( parsed < aMinimum || parsed > aMaximum )
        {
            return failure( "invalid_arguments", std::string( "verify." ) + aName
                                                         + " is outside its allowed range" );
        }

        aValue = static_cast<int>( parsed );
        return nullptr;
    };

    for( const auto& [name, minimum, maximum, destination] :
         std::array<std::tuple<const char*, int64_t, int64_t, int*>, 3>{
                 std::tuple{ "offset", 0, 1000000, &offset },
                 std::tuple{ "limit", 1, 200, &limit },
                 std::tuple{ "maxAgeDays", 1, 90, &maxAgeDays } } )
    {
        JSON error = boundedInteger( name, minimum, maximum, *destination );

        if( !error.is_null() )
            return error;
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName sidecar;
    std::string pathError;

    if( !resolveProjectSidecar( aProjectPath, relativePath, sidecar, pathError ) )
        return failure( "invalid_path", pathError );

    if( !sidecar.FileExists() )
        return failure( "read_failed", "KiChad Design Script sidecar does not exist" );

    std::string source;

    if( !readDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( !compiled.ok )
    {
        return failure( "compile_failed",
                        "KDS must compile before sourcing verification; use design.compile "
                        "for structured diagnostics" );
    }

    std::set<std::string> requiredComponents;

    for( const JSON& component : compiled.ir["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string() )
        {
            return failure( "compile_failed", "KDS compiler returned invalid component data" );
        }

        const std::string reference = component["reference"].get<std::string>();

        if( component.contains( "footprint" ) && !component["footprint"].is_null() )
            requiredComponents.emplace( reference );
    }

    std::map<std::string, const JSON*> records;

    for( const JSON& record : compiled.ir["sourcing"] )
    {
        if( !record.is_object() || !record.contains( "component" )
            || !record["component"].is_string() )
        {
            return failure( "compile_failed", "KDS compiler returned invalid sourcing data" );
        }

        records.emplace( record["component"].get<std::string>(), &record );
    }

    JSON issues = JSON::array();
    std::set<std::string> componentsWithIssues;
    const auto addIssue = [&]( const std::string& aComponent, const std::string& aType,
                               const std::string& aSeverity, const std::string& aDescription,
                               JSON aDetail = JSON::object() )
    {
        JSON issue = { { "category", "sourcing" },
                       { "type", aType },
                       { "severity", aSeverity },
                       { "description", aDescription },
                       { "component", aComponent } };

        if( aDetail.is_object() )
            issue.update( aDetail );

        issues.push_back( std::move( issue ) );
        componentsWithIssues.emplace( aComponent );
    };

    static constexpr const char* REQUIRED_FIELDS[] = {
        "manufacturer", "mpn",       "datasheet",  "lifecycle", "supplier",
        "sku",          "product_url", "available", "verified_on", "quantity"
    };
    const wxDateTime today = wxDateTime::Today();

    for( const std::string& reference : requiredComponents )
    {
        auto recordIt = records.find( reference );

        if( recordIt == records.end() )
        {
            addIssue( reference, "missing_source", "error",
                      "Physical component has no cached sourcing evidence" );
            continue;
        }

        const JSON& record = *recordIt->second;
        JSON missing = JSON::array();

        for( const char* field : REQUIRED_FIELDS )
        {
            if( !record.contains( field ) )
                missing.push_back( field );
        }

        if( !missing.empty() )
        {
            addIssue( reference, "missing_evidence_field", "error",
                      "Sourcing evidence is incomplete", { { "missingFields", missing } } );
            continue;
        }

        const std::string lifecycle = record["lifecycle"].get<std::string>();

        if( lifecycle != "active" )
        {
            addIssue( reference, "non_active_lifecycle",
                      lifecycle == "nrnd" ? "warning" : "error",
                      "Component lifecycle is not active", { { "lifecycle", lifecycle } } );
        }

        const int64_t available = record["available"].get<int64_t>();

        if( available <= 0 )
        {
            addIssue( reference, "not_orderable", "error",
                      "Distributor evidence reports no available stock",
                      { { "available", available }, { "supplier", record["supplier"] } } );
        }

        const std::string verifiedOn = record["verified_on"].get<std::string>();
        const int year = std::stoi( verifiedOn.substr( 0, 4 ) );
        const int month = std::stoi( verifiedOn.substr( 5, 2 ) );
        const int day = std::stoi( verifiedOn.substr( 8, 2 ) );
        const wxDateTime verified( day, static_cast<wxDateTime::Month>( month - 1 ), year );
        const int ageDays = ( today - verified ).GetDays();

        if( ageDays < 0 )
        {
            addIssue( reference, "future_evidence", "error",
                      "Sourcing evidence date is in the future",
                      { { "verifiedOn", verifiedOn } } );
        }
        else if( ageDays > maxAgeDays )
        {
            addIssue( reference, "stale_evidence", "error",
                      "Sourcing evidence is older than the allowed age",
                      { { "verifiedOn", verifiedOn }, { "ageDays", ageDays } } );
        }
    }

    for( const auto& entry : records )
    {
        const std::string& reference = entry.first;

        if( !requiredComponents.contains( reference ) )
        {
            addIssue( reference, "non_physical_source", "warning",
                      "Sourcing evidence belongs to a component with no physical footprint" );
        }
    }

    size_t errorCount = 0;
    size_t warningCount = 0;

    for( const JSON& issue : issues )
    {
        if( issue["severity"] == "error" )
            ++errorCount;
        else
            ++warningCount;
    }

    size_t completeComponents = 0;

    for( const std::string& reference : requiredComponents )
    {
        if( !componentsWithIssues.contains( reference ) )
            ++completeComponents;
    }

    const size_t totalIssues = issues.size();
    JSON page = JSON::array();

    for( size_t i = static_cast<size_t>( offset ); i < totalIssues
                                                      && page.size() < static_cast<size_t>( limit );
         ++i )
    {
        page.push_back( issues[i] );
    }

    const size_t returned = page.size();
    const bool hasMore = static_cast<size_t>( offset ) + returned < totalIssues;
    JSON payload = {
        { "operation", "sourcing" },
        { "path", relativePath },
        { "sourceSha256", compiled.sourceSha256 },
        { "verifiedOn", std::string( today.FormatISODate().ToUTF8() ) },
        { "maxAgeDays", maxAgeDays },
        { "clean", totalIssues == 0 },
        { "waiversPresent", false },
        { "counts",
          { { "total", totalIssues },
            { "errors", errorCount },
            { "warnings", warningCount },
            { "exclusions", 0 },
            { "other", 0 },
            { "categories", { { "sourcing", totalIssues } } } } },
        { "sourcing",
          { { "requiredComponents", requiredComponents.size() },
            { "sourceRecords", records.size() },
            { "completeComponents", completeComponents } } },
        { "ignoredChecksCount", 0 },
        { "ignoredChecks", JSON::array() },
        { "ignoredChecksTruncated", false },
        { "offset", offset },
        { "returnedViolations", returned },
        { "violations", std::move( page ) },
        { "resultTruncated", hasMore }
    };

    if( hasMore )
        payload["nextOffset"] = static_cast<size_t>( offset ) + returned;

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleVerify(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() || !aArguments.contains( "path" )
        || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "verify.operation and verify.path must be strings" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();
    const std::string relativePath = aArguments["path"].get<std::string>();

    if( operation != "erc" && operation != "drc" && operation != "sourcing" )
    {
        return failure( "invalid_arguments",
                        "verify.operation must be 'erc', 'drc', or 'sourcing'" );
    }

    if( operation == "sourcing" )
        return handleSourcingVerify( aArguments, aProjectPath );

    if( aArguments.contains( "maxAgeDays" ) )
        return failure( "invalid_arguments", "verify.maxAgeDays applies only to sourcing" );

    int offset = 0;
    int limit = 100;

    if( aArguments.contains( "offset" ) )
    {
        if( !aArguments["offset"].is_number_integer() )
            return failure( "invalid_arguments", "verify.offset must be an integer" );

        const int64_t parsedOffset = aArguments["offset"].get<int64_t>();

        if( parsedOffset < 0 || parsedOffset > 1000000 )
            return failure( "invalid_arguments", "verify.offset must be between 0 and 1000000" );

        offset = static_cast<int>( parsedOffset );
    }

    if( aArguments.contains( "limit" ) )
    {
        if( !aArguments["limit"].is_number_integer() )
            return failure( "invalid_arguments", "verify.limit must be an integer" );

        const int64_t parsedLimit = aArguments["limit"].get<int64_t>();

        if( parsedLimit < 1 || parsedLimit > 200 )
            return failure( "invalid_arguments", "verify.limit must be between 1 and 200" );

        limit = static_cast<int>( parsedLimit );
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    wxFileName resolved;
    std::string pathError;

    if( !resolveProjectFile( aProjectPath, relativePath, resolved, pathError ) )
        return failure( "invalid_path", pathError );

    const wxString expectedExtension = operation == "erc" ? wxS( "kicad_sch" )
                                                            : wxS( "kicad_pcb" );

    if( resolved.GetExt() != expectedExtension )
    {
        return failure( "invalid_path", operation == "erc"
                                                ? "ERC requires a .kicad_sch file"
                                                : "DRC requires a .kicad_pcb file" );
    }

    std::string reportSource;
    std::string checkError;
    const bool ran = m_nativeCheckRunner
                             ? m_nativeCheckRunner( operation, resolved, reportSource, checkError )
                             : runNativeKiCadCheck( operation, resolved, reportSource, checkError );

    if( !ran )
    {
        if( checkError.empty() )
            checkError = "native KiCad verification failed without an error message";

        return failure( "check_failed", checkError );
    }

    if( reportSource.empty() || reportSource.size() > MAX_VERIFICATION_REPORT_BYTES )
    {
        return failure( "invalid_report",
                        "native verification report must contain 1 byte to 64 MiB" );
    }

    JSON report = JSON::parse( reportSource, nullptr, false );

    if( report.is_discarded() || !report.is_object() )
        return failure( "invalid_report", "native verification report is not valid JSON" );

    const auto hasString = [&]( const char* aName )
    {
        return report.contains( aName ) && report[aName].is_string();
    };

    if( !hasString( "$schema" ) || !hasString( "source" ) || !hasString( "date" )
        || !hasString( "kicad_version" ) || !hasString( "coordinate_units" )
        || !report.contains( "included_severities" )
        || !report["included_severities"].is_array() || !report.contains( "ignored_checks" )
        || !report["ignored_checks"].is_array() )
    {
        return failure( "invalid_report", "native verification report is missing required fields" );
    }

    const std::string expectedSchema = operation == "erc"
                                               ? "https://schemas.kicad.org/erc.v1.json"
                                               : "https://schemas.kicad.org/drc.v1.json";
    const std::string nativeVersion( GetMajorMinorPatchVersion().ToUTF8() );

    if( report["$schema"].get<std::string>() != expectedSchema
        || report["coordinate_units"].get<std::string>() != "mm"
        || report["source"].get<std::string>()
                   != std::string( resolved.GetFullName().ToUTF8() ) )
    {
        return failure( "invalid_report",
                        "native verification report schema, units, or source does not match" );
    }

    if( report["kicad_version"].get<std::string>() != nativeVersion )
    {
        return failure( "version_mismatch",
                        "native verification report was not produced by this KiChad version" );
    }

    std::set<std::string> includedSeverities;

    for( const JSON& severity : report["included_severities"] )
    {
        if( !severity.is_string() )
            return failure( "invalid_report", "included severity is not a string" );

        includedSeverities.emplace( severity.get<std::string>() );
    }

    if( report["included_severities"].size() != 3
        || includedSeverities != std::set<std::string>{ "error", "exclusion", "warning" } )
    {
        return failure( "invalid_report",
                        "native verification report does not include every severity" );
    }

    JSON ignoredChecks = JSON::array();
    size_t ignoredCheckBytes = 0;

    for( const JSON& ignored : report["ignored_checks"] )
    {
        if( !ignored.is_object() || !ignored.contains( "key" ) || !ignored["key"].is_string()
            || !ignored.contains( "description" ) || !ignored["description"].is_string() )
        {
            return failure( "invalid_report", "ignored check entry is malformed" );
        }

        const size_t entryBytes = ignored.dump().size();

        if( ignoredChecks.size() < MAX_VERIFICATION_IGNORED_CHECKS
            && ignoredCheckBytes + entryBytes <= MAX_VERIFICATION_IGNORED_BYTES )
        {
            ignoredChecks.push_back( ignored );
            ignoredCheckBytes += entryBytes;
        }
    }

    JSON violations = JSON::array();
    JSON categoryCounts = JSON::object();
    size_t total = 0;
    size_t errors = 0;
    size_t warnings = 0;
    size_t exclusions = 0;
    size_t other = 0;
    size_t resultBytes = ignoredCheckBytes;
    bool   resultByteLimited = false;
    bool   malformedViolation = false;

    const auto visitViolation = [&]( const std::string& aCategory, const JSON& aViolation,
                                     const std::string& aSheetPath = std::string(),
                                     const std::string& aSheetUuidPath = std::string() )
    {
        if( !aViolation.is_object() || !aViolation.contains( "type" )
            || !aViolation["type"].is_string() || !aViolation.contains( "description" )
            || !aViolation["description"].is_string() || !aViolation.contains( "severity" )
            || !aViolation["severity"].is_string() || !aViolation.contains( "items" )
            || !aViolation["items"].is_array()
            || ( aViolation.contains( "excluded" ) && !aViolation["excluded"].is_boolean() )
            || ( aViolation.contains( "comment" ) && !aViolation["comment"].is_string() ) )
        {
            malformedViolation = true;
            return;
        }

        for( const JSON& item : aViolation["items"] )
        {
            if( !item.is_object() || !item.contains( "uuid" ) || !item["uuid"].is_string()
                || !item.contains( "description" ) || !item["description"].is_string()
                || !item.contains( "pos" ) || !item["pos"].is_object()
                || !item["pos"].contains( "x" ) || !item["pos"]["x"].is_number()
                || !item["pos"].contains( "y" ) || !item["pos"]["y"].is_number() )
            {
                malformedViolation = true;
                return;
            }
        }

        const std::string severity = aViolation["severity"].get<std::string>();

        if( severity != "error" && severity != "warning" && severity != "exclusion" )
        {
            malformedViolation = true;
            return;
        }

        const bool excluded = ( aViolation.contains( "excluded" )
                                && aViolation["excluded"].get<bool>() )
                              || severity == "exclusion";

        if( excluded )
            ++exclusions;
        else if( severity == "error" )
            ++errors;
        else if( severity == "warning" )
            ++warnings;
        else
            ++other;

        categoryCounts[aCategory] = categoryCounts[aCategory].get<size_t>() + 1;
        const size_t index = total++;

        if( index < static_cast<size_t>( offset )
            || violations.size() >= static_cast<size_t>( limit ) || resultByteLimited )
        {
            return;
        }

        JSON item = aViolation;
        item["category"] = aCategory;

        if( !aSheetPath.empty() )
        {
            item["sheetPath"] = aSheetPath;
            item["sheetUuidPath"] = aSheetUuidPath;
        }

        const size_t itemBytes = item.dump().size();

        if( resultBytes + itemBytes > MAX_VERIFICATION_RESULT_BYTES )
        {
            resultByteLimited = true;
            return;
        }

        resultBytes += itemBytes;
        violations.push_back( std::move( item ) );
    };

    if( operation == "erc" )
    {
        if( !report.contains( "sheets" ) || !report["sheets"].is_array() )
            return failure( "invalid_report", "native ERC report has no sheets array" );

        categoryCounts["erc"] = 0;

        for( const JSON& sheet : report["sheets"] )
        {
            if( !sheet.is_object() || !sheet.contains( "path" ) || !sheet["path"].is_string()
                || !sheet.contains( "uuid_path" ) || !sheet["uuid_path"].is_string()
                || !sheet.contains( "violations" ) || !sheet["violations"].is_array() )
            {
                return failure( "invalid_report", "native ERC sheet entry is malformed" );
            }

            for( const JSON& violation : sheet["violations"] )
            {
                visitViolation( "erc", violation, sheet["path"].get<std::string>(),
                                sheet["uuid_path"].get<std::string>() );
            }
        }
    }
    else
    {
        const std::array<std::pair<const char*, const char*>, 3> categories = {
            std::pair{ "violations", "drc" },
            std::pair{ "unconnected_items", "unconnectedItem" },
            std::pair{ "schematic_parity", "schematicParity" }
        };

        for( const auto& [field, category] : categories )
        {
            if( !report.contains( field ) || !report[field].is_array() )
                return failure( "invalid_report", "native DRC report category is malformed" );

            categoryCounts[category] = 0;

            for( const JSON& violation : report[field] )
                visitViolation( category, violation );
        }
    }

    if( malformedViolation )
        return failure( "invalid_report", "native verification violation is malformed" );

    if( resultByteLimited && violations.empty() && total > static_cast<size_t>( offset ) )
    {
        return failure( "result_too_large",
                        "one native verification violation exceeds the tool result limit" );
    }

    const size_t returned = violations.size();
    const bool hasMore = static_cast<size_t>( offset ) + returned < total;
    const bool ignoredChecksTruncated =
            ignoredChecks.size() < report["ignored_checks"].size();
    JSON payload = {
        { "operation", operation },
        { "path", relativePath },
        { "schema", expectedSchema },
        { "kicadVersion", nativeVersion },
        { "coordinateUnits", "mm" },
        { "clean", errors == 0 && warnings == 0 },
        { "waiversPresent", exclusions > 0 || !report["ignored_checks"].empty() },
        { "counts",
          { { "total", total },
            { "errors", errors },
            { "warnings", warnings },
            { "exclusions", exclusions },
            { "other", other },
            { "categories", std::move( categoryCounts ) } } },
        { "ignoredChecksCount", report["ignored_checks"].size() },
        { "ignoredChecks", std::move( ignoredChecks ) },
        { "ignoredChecksTruncated", ignoredChecksTruncated },
        { "offset", offset },
        { "returnedViolations", returned },
        { "violations", std::move( violations ) },
        { "resultTruncated", hasMore }
    };

    if( hasMore )
        payload["nextOffset"] = static_cast<size_t>( offset ) + returned;

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleFabricate(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        bool aFinalActionApproved ) const
{
    static const std::set<std::string> ALLOWED_ARGUMENTS = {
        "operation", "path", "boardPath", "schematicPath", "expectedSha256", "allowWaivers"
    };

    if( !aArguments.is_object() || aArguments.dump().size() > 32 * 1024 )
        return failure( "invalid_arguments", "fabricate arguments must be a bounded object" );

    for( auto argument = aArguments.begin(); argument != aArguments.end(); ++argument )
    {
        if( !ALLOWED_ARGUMENTS.contains( argument.key() ) )
            return failure( "invalid_arguments", "fabricate contains an unknown argument" );
    }

    for( const char* required :
         { "operation", "path", "boardPath", "schematicPath", "expectedSha256" } )
    {
        if( !aArguments.contains( required ) || !aArguments[required].is_string() )
        {
            return failure( "invalid_arguments",
                            std::string( "fabricate." ) + required + " must be a string" );
        }
    }

    if( aArguments.contains( "allowWaivers" ) && !aArguments["allowWaivers"].is_boolean() )
        return failure( "invalid_arguments", "fabricate.allowWaivers must be a boolean" );

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "plan" && operation != "export" )
        return failure( "invalid_arguments", "fabricate.operation must be 'plan' or 'export'" );

    const std::string expectedSha256 = aArguments["expectedSha256"].get<std::string>();

    if( expectedSha256.size() != 64
        || !std::all_of( expectedSha256.begin(), expectedSha256.end(),
                         []( unsigned char aCharacter )
                         {
                             return std::isdigit( aCharacter )
                                    || ( aCharacter >= 'a' && aCharacter <= 'f' );
                         } ) )
    {
        return failure( "invalid_arguments",
                        "fabricate.expectedSha256 must be one lowercase SHA-256 digest" );
    }

    if( !wxFileName::DirExists( aProjectPath ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    const std::string sourceRelativePath = aArguments["path"].get<std::string>();
    const std::string boardRelativePath = aArguments["boardPath"].get<std::string>();
    const std::string schematicRelativePath =
            aArguments["schematicPath"].get<std::string>();
    wxFileName sidecar;
    wxFileName board;
    wxFileName schematic;
    std::string pathError;

    if( !resolveProjectSidecar( aProjectPath, sourceRelativePath, sidecar, pathError ) )
        return failure( "invalid_path", pathError );

    if( !sidecar.FileExists() )
        return failure( "read_failed", "KiChad Design Script sidecar does not exist" );

    if( !resolveProjectFile( aProjectPath, boardRelativePath, board, pathError ) )
        return failure( "invalid_path", pathError );

    if( board.GetExt() != wxS( "kicad_pcb" ) )
        return failure( "invalid_path", "fabrication requires a .kicad_pcb board" );

    if( !resolveProjectFile( aProjectPath, schematicRelativePath, schematic, pathError ) )
        return failure( "invalid_path", pathError );

    if( schematic.GetExt() != wxS( "kicad_sch" ) )
        return failure( "invalid_path", "fabrication requires a root .kicad_sch schematic" );

    if( !validateExactNativeFormat( board, "kicad_pcb", "20260206", pathError )
        || !validateExactNativeFormat( schematic, "kicad_sch", "20260306", pathError ) )
    {
        return failure( "format_version_mismatch", pathError );
    }

    std::string source;

    if( !readDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( !compiled.ok )
    {
        return failure( "compile_failed",
                        "KDS must compile before fabrication; use design.compile for "
                        "structured diagnostics" );
    }

    if( compiled.sourceSha256 != expectedSha256 )
        return failure( "stale_source", "KDS changed after the approved revision was compiled" );

    const std::string fileStem( board.GetName().ToUTF8() );
    const std::string schematicStem( schematic.GetName().ToUTF8() );
    const std::string projectName = compiled.ir["project"].value( "name", "" );

    if( fileStem.empty() || fileStem != schematicStem || fileStem != projectName )
    {
        return failure( "project_mismatch",
                        "KDS project, root schematic, and board must have the same file stem" );
    }

    JSON plan = buildFabricationPlan( compiled.ir, fileStem );
    plan["operation"] = "plan";
    plan["path"] = sourceRelativePath;
    plan["boardPath"] = boardRelativePath;
    plan["schematicPath"] = schematicRelativePath;
    plan["sourceSha256"] = compiled.sourceSha256;
    plan["kicadVersion"] = std::string( GetMajorMinorPatchVersion().ToUTF8() );
    plan["nativeInputFormats"] = { { "board", "20260206" },
                                    { "schematic", "20260306" } };
    std::string boardIntentError;
    const bool boardIntentValid =
            validateBoardFabricationIntent( board, compiled.ir, plan, boardIntentError );
    plan["nativeBoardIntentValid"] = boardIntentValid;

    if( !boardIntentValid )
    {
        plan["productionReady"] = false;
        plan["issues"].push_back( { { "type", "native_board_intent_mismatch" },
                                    { "severity", "error" },
                                    { "description", boardIntentError } } );
    }

    if( operation == "plan" )
        return success( plan );

    if( !boardIntentValid )
        return failure( "native_board_intent_mismatch", boardIntentError );

    if( !plan["productionReady"].get<bool>() )
        return failure( "incomplete_fabrication_intent",
                        "KDS is missing one or more production checks, outputs, or stackup" );

    if( !aMutationAvailable )
    {
        return failure( "snapshot_required",
                        "A complete pre-turn project snapshot is required for fabrication" );
    }

    if( !aFinalActionApproved )
    {
        return failure( "permission_required",
                        "Visible user confirmation is required for final fabrication export" );
    }

    const bool allowWaivers = aArguments.value( "allowWaivers", false );
    std::string boardSha256;
    std::string schematicSha256;

    if( !hashFabricationFile( board.GetFullPath().ToStdString(), boardSha256 )
        || !hashFabricationFile( schematic.GetFullPath().ToStdString(), schematicSha256 ) )
    {
        return failure( "read_failed", "could not hash the complete native design inputs" );
    }

    wxFileName projectRoot = wxFileName::DirName( aProjectPath );
    projectRoot.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( projectRoot, true ) )
        return failure( "project_unavailable", "active project directory could not be resolved" );

    PRIVATE_TEMPORARY_DIRECTORY verificationSnapshot;

    if( !createFabricationVerificationSnapshot(
                projectRoot, board, schematic, sidecar,
                { sourceRelativePath, boardRelativePath, schematicRelativePath },
                verificationSnapshot, pathError ) )
    {
        return failure( "verification_snapshot_failed", pathError );
    }

    const wxString verificationProjectPath =
            wxString::FromUTF8( verificationSnapshot.Path().string().c_str() );
    wxFileName verificationBoard;

    if( !resolveProjectFile( verificationProjectPath, boardRelativePath,
                             verificationBoard, pathError ) )
    {
        return failure( "verification_snapshot_failed",
                        "private board snapshot could not be resolved: " + pathError );
    }

    const auto decodeResult = [&]( const JSON& aResult, JSON& aData,
                                   std::string& aError )
    {
        if( !aResult.is_object() || !aResult.value( "success", false )
            || !aResult.contains( "contentItems" ) || !aResult["contentItems"].is_array()
            || aResult["contentItems"].empty()
            || !aResult["contentItems"][0].contains( "text" )
            || !aResult["contentItems"][0]["text"].is_string() )
        {
            if( aResult.is_object() && aResult.contains( "contentItems" )
                && aResult["contentItems"].is_array() && !aResult["contentItems"].empty()
                && aResult["contentItems"][0].contains( "text" )
                && aResult["contentItems"][0]["text"].is_string() )
            {
                JSON envelope = JSON::parse(
                        aResult["contentItems"][0]["text"].get<std::string>(), nullptr, false );

                if( envelope.is_object() && envelope.contains( "error" ) )
                    aError = envelope["error"].value( "message", "verification failed" );
            }

            if( aError.empty() )
                aError = "verification tool returned an invalid failure envelope";

            return false;
        }

        JSON envelope = JSON::parse(
                aResult["contentItems"][0]["text"].get<std::string>(), nullptr, false );

        if( !envelope.is_object() || !envelope.value( "ok", false )
            || !envelope.contains( "data" ) || !envelope["data"].is_object() )
        {
            aError = "verification tool returned an invalid success envelope";
            return false;
        }

        aData = std::move( envelope["data"] );
        return true;
    };
    JSON checks = JSON::object();

    for( const auto& [kind, path] :
        std::array<std::pair<const char*, std::string>, 3>{
                 std::pair{ "erc", schematicRelativePath },
                 std::pair{ "drc", boardRelativePath },
                 std::pair{ "sourcing", sourceRelativePath } } )
    {
        JSON result = handleVerify( { { "operation", kind }, { "path", path } },
                                    verificationProjectPath );
        JSON data;
        std::string checkError;

        if( !decodeResult( result, data, checkError ) )
            return failure( std::string( kind ) + "_check_failed", checkError );

        if( kind == std::string_view( "sourcing" )
            && data.value( "sourceSha256", "" ) != compiled.sourceSha256 )
        {
            return failure( "stale_source", "sourcing gate checked a different KDS revision" );
        }

        const bool clean = data.value( "clean", false );
        const bool waivers = data.value( "waiversPresent", false );

        if( !clean )
        {
            const JSON counts = data.value( "counts", JSON::object() );
            return failure( std::string( kind ) + "_gate_failed",
                            std::string( kind ) + " gate is not clean ("
                                    + std::to_string( counts.value( "errors", 0 ) )
                                    + " errors, "
                                    + std::to_string( counts.value( "warnings", 0 ) )
                                    + " warnings)" );
        }

        if( waivers && !allowWaivers )
        {
            return failure( "waiver_confirmation_required",
                            std::string( kind )
                                    + " has exclusions or ignored checks; set allowWaivers "
                                      "only with explicit release approval" );
        }

        checks[kind] = { { "clean", true },
                         { "waiversPresent", waivers },
                         { "counts", data.value( "counts", JSON::object() ) } };

        if( data.contains( "schema" ) )
            checks[kind]["schema"] = data["schema"];

        if( data.contains( "kicadVersion" ) )
            checks[kind]["kicadVersion"] = data["kicadVersion"];
    }

    const auto inputsUnchanged = [&]()
    {
        std::string currentBoardHash;
        std::string currentSchematicHash;
        std::string currentSource;

        return hashFabricationFile( board.GetFullPath().ToStdString(), currentBoardHash )
               && hashFabricationFile( schematic.GetFullPath().ToStdString(),
                                       currentSchematicHash )
               && currentBoardHash == boardSha256 && currentSchematicHash == schematicSha256
               && readDesignScriptSidecar( sidecar, currentSource, pathError )
               && currentSource == source;
    };

    if( !inputsUnchanged() )
    {
        return failure( "stale_inputs",
                        "board, schematic, or KDS changed while release gates were running" );
    }

    const std::filesystem::path root( projectRoot.GetFullPath().ToStdString() );
    const std::string transactionId( KIID().AsString().ToUTF8() );
    const std::filesystem::path staging =
            root / ( ".kichad-fabrication-staging-" + transactionId );
    const std::filesystem::path target = root / "fabrication";
    const std::filesystem::path backup =
            root / ( ".kichad-fabrication-backup-" + transactionId );
    std::error_code filesystemError;
    std::filesystem::create_directory( staging, filesystemError );

    if( filesystemError )
        return failure( "staging_failed", "could not create private fabrication staging" );

    std::filesystem::permissions( staging, std::filesystem::perms::owner_all,
                                  std::filesystem::perm_options::replace, filesystemError );

    if( filesystemError )
    {
        std::error_code cleanupError;
        std::filesystem::remove_all( staging, cleanupError );
        return failure( "staging_failed", "could not secure private fabrication staging" );
    }

    const auto cleanupStaging = [&]()
    {
        std::error_code cleanupError;
        std::filesystem::remove_all( staging, cleanupError );
    };
    wxFileName stagingDirectory =
            wxFileName::DirName( wxString::FromUTF8( staging.string().c_str() ) );
    std::string exportError;
    const bool exported = m_nativeFabricationRunner
                                  ? m_nativeFabricationRunner( verificationBoard, plan,
                                                               stagingDirectory, exportError )
                                  : runNativeKiCadFabrication( verificationBoard, plan,
                                                               stagingDirectory, exportError );

    if( !exported )
    {
        cleanupStaging();

        if( exportError.empty() )
            exportError = "native fabrication backend failed without an error message";

        return failure( "export_failed", exportError );
    }

    size_t bomRows = 0;

    if( !writeFabricationBom( compiled.ir, stagingDirectory, plan, bomRows, exportError ) )
    {
        cleanupStaging();
        return failure( "bom_failed", exportError );
    }

    JSON artifacts;

    if( !validateFabricationArtifacts( stagingDirectory, plan, artifacts, exportError ) )
    {
        cleanupStaging();
        return failure( "artifact_validation_failed", exportError );
    }

    if( !inputsUnchanged() )
    {
        cleanupStaging();
        return failure( "stale_inputs",
                        "board, schematic, or KDS changed during fabrication export" );
    }

    const bool waiversPresent = checks["erc"].value( "waiversPresent", false )
                                || checks["drc"].value( "waiversPresent", false );
    JSON manifest = {
        { "schema", "kichad.fabrication-manifest.v1" },
        { "manifestVersion", 1 },
        { "profile", plan["profile"] },
        { "kicadVersion", std::string( GetMajorMinorPatchVersion().ToUTF8() ) },
        { "releaseStatus", waiversPresent ? "waived" : "clean" },
        { "waiversAllowed", allowWaivers },
        { "source",
          { { "kds", { { "path", sourceRelativePath },
                          { "sha256", compiled.sourceSha256 } } },
            { "board", { { "path", boardRelativePath }, { "sha256", boardSha256 },
                           { "formatVersion", "20260206" } } },
            { "schematic", { { "path", schematicRelativePath },
                               { "sha256", schematicSha256 },
                               { "formatVersion", "20260306" } } } } },
        { "checks", checks },
        { "bomRows", bomRows },
        { "artifacts", artifacts }
    };
    wxFileName manifestPath( stagingDirectory.GetFullPath(), wxS( "manifest.json" ) );

    if( !writeJsonAtomically( manifestPath, manifest, exportError ) )
    {
        cleanupStaging();
        return failure( "manifest_failed", exportError );
    }

    std::string manifestSha256;

    if( !hashFabricationFile( manifestPath.GetFullPath().ToStdString(), manifestSha256 ) )
    {
        cleanupStaging();
        return failure( "manifest_failed", "could not hash the fabrication manifest" );
    }

    const bool targetExists = std::filesystem::exists( target, filesystemError );

    if( filesystemError )
    {
        cleanupStaging();
        return failure( "install_failed", "could not inspect existing fabrication output" );
    }

    if( targetExists )
    {
        const std::filesystem::file_status targetStatus =
                std::filesystem::symlink_status( target, filesystemError );

        if( filesystemError || std::filesystem::is_symlink( targetStatus )
            || !std::filesystem::is_directory( targetStatus ) )
        {
            cleanupStaging();
            return failure( "install_failed",
                            "existing fabrication target must be a real project directory" );
        }

        std::filesystem::rename( target, backup, filesystemError );

        if( filesystemError )
        {
            cleanupStaging();
            return failure( "install_failed", "could not stage existing fabrication output" );
        }
    }

    filesystemError.clear();
    std::filesystem::rename( staging, target, filesystemError );

    if( filesystemError )
    {
        std::error_code restoreError;

        if( targetExists )
            std::filesystem::rename( backup, target, restoreError );

        cleanupStaging();
        return failure( "install_failed",
                        restoreError ? "fabrication installation and rollback both failed"
                                     : "could not atomically install fabrication output" );
    }

    std::string installedManifestSha256;

    if( !hashFabricationFile( target / "manifest.json", installedManifestSha256 )
        || installedManifestSha256 != manifestSha256 )
    {
        std::error_code rollbackError;
        std::filesystem::remove_all( target, rollbackError );

        if( targetExists && !rollbackError )
            std::filesystem::rename( backup, target, rollbackError );

        return failure( "install_failed",
                        rollbackError ? "installed manifest verification and rollback failed"
                                      : "installed manifest verification failed; output restored" );
    }

    bool backupRetained = false;

    if( targetExists )
    {
        filesystemError.clear();
        std::filesystem::remove_all( backup, filesystemError );
        backupRetained = static_cast<bool>( filesystemError );
    }

    uintmax_t artifactBytes = 0;

    for( const JSON& artifact : artifacts )
        artifactBytes += artifact.at( "bytes" ).get<uintmax_t>();

    return success( { { "operation", "export" },
                      { "path", sourceRelativePath },
                      { "boardPath", boardRelativePath },
                      { "schematicPath", schematicRelativePath },
                      { "sourceSha256", compiled.sourceSha256 },
                      { "profile", plan["profile"] },
                      { "targetDirectory", "fabrication" },
                      { "releaseStatus", waiversPresent ? "waived" : "clean" },
                      { "checks", std::move( checks ) },
                      { "artifactCount", artifacts.size() },
                      { "artifactBytes", artifactBytes },
                      { "bomRows", bomRows },
                      { "manifestSha256", manifestSha256 },
                      { "transaction", "confirmed staged validation and atomic replacement" },
                      { "backupRetained", backupRetained } } );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleProject(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string()
        || aArguments["operation"].get<std::string>() != "context" )
    {
        return failure( "invalid_arguments", "project.operation must be 'context'" );
    }

    wxString path = aProjectPath;

    if( !wxFileName::DirExists( path ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    JSON files = JSON::array();
    wxDir directory( path );
    wxString name;
    bool found = directory.GetFirst( &name, wxS( "*.kicad_*" ), wxDIR_FILES );

    while( found )
    {
        wxFileName file( path, name );
        wxULongLong size = file.GetSize();
        JSON item = { { "name", std::string( name.ToUTF8() ) } };

        if( size != wxInvalidSize )
            item["bytes"] = size.GetValue();

        files.emplace_back( std::move( item ) );
        found = directory.GetNext( &name );
    }

    JSON payload = {
        { "operation", "context" },
        { "projectPath", std::string( path.ToUTF8() ) },
        { "kicadVersion", std::string( GetBuildVersion().ToUTF8() ) },
        { "files", std::move( files ) },
        { "mutationAvailable", aMutationAvailable }
    };

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::success( const JSON& aPayload ) const
{
    JSON envelope = { { "ok", true }, { "data", aPayload } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", true } };
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::failure( const std::string& aCode,
                                                        const std::string& aMessage ) const
{
    JSON envelope = { { "ok", false },
                      { "error", { { "code", aCode }, { "message", aMessage } } } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", false } };
}


wxString CODEX_TOOL_REGISTRY::projectPath() const
{
    wxString path = m_projectPathProvider ? m_projectPathProvider() : wxString();

    return path;
}
