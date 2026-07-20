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

#include <boost/test/unit_test.hpp>

#include <kicad/codex/design_script_compiler.h>
#include <kicad/codex/design_script_footprint_library_generator.h>
#include <kicad/codex/managed_footprint_library_io.h>

#include <kiid.h>

#include <filesystem>

#include <wx/file.h>
#include <wx/utils.h>


namespace
{

class TEMPORARY_PROJECT
{
public:
    TEMPORARY_PROJECT()
    {
        m_root = wxFileName::DirName( wxFileName::GetTempDir() );
        m_root.AppendDir( wxS( "kichad-managed-footprint-qa-" ) + KIID().AsString() );
        BOOST_REQUIRE( wxFileName::Mkdir( m_root.GetFullPath(), 0700 ) );
        m_libraries = m_root;
        m_libraries.AppendDir( wxS( "libraries" ) );
        BOOST_REQUIRE( wxFileName::Mkdir( m_libraries.GetFullPath(), 0700 ) );
        m_project.Assign( m_root.GetFullPath(), wxS( "Project.kicad_pro" ) );
    }

    ~TEMPORARY_PROJECT()
    {
        std::error_code error;
        const std::string utf8( m_root.GetFullPath().ToUTF8() );
        const std::u8string path( reinterpret_cast<const char8_t*>( utf8.data() ), utf8.size() );
        std::filesystem::remove_all( std::filesystem::path( path ), error );
    }

    const wxFileName& Project() const { return m_project; }
    wxString Root() const { return m_root.GetFullPath(); }
    const wxFileName& Libraries() const { return m_libraries; }

private:
    wxFileName m_root;
    wxFileName m_libraries;
    wxFileName m_project;
};


class BUILD_CLI_ENVIRONMENT
{
public:
    BUILD_CLI_ENVIRONMENT()
    {
        m_hadPrevious = wxGetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), &m_previous );
        wxSetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), wxS( "1" ) );
    }

    ~BUILD_CLI_ENVIRONMENT()
    {
        if( m_hadPrevious )
            wxSetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), m_previous );
        else
            wxUnsetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ) );
    }

private:
    bool m_hadPrevious = false;
    wxString m_previous;
};

} // namespace


BOOST_AUTO_TEST_SUITE( ManagedFootprintLibraryIo )


BOOST_AUTO_TEST_CASE( AtomicallyInstallsAndNativeLoadsGeneratedCurrentFormat )
{
    TEMPORARY_PROJECT project;
    BUILD_CLI_ENVIRONMENT environment;
    const std::string program = R"KDS((kichad_design
  (version 1) (project Project)
  (library footprint Product (table project)
    (uri "${KIPRJMOD}/libraries/Product.pretty") (managed true))
  (footprint Product:SENSOR_2P
    (reference U) (value SENSOR_2P) (description "Two-pad sensor")
    (attributes (smd true) (allow_missing_courtyard true))
    (net_tie_group 1 2)
    (pad p1 (number 1) (type smd) (shape roundrect) (at -0.8mm 0mm)
      (rotation 12.5deg) (size 0.8mm 0.8mm) (layers F.Cu F.Mask F.Paste)
      (roundrect_radius 0.2mm) (pin_function INPUT) (pin_type signal)
      (solder_mask_margin 0.02mm) (solder_paste_margin_ratio -0.1)
      (zone_connection thermal) (thermal_spoke_width 0.2mm)
      (thermal_spoke_angle 45deg) (thermal_gap 0.15mm))
    (pad p2 (number 2) (type smd) (shape rect) (at 0.8mm 0mm)
      (size 0.8mm 0.8mm) (layers F.Cu F.Mask F.Paste)
      (shape_offset 0.05mm 0mm))
    (pad p3 (number 3) (type smd) (shape trapezoid) (at 0mm 1.5mm)
      (size 1mm 0.8mm) (layers F.Cu F.Mask)
      (trapezoid_delta 0.8mm 0mm))
    (model "${KIPRJMOD}/models/SENSOR_2P.step" (visible false) (opacity 0.75)
      (offset 0mm 0mm 0.1mm) (scale 1 1 1) (rotation 0deg 0deg 90deg)))
  (footprint Product:HEADER_1X01
    (reference J) (value HEADER_1X01) (attributes (through_hole true))
    (pad p1 (number 1) (type thru_hole) (shape circle) (at 0mm 0mm)
      (size 2mm 2mm) (layers all_copper all_mask) (drill round 1mm)
      (remove_unused_layers true) (keep_end_layers true))
    (pad h1 (number "") (type np_thru_hole) (shape circle) (at 3mm 0mm)
      (size 2mm 2mm) (layers all_copper all_mask) (drill round 2mm)))
))KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::RESULT generated =
            KICHAD::DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( generated.ok, generated.diagnostics.dump( 2 ) );

    wxFileName resolved;
    std::string relative;
    std::string error;
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::Resolve(
                                   project.Root(), "${KIPRJMOD}/libraries/Product.pretty",
                                   resolved, relative, error ), error );
    BOOST_CHECK_EQUAL( relative, "libraries/Product.pretty" );
    BOOST_CHECK( !resolved.DirExists() );

    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES desired;

    for( const auto& [id, source] : generated.sources.items() )
    {
        const size_t separator = id.find( ':' );
        BOOST_REQUIRE_NE( separator, std::string::npos );
        desired.emplace( id.substr( separator + 1 ) + ".kicad_mod",
                         source.get<std::string>() );
    }

    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                                   resolved, true, desired, error ), error );
    bool present = false;
    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES readBack;
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ReadOptional(
                                   resolved, present, readBack, error ), error );
    BOOST_CHECK( present );
    BOOST_CHECK( readBack == desired );
    BOOST_CHECK_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ValidateNative(
                                 resolved, error ), error );

    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES replacement;
    replacement.emplace( *desired.begin() );
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                                   resolved, true, replacement, error ), error );
    readBack.clear();
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ReadOptional(
                                   resolved, present, readBack, error ), error );
    BOOST_CHECK( readBack == replacement );
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                                   resolved, true, desired, error ), error );
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
                                   resolved, false, {}, error ), error );
    BOOST_CHECK( !resolved.DirExists() );
}


BOOST_AUTO_TEST_CASE( RejectsTraversalUnownedEntriesSymlinksAndNativeGarbage )
{
    TEMPORARY_PROJECT project;
    BUILD_CLI_ENVIRONMENT environment;
    wxFileName resolved;
    std::string relative;
    std::string error;
    BOOST_CHECK( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::Resolve(
            project.Root(), "${KIPRJMOD}/../escape.pretty", resolved, relative, error ) );
    error.clear();
    BOOST_CHECK( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::Resolve(
            project.Root(), "${KIPRJMOD}/missing/Bad.pretty", resolved, relative, error ) );

    error.clear();
    BOOST_REQUIRE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::Resolve(
            project.Root(), "${KIPRJMOD}/libraries/Bad.pretty",
            resolved, relative, error ) );
    BOOST_REQUIRE( wxFileName::Mkdir( resolved.GetFullPath(), 0700 ) );
    wxFile note( resolved.GetFullPath() + wxFileName::GetPathSeparator() + wxS( "notes.txt" ),
                 wxFile::write );
    BOOST_REQUIRE( note.IsOpened() );
    BOOST_REQUIRE_EQUAL( note.Write( "unowned", 7 ), 7 );
    note.Close();
    bool present = false;
    KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::FILES files;
    BOOST_CHECK( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ReadOptional(
            resolved, present, files, error ) );

    std::error_code cleanupError;
    std::filesystem::remove_all( std::filesystem::path( resolved.GetFullPath().ToStdString() ),
                                 cleanupError );
    BOOST_REQUIRE_MESSAGE( !cleanupError, cleanupError.message() );
    BOOST_REQUIRE( project.Libraries().DirExists() );
    BOOST_REQUIRE_NE( resolved.GetFullPath(), project.Libraries().GetFullPath() );
    const wxFileName symlinkPath( project.Libraries().GetFullPath(), wxS( "Bad.pretty" ) );
    cleanupError.clear();
    std::filesystem::create_directory_symlink(
            std::filesystem::path( project.Libraries().GetFullPath().ToStdString() ),
            std::filesystem::path( symlinkPath.GetFullPath().ToStdString() ), cleanupError );
    BOOST_REQUIRE_MESSAGE( !cleanupError, cleanupError.message() );
    error.clear();
    BOOST_CHECK( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::Resolve(
            project.Root(), "${KIPRJMOD}/libraries/Bad.pretty",
            resolved, relative, error ) );
    cleanupError.clear();
    std::filesystem::remove( std::filesystem::path( symlinkPath.GetFullPath().ToStdString() ),
                             cleanupError );
    BOOST_REQUIRE_MESSAGE( !cleanupError, cleanupError.message() );
    error.clear();
    files = { { "Bad.kicad_mod", "not a footprint\n" } };
    BOOST_REQUIRE( KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::InstallAtomically(
            resolved, true, files, error ) );
    error.clear();
    BOOST_CHECK( !KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO::ValidateNative( resolved, error ) );
    BOOST_CHECK_NE( error.find( "rejected" ), std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
