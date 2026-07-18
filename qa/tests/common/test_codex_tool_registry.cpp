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

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <kicad/codex/codex_tool_registry.h>

#include <kiid.h>
#include <filesystem>
#include <wx/ffile.h>
#include <wx/filename.h>


namespace
{

using JSON = nlohmann::json;


JSON envelope( const JSON& aResult )
{
    return JSON::parse( aResult.at( "contentItems" ).at( 0 ).at( "text" ).get<std::string>() );
}


class TOOL_PROJECT_FIXTURE
{
public:
    TOOL_PROJECT_FIXTURE()
    {
        wxFileName root = wxFileName::DirName( wxFileName::GetTempDir() );
        root.AppendDir( wxS( "kichad-codex-tools-" ) + KIID().AsString() );
        m_root = root.GetFullPath();
        BOOST_REQUIRE( wxFileName::Mkdir( m_root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

        write( wxS( "design.kicad_pro" ), wxS( "{}\n" ) );
        write( wxS( "design.kicad_pcb" ),
               wxS( "(kicad_pcb (version 20250524)\n"
                    "  (general (thickness 1.6))\n"
                    "  (footprint \"Package_SO:SOIC-8_3.9x4.9mm_P1.27mm\"\n"
                    "    (property \"Reference\" \"U1\")\n"
                    "    (unknown_extension keep-me))\n"
                    "  (segment (start 1 2) (end 3 4) (width 0.25))\n"
                    ")\n" ) );
        write( wxS( "empty.kicad_pcb" ), wxString() );
        write( wxS( "wrong-root.kicad_pcb" ), wxS( "(kicad_sch)\n" ) );

        wxFileName outsideRoot = wxFileName::DirName( m_root );
        wxString   rootName = outsideRoot.GetDirs().Last();
        outsideRoot.RemoveLastDir();
        m_outside = wxFileName( outsideRoot.GetFullPath(),
                                rootName + wxS( "-outside.kicad_pcb" ) )
                            .GetFullPath();
        wxFFile outsideFile( m_outside, wxS( "wb" ) );
        BOOST_REQUIRE( outsideFile.IsOpened() );
        BOOST_REQUIRE( outsideFile.Write( wxS( "(kicad_pcb)\n" ) ) );
    }

    ~TOOL_PROJECT_FIXTURE()
    {
        wxRemoveFile( m_outside );
        wxFileName::Rmdir( m_root, wxPATH_RMDIR_RECURSIVE );
    }

    const wxString& Root() const { return m_root; }

    wxString OutsideRelativePath() const
    {
        return wxS( "../" ) + wxFileName( m_outside ).GetFullName();
    }

    const wxString& OutsidePath() const { return m_outside; }

private:
    void write( const wxString& aName, const wxString& aContent )
    {
        wxFFile file( wxFileName( m_root, aName ).GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE( file.Write( aContent ) );
    }

    wxString m_root;
    wxString m_outside;
};

} // namespace


BOOST_AUTO_TEST_SUITE( CodexToolRegistry )


BOOST_AUTO_TEST_CASE( AdvertisesOnlyImplementedNativeTools )
{
    CODEX_TOOL_REGISTRY registry( []() { return wxString(); } );
    JSON                specs = registry.Specs();

    BOOST_REQUIRE_EQUAL( specs.size(), 2 );
    BOOST_CHECK_EQUAL( specs[0]["name"].get<std::string>(), "project" );
    BOOST_CHECK_EQUAL( specs[1]["name"].get<std::string>(), "inspect" );
}


BOOST_AUTO_TEST_CASE( ReportsProjectAndSnapshotState )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; } );

    JSON result = registry.Handle( "project", { { "operation", "context" } } );
    BOOST_REQUIRE( result.at( "success" ).get<bool>() );

    JSON data = envelope( result ).at( "data" );
    BOOST_CHECK_EQUAL( data.at( "projectPath" ).get<std::string>(),
                       std::string( fixture.Root().ToUTF8() ) );
    BOOST_CHECK( data.at( "mutationAvailable" ).get<bool>() );
    BOOST_CHECK_GE( data.at( "files" ).size(), 2 );
}


BOOST_AUTO_TEST_CASE( SummarizesAndFindsBoundedExpressions )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON summary = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "design.kicad_pcb" } } );
    BOOST_REQUIRE_MESSAGE( summary.at( "success" ).get<bool>(), summary.dump() );
    JSON summaryData = envelope( summary ).at( "data" );
    BOOST_CHECK_EQUAL( summaryData.at( "rootHead" ).get<std::string>(), "kicad_pcb" );
    BOOST_CHECK_GT( summaryData.at( "nodes" ).get<size_t>(), 0 );

    JSON found = registry.Handle( "inspect", { { "operation", "find" },
                                                { "path", "design.kicad_pcb" },
                                                { "head", "footprint" }, { "limit", 1 } } );
    BOOST_REQUIRE_MESSAGE( found.at( "success" ).get<bool>(), found.dump() );
    JSON foundData = envelope( found ).at( "data" );
    BOOST_CHECK_EQUAL( foundData.at( "totalMatches" ).get<size_t>(), 1 );
    BOOST_REQUIRE_EQUAL( foundData.at( "expressions" ).size(), 1 );
    BOOST_CHECK_NE( foundData["expressions"][0]["text"].get<std::string>().find( "keep-me" ),
                    std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsPathsOutsideTheProject )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON escaped = registry.Handle(
            "inspect", { { "operation", "summary" },
                         { "path", std::string( fixture.OutsideRelativePath().ToUTF8() ) } } );
    BOOST_REQUIRE_MESSAGE( !escaped.at( "success" ).get<bool>(), escaped.dump() );
    BOOST_CHECK_EQUAL( envelope( escaped )["error"]["code"].get<std::string>(), "invalid_path" );

    JSON absolute = registry.Handle(
            "inspect", { { "operation", "summary" },
                         { "path", std::string( wxFileName( fixture.Root(),
                                                             wxS( "design.kicad_pcb" ) )
                                                        .GetFullPath()
                                                        .ToUTF8() ) } } );
    BOOST_CHECK( !absolute.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( absolute )["error"]["code"].get<std::string>(), "invalid_path" );

#ifndef __WXMSW__
    wxFileName link( fixture.Root(), wxS( "linked.kicad_pcb" ) );
    std::error_code linkError;
    std::filesystem::create_symlink( std::filesystem::path( fixture.OutsidePath().ToStdString() ),
                                     std::filesystem::path( link.GetFullPath().ToStdString() ),
                                     linkError );
    BOOST_REQUIRE_MESSAGE( !linkError, linkError.message() );

    JSON linked = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "linked.kicad_pcb" } } );
    BOOST_REQUIRE_MESSAGE( !linked.at( "success" ).get<bool>(), linked.dump() );
    BOOST_CHECK_EQUAL( envelope( linked )["error"]["code"].get<std::string>(), "invalid_path" );
    std::filesystem::remove( std::filesystem::path( link.GetFullPath().ToStdString() ) );
#endif
}


BOOST_AUTO_TEST_CASE( RejectsMalformedArgumentsAndDocuments )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON result;
    BOOST_REQUIRE_NO_THROW( result = registry.Handle(
                                    "inspect", { { "operation", 7 },
                                                 { "path", "design.kicad_pcb" } } ) );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );

    BOOST_REQUIRE_NO_THROW( result = registry.Handle(
                                    "inspect", { { "operation", "find" },
                                                 { "path", "design.kicad_pcb" },
                                                 { "head", JSON::array() } } ) );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );

    result = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "empty.kicad_pcb" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(), "parse_failed" );

    result = registry.Handle(
            "inspect", { { "operation", "summary" }, { "path", "wrong-root.kicad_pcb" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "format_mismatch" );
}


BOOST_AUTO_TEST_SUITE_END()
