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
#include <kicad/codex/design_script_pcb_planner.h>

#include <atomic>
#include <import_export.h>
#include <api/board/board_commands.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <google/protobuf/empty.pb.h>
#include <kiid.h>
#include <kinng.h>
#include <filesystem>
#include <map>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>


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

    BOOST_REQUIRE_EQUAL( specs.size(), 4 );
    BOOST_CHECK_EQUAL( specs[0]["name"].get<std::string>(), "project" );
    BOOST_CHECK_EQUAL( specs[1]["name"].get<std::string>(), "inspect" );
    BOOST_CHECK_EQUAL( specs[2]["name"].get<std::string>(), "design" );
    BOOST_CHECK_EQUAL( specs[3]["name"].get<std::string>(), "pcb" );
    const JSON& designOperations =
            specs[2]["inputSchema"]["properties"]["operation"]["enum"];
    BOOST_REQUIRE_EQUAL( designOperations.size(), 6 );
    BOOST_CHECK_EQUAL( designOperations[1].get<std::string>(), "read" );
    BOOST_CHECK_EQUAL( designOperations[3].get<std::string>(), "preview" );
    BOOST_CHECK_EQUAL( designOperations[5].get<std::string>(), "apply" );
}


BOOST_AUTO_TEST_CASE( DescribesExactPcbProtobufJsonFieldsWithoutAnEditor )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); } );

    JSON root = registry.Handle( "pcb", { { "operation", "describe" },
                                           { "path", "design.kicad_pcb" },
                                           { "itemType", "footprint" } } );
    BOOST_REQUIRE_MESSAGE( root.at( "success" ).get<bool>(), root.dump() );
    JSON rootSchema = envelope( root )["data"]["schema"];
    BOOST_CHECK_EQUAL( rootSchema["messageType"].get<std::string>(),
                       "kiapi.board.types.FootprintInstance" );

    JSON nested = registry.Handle( "pcb", { { "operation", "describe" },
                                             { "path", "design.kicad_pcb" },
                                             { "itemType", "trace" },
                                             { "messagePath", "start" } } );
    BOOST_REQUIRE_MESSAGE( nested.at( "success" ).get<bool>(), nested.dump() );
    JSON nestedSchema = envelope( nested )["data"]["schema"];
    BOOST_CHECK_EQUAL( nestedSchema["messageType"].get<std::string>(),
                       "kiapi.common.types.Vector2" );
    BOOST_CHECK_EQUAL( nestedSchema["fields"][0]["name"].get<std::string>(), "xNm" );
    BOOST_CHECK_EQUAL( nestedSchema["fields"][0]["jsonEncoding"].get<std::string>(),
                       "decimal string" );
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


BOOST_AUTO_TEST_CASE( CompilesAndAtomicallySavesReusableDesignSidecars )
{
    TOOL_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project reusable)
  (component R1
    (symbol "Device:R")
    (value "10k")
    (footprint "Resistor_SMD:R_0603_1608Metric"))
  (component LED1
    (symbol "Device:LED")
    (value "GREEN")
    (footprint "LED_SMD:LED_0603_1608Metric"))
  (net LED_A (pin R1 1) (pin LED1 1))
  (board
    (outline (rect (id board-edge) (at 0mm 0mm) (size 40mm 30mm)))
    (route LED_A (id led-a-trace) (from 10mm 10mm) (to 20mm 10mm)
      (width 0.25mm) (layer F.Cu)))
  (check erc)
  (check drc)
  (output gerbers))
)KDS";

    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                  { "source", source } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_CHECK( compileData["valid"].get<bool>() );
    BOOST_CHECK( !compileData.contains( "ir" ) );
    const std::string firstHash = compileData["sourceSha256"].get<std::string>();

    CODEX_TOOL_REGISTRY readOnlyRegistry( [&fixture]() { return fixture.Root(); } );
    JSON firstPreview = readOnlyRegistry.Handle(
            "design", { { "operation", "preview" }, { "source", source } } );
    JSON secondPreview = readOnlyRegistry.Handle(
            "design", { { "operation", "preview" }, { "source", source } } );
    BOOST_REQUIRE_MESSAGE( firstPreview.at( "success" ).get<bool>(), firstPreview.dump() );
    BOOST_REQUIRE_MESSAGE( secondPreview.at( "success" ).get<bool>(), secondPreview.dump() );
    JSON firstBoardPlan = envelope( firstPreview )["data"]["boardPlan"];
    JSON secondBoardPlan = envelope( secondPreview )["data"]["boardPlan"];
    BOOST_CHECK( firstBoardPlan["fullyLowered"].get<bool>() );
    BOOST_CHECK( !firstBoardPlan["itemsTruncated"].get<bool>() );
    BOOST_REQUIRE_EQUAL( firstBoardPlan["items"].size(), 2 );
    BOOST_CHECK_EQUAL( firstBoardPlan["items"][0]["targetId"].get<std::string>(),
                       secondBoardPlan["items"][0]["targetId"].get<std::string>() );
    BOOST_CHECK_EQUAL( firstBoardPlan["items"][1]["targetId"].get<std::string>(),
                       secondBoardPlan["items"][1]["targetId"].get<std::string>() );

    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "reusable.kicad_kds" },
                                               { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    BOOST_CHECK_EQUAL( envelope( saved )["data"]["sourceSha256"].get<std::string>(),
                       firstHash );

    JSON read = registry.Handle( "design", { { "operation", "read" },
                                               { "path", "reusable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( read.at( "success" ).get<bool>(), read.dump() );
    JSON readData = envelope( read )["data"];
    BOOST_CHECK_EQUAL( readData["source"].get<std::string>(), source );
    BOOST_CHECK_EQUAL( readData["bytes"].get<size_t>(), source.size() );
    BOOST_CHECK_EQUAL( readData["sourceSha256"].get<std::string>(), firstHash );
    BOOST_CHECK( readData["valid"].get<bool>() );
    BOOST_CHECK( !readData.contains( "ir" ) );
    BOOST_CHECK( !readData.contains( "plan" ) );

    JSON loaded = registry.Handle( "design", { { "operation", "compile" },
                                                { "path", "reusable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( loaded.at( "success" ).get<bool>(), loaded.dump() );
    BOOST_CHECK( envelope( loaded )["data"]["valid"].get<bool>() );
    BOOST_CHECK_EQUAL( envelope( loaded )["data"]["sourceSha256"].get<std::string>(),
                       firstHash );
    BOOST_CHECK( !envelope( loaded )["data"].contains( "ir" ) );

    const std::string updated = source + "; reusable sidecar comment\n";
    JSON noHash = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "reusable.kicad_kds" },
                                                { "source", updated } } );
    BOOST_CHECK( !noHash.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( noHash )["error"]["code"].get<std::string>(), "stale_source" );

    JSON stale = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "reusable.kicad_kds" },
                                               { "source", updated },
                                               { "expectedSha256", std::string( 64, '0' ) } } );
    BOOST_CHECK( !stale.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( stale )["error"]["code"].get<std::string>(), "stale_source" );

    JSON replaced = registry.Handle( "design", { { "operation", "save" },
                                                  { "path", "reusable.kicad_kds" },
                                                  { "source", updated },
                                                  { "expectedSha256", firstHash } } );
    BOOST_REQUIRE_MESSAGE( replaced.at( "success" ).get<bool>(), replaced.dump() );

    wxFile installed( wxFileName( fixture.Root(), wxS( "reusable.kicad_kds" ) ).GetFullPath(),
                      wxFile::read );
    BOOST_REQUIRE( installed.IsOpened() );
    std::string installedSource( static_cast<size_t>( installed.Length() ), '\0' );
    BOOST_REQUIRE_EQUAL( installed.Read( installedSource.data(), installedSource.size() ),
                         installedSource.size() );
    BOOST_CHECK_EQUAL( installedSource, updated );

    JSON reread = registry.Handle( "design", { { "operation", "read" },
                                                 { "path", "reusable.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( reread.at( "success" ).get<bool>(), reread.dump() );
    BOOST_CHECK_EQUAL( envelope( reread )["data"]["source"].get<std::string>(), updated );

    JSON escaped = registry.Handle( "design", { { "operation", "save" },
                                                 { "path", "../escape.kicad_kds" },
                                                 { "source", source } } );
    BOOST_CHECK( !escaped.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( escaped )["error"]["code"].get<std::string>(), "invalid_path" );
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

    result = registry.Handle( "design", { { "operation", "read" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_arguments" );

    const std::string malformedSource = "(kichad_design";
    wxFFile malformedFile( wxFileName( fixture.Root(), wxS( "malformed.kicad_kds" ) )
                                   .GetFullPath(),
                           wxS( "wb" ) );
    BOOST_REQUIRE( malformedFile.IsOpened() );
    BOOST_REQUIRE_EQUAL( malformedFile.Write( malformedSource.data(), malformedSource.size() ),
                         malformedSource.size() );
    malformedFile.Close();

    result = registry.Handle( "design", { { "operation", "read" },
                                            { "path", "malformed.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    BOOST_CHECK_EQUAL( envelope( result )["data"]["source"].get<std::string>(),
                       malformedSource );
    BOOST_CHECK( !envelope( result )["data"]["valid"].get<bool>() );

    const std::string invalidUtf8 = "(kichad_design \xFF)";
    wxFFile invalidFile( wxFileName( fixture.Root(), wxS( "invalid-utf8.kicad_kds" ) )
                                 .GetFullPath(),
                         wxS( "wb" ) );
    BOOST_REQUIRE( invalidFile.IsOpened() );
    BOOST_REQUIRE_EQUAL( invalidFile.Write( invalidUtf8.data(), invalidUtf8.size() ),
                         invalidUtf8.size() );
    invalidFile.Close();

    result = registry.Handle( "design", { { "operation", "read" },
                                            { "path", "invalid-utf8.kicad_kds" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "invalid_source" );

    result = registry.Handle( "pcb", { { "operation", "status" },
                                        { "path", "design.kicad_pro" } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(), "invalid_path" );

    result = registry.Handle( "pcb", { { "operation", "mutate" },
                                        { "path", "design.kicad_pcb" },
                                        { "itemType", "trace" },
                                        { "action", "delete" },
                                        { "ids", JSON::array( { KIID().AsStdString() } ) } } );
    BOOST_CHECK( !result.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( result )["error"]["code"].get<std::string>(),
                       "snapshot_required" );
}


BOOST_AUTO_TEST_CASE( CreatesPcbItemsInsideAnIpcTransaction )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-tool-token";
    const std::string commitId = KIID().AsStdString();
    const std::string createdId = KIID().AsStdString();
    std::atomic<int> beginCount( 0 );
    std::atomic<int> commitCount( 0 );

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    if( ++beginCount == 1 )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message(
                                "an earlier commit response was lost" );
                    }
                    else
                    {
                        kiapi::common::commands::BeginCommitResponse begin;
                        begin.mutable_id()->set_value( commitId );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( begin );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::CreateItems>() )
                {
                    kiapi::common::commands::CreateItems create;
                    kiapi::board::types::Track track;

                    if( request.header().kicad_token() == token
                        && request.message().UnpackTo( &create ) && create.items_size() == 1
                        && create.items( 0 ).UnpackTo( &track ) )
                    {
                        track.mutable_id()->set_value( createdId );
                        kiapi::common::commands::CreateItemsResponse created;
                        created.mutable_header()->CopyFrom( create.header() );
                        created.set_status( kiapi::common::types::IRS_OK );
                        auto* item = created.add_created_items();
                        item->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        item->mutable_item()->PackFrom( track );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( created );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::board::types::Track track;

                    if( request.message().UnpackTo( &update ) && update.items_size() == 1
                        && update.items( 0 ).UnpackTo( &track )
                        && track.id().value() == createdId
                        && update.header().field_mask().paths_size() == 1 )
                    {
                        kiapi::common::commands::UpdateItemsResponse updated;
                        updated.mutable_header()->CopyFrom( update.header() );
                        updated.set_status( kiapi::common::types::IRS_OK );
                        auto* item = updated.add_updated_items();
                        item->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        item->mutable_item()->PackFrom( track );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( updated );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::DeleteItems>() )
                {
                    kiapi::common::commands::DeleteItems remove;

                    if( request.message().UnpackTo( &remove ) && remove.item_ids_size() == 1
                        && remove.item_ids( 0 ).value() == createdId )
                    {
                        kiapi::common::commands::DeleteItemsResponse deleted;
                        deleted.mutable_header()->CopyFrom( remove.header() );
                        deleted.set_status( kiapi::common::types::IRS_OK );
                        auto* item = deleted.add_deleted_items();
                        item->mutable_id()->set_value( createdId );
                        item->set_status( kiapi::common::commands::IDS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( deleted );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;

                    if( request.message().UnpackTo( &end ) && end.id().value() == commitId
                        && end.action() == kiapi::common::commands::CMA_COMMIT )
                    {
                        ++commitCount;
                    }

                    kiapi::common::commands::EndCommitResponse ended;
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( ended );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    JSON result = registry.Handle(
            "pcb", { { "operation", "mutate" }, { "path", "design.kicad_pcb" },
                     { "itemType", "trace" }, { "action", "create" },
                     { "commitMessage", "Route QA trace" },
                     { "items",
                       JSON::array( { { { "start", { { "xNm", "1000000" },
                                                       { "yNm", "2000000" } } },
                                        { "end", { { "xNm", "3000000" },
                                                     { "yNm", "4000000" } } },
                                        { "width", { { "valueNm", "250000" } } },
                                        { "layer", "BL_F_Cu" },
                                        { "net", { { "name", "GND" } } } } } ) } } );

    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    JSON data = envelope( result ).at( "data" );
    BOOST_CHECK_EQUAL( data.at( "transaction" ).get<std::string>(), "committed" );
    BOOST_REQUIRE_EQUAL( data.at( "itemIds" ).size(), 1 );
    BOOST_CHECK_EQUAL( data["itemIds"][0].get<std::string>(), createdId );
    BOOST_CHECK_EQUAL( beginCount.load(), 2 );
    BOOST_CHECK_EQUAL( commitCount.load(), 1 );

    result = registry.Handle(
            "pcb", { { "operation", "mutate" }, { "path", "design.kicad_pcb" },
                     { "itemType", "trace" }, { "action", "update" },
                     { "fieldMask", JSON::array( { "width" } ) },
                     { "items",
                       JSON::array( { { { "id", { { "value", createdId } } },
                                        { "width", { { "valueNm", "300000" } } } } } ) } } );
    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    BOOST_CHECK_EQUAL( envelope( result )["data"]["transaction"].get<std::string>(),
                       "committed" );

    result = registry.Handle(
            "pcb", { { "operation", "mutate" }, { "path", "design.kicad_pcb" },
                     { "itemType", "trace" }, { "action", "delete" },
                     { "ids", JSON::array( { createdId } ) } } );
    BOOST_REQUIRE_MESSAGE( result.at( "success" ).get<bool>(), result.dump() );
    BOOST_CHECK_EQUAL( envelope( result )["data"]["affectedItems"].get<size_t>(), 1 );
    BOOST_CHECK_EQUAL( beginCount.load(), 4 );
    BOOST_CHECK_EQUAL( commitCount.load(), 3 );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesReusableDesignsIdempotentlyWithManagedState )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-design-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-design-token";
    std::map<std::string, google::protobuf::Any> liveItems;
    std::map<std::string, google::protobuf::Any> transactionBefore;
    int commitCount = 0;
    bool rejectNextCreate = false;
    bool rejectNextCommitResponse = false;
    bool transactionActive = false;

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::GetItemsById>() )
                {
                    kiapi::common::commands::GetItemsById get;
                    kiapi::common::commands::GetItemsResponse items;

                    if( request.message().UnpackTo( &get ) )
                    {
                        for( const auto& id : get.items() )
                        {
                            auto item = liveItems.find( id.value() );

                            if( item != liveItems.end() )
                                items.add_items()->CopyFrom( item->second );
                        }

                        items.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( items );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    if( transactionActive )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message( "transaction already active" );
                    }
                    else
                    {
                        transactionActive = true;
                        transactionBefore = liveItems;
                        kiapi::common::commands::BeginCommitResponse begin;
                        begin.mutable_id()->set_value( KIID().AsStdString() );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( begin );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::CreateItems>() )
                {
                    kiapi::common::commands::CreateItems create;
                    kiapi::common::commands::CreateItemsResponse created;

                    if( request.message().UnpackTo( &create ) )
                    {
                        for( const google::protobuf::Any& packed : create.items() )
                        {
                            std::string id;
                            kiapi::board::types::BoardGraphicShape shape;
                            kiapi::board::types::Track track;

                            if( packed.UnpackTo( &shape ) )
                                id = shape.id().value();
                            else if( packed.UnpackTo( &track ) )
                                id = track.id().value();

                            auto* result = created.add_created_items();

                            if( rejectNextCreate )
                            {
                                rejectNextCreate = false;
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_INVALID_DATA );
                                result->mutable_status()->set_error_message( "injected failure" );
                            }
                            else if( id.empty() || liveItems.contains( id ) )
                            {
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_EXISTING );
                            }
                            else
                            {
                                liveItems[id] = packed;
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_OK );
                                result->mutable_item()->CopyFrom( packed );
                            }
                        }

                        created.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( created );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::common::commands::UpdateItemsResponse updated;

                    if( request.message().UnpackTo( &update ) )
                    {
                        for( const google::protobuf::Any& packed : update.items() )
                        {
                            std::string id;
                            kiapi::board::types::BoardGraphicShape shape;
                            kiapi::board::types::Track track;

                            if( packed.UnpackTo( &shape ) )
                                id = shape.id().value();
                            else if( packed.UnpackTo( &track ) )
                                id = track.id().value();

                            auto* result = updated.add_updated_items();

                            if( id.empty() || !liveItems.contains( id ) )
                            {
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_NONEXISTENT );
                            }
                            else
                            {
                                liveItems[id] = packed;
                                result->mutable_status()->set_code(
                                        kiapi::common::commands::ISC_OK );
                                result->mutable_item()->CopyFrom( packed );
                            }
                        }

                        updated.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( updated );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::DeleteItems>() )
                {
                    kiapi::common::commands::DeleteItems remove;
                    kiapi::common::commands::DeleteItemsResponse deleted;

                    if( request.message().UnpackTo( &remove ) )
                    {
                        for( const auto& id : remove.item_ids() )
                        {
                            auto* result = deleted.add_deleted_items();
                            result->mutable_id()->CopyFrom( id );
                            result->set_status( liveItems.erase( id.value() ) == 1
                                                        ? kiapi::common::commands::IDS_OK
                                                        : kiapi::common::commands::IDS_NONEXISTENT );
                        }

                        deleted.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( deleted );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;
                    request.message().UnpackTo( &end );
                    bool rejectResponse = false;

                    if( end.action() == kiapi::common::commands::CMA_COMMIT )
                    {
                        if( transactionActive )
                        {
                            transactionActive = false;
                            ++commitCount;
                            rejectResponse = rejectNextCommitResponse;
                            rejectNextCommitResponse = false;
                        }
                        else
                        {
                            response.mutable_status()->set_status(
                                    kiapi::common::AS_BAD_REQUEST );
                            response.mutable_status()->set_error_message(
                                    "no transaction is active" );
                        }
                    }
                    else if( end.action() == kiapi::common::commands::CMA_DROP )
                    {
                        if( transactionActive )
                        {
                            transactionActive = false;
                            liveItems = transactionBefore;
                        }
                        else
                        {
                            response.mutable_status()->set_status(
                                    kiapi::common::AS_BAD_REQUEST );
                            response.mutable_status()->set_error_message(
                                    "no transaction is active" );
                        }
                    }

                    kiapi::common::commands::EndCommitResponse ended;

                    if( rejectResponse )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message( "injected lost acknowledgement" );
                    }
                    else if( response.status().status() == kiapi::common::AS_UNKNOWN )
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( ended );
                    }
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project managed_design)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1) (pin R2 1))
  (board
    (outline (rect (id edge) (at 0mm 0mm) (size 20mm 10mm)))
    (route SIGNAL (id trace-a) (from 1mm 2mm) (to 3mm 4mm)
      (width 0.25mm) (layer F.Cu))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "managed.kicad_kds" },
                                               { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                 { "path", "managed.kicad_kds" },
                                                 { "boardPath", "design.kicad_pcb" },
                                                 { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    BOOST_CHECK_EQUAL( envelope( applied )["data"]["counts"]["create"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );
    BOOST_CHECK_EQUAL( commitCount, 1 );

    JSON repeated = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "managed.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( repeated.at( "success" ).get<bool>(), repeated.dump() );
    BOOST_CHECK_EQUAL( envelope( repeated )["data"]["counts"]["update"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );

    const std::string reduced = R"KDS((kichad_design
  (version 1)
  (project managed_design)
  (board (outline (rect (id edge) (at 0mm 0mm) (size 25mm 10mm)))))
)KDS";
    JSON replaced = registry.Handle( "design", { { "operation", "save" },
                                                  { "path", "managed.kicad_kds" },
                                                  { "source", reduced },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( replaced.at( "success" ).get<bool>(), replaced.dump() );
    hash = envelope( replaced )["data"]["sourceSha256"].get<std::string>();
    JSON reducedApply = registry.Handle( "design", { { "operation", "apply" },
                                                      { "path", "managed.kicad_kds" },
                                                      { "boardPath", "design.kicad_pcb" },
                                                      { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( reducedApply.at( "success" ).get<bool>(), reducedApply.dump() );
    BOOST_CHECK_EQUAL( envelope( reducedApply )["data"]["counts"]["delete"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( liveItems.size(), 1 );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "managed.kicad_kds_state" ) ).GetFullPath() ) );
    BOOST_CHECK( !wxFileExists( wxFileName( fixture.Root(),
                                            wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );

    const std::string recoveredSource = R"KDS((kichad_design
  (version 1)
  (project managed_design)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net SIGNAL (pin R1 1) (pin R2 1))
  (board
    (outline (rect (id edge) (at 0mm 0mm) (size 30mm 10mm)))
    (route SIGNAL (id recovered-trace) (from 1mm 2mm) (to 4mm 4mm)
      (width 0.3mm) (layer F.Cu))))
)KDS";
    replaced = registry.Handle( "design", { { "operation", "save" },
                                             { "path", "managed.kicad_kds" },
                                             { "source", recoveredSource },
                                             { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( replaced.at( "success" ).get<bool>(), replaced.dump() );
    hash = envelope( replaced )["data"]["sourceSha256"].get<std::string>();
    rejectNextCreate = true;
    JSON failed = registry.Handle( "design", { { "operation", "apply" },
                                                { "path", "managed.kicad_kds" },
                                                { "boardPath", "design.kicad_pcb" },
                                                { "expectedSha256", hash } } );
    BOOST_CHECK( !failed.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( failed )["error"]["code"].get<std::string>(), "apply_failed" );
    BOOST_CHECK_EQUAL( liveItems.size(), 1 );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );

    rejectNextCommitResponse = true;
    JSON ambiguous = registry.Handle( "design", { { "operation", "apply" },
                                                    { "path", "managed.kicad_kds" },
                                                    { "boardPath", "design.kicad_pcb" },
                                                    { "expectedSha256", hash } } );
    BOOST_CHECK( !ambiguous.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( ambiguous )["error"]["code"].get<std::string>(),
                       "transaction_failed" );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );

    JSON recovered = registry.Handle( "design", { { "operation", "apply" },
                                                    { "path", "managed.kicad_kds" },
                                                    { "boardPath", "design.kicad_pcb" },
                                                    { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( recovered.at( "success" ).get<bool>(), recovered.dump() );
    BOOST_CHECK_EQUAL( liveItems.size(), 2 );
    BOOST_CHECK_EQUAL( envelope( recovered )["data"]["counts"]["update"].get<int>(), 2 );
    BOOST_CHECK( !wxFileExists( wxFileName( fixture.Root(),
                                            wxS( "managed.kicad_kds_journal" ) ).GetFullPath() ) );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesKdsPlacementAsNarrowSchematicFootprintUpdate )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-placement-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-placement-token";
    const std::string commitId = "33333333-3333-8333-8333-333333333333";
    const std::string footprintId = "11111111-1111-8111-8111-111111111111";
    const std::string rootSheetId = "44444444-4444-8444-8444-444444444444";
    const std::string symbolId = "55555555-5555-8555-8555-555555555555";
    kiapi::board::types::FootprintInstance liveFootprint;
    liveFootprint.mutable_id()->set_value( footprintId );
    liveFootprint.mutable_position()->set_x_nm( 1000000 );
    liveFootprint.mutable_position()->set_y_nm( 2000000 );
    liveFootprint.mutable_orientation()->set_value_degrees( 0.0 );
    liveFootprint.set_layer( kiapi::board::types::BL_F_Cu );
    liveFootprint.set_locked( kiapi::common::types::LS_UNLOCKED );
    liveFootprint.mutable_reference_field()->mutable_text()->mutable_text()->set_text( "R1" );
    liveFootprint.mutable_attributes()->set_not_in_schematic( false );
    liveFootprint.mutable_symbol_path()->add_path()->set_value( rootSheetId );
    liveFootprint.mutable_symbol_path()->add_path()->set_value( symbolId );
    bool sawFootprintInventory = false;
    bool sawNarrowUpdate = false;
    int  commitCount = 0;

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::GetItems>() )
                {
                    kiapi::common::commands::GetItems get;
                    kiapi::common::commands::GetItemsResponse items;

                    if( request.message().UnpackTo( &get ) && get.types_size() == 1
                        && get.types( 0 ) == kiapi::common::types::KOT_PCB_FOOTPRINT )
                    {
                        sawFootprintInventory = true;
                        items.add_items()->PackFrom( liveFootprint );
                        items.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( items );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    kiapi::common::commands::BeginCommitResponse begin;
                    begin.mutable_id()->set_value( commitId );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( begin );
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::board::types::FootprintInstance placement;
                    const std::set<std::string> expectedMask = {
                        "position", "orientation", "layer", "locked"
                    };
                    std::set<std::string> actualMask;

                    if( request.message().UnpackTo( &update ) )
                    {
                        actualMask.insert( update.header().field_mask().paths().begin(),
                                           update.header().field_mask().paths().end() );
                    }

                    if( update.items_size() == 1 && update.items( 0 ).UnpackTo( &placement )
                        && placement.id().value() == footprintId
                        && placement.position().x_nm() == 12500000
                        && placement.position().y_nm() == 7500000
                        && placement.orientation().value_degrees() == 37.5
                        && placement.layer() == kiapi::board::types::BL_B_Cu
                        && placement.locked() == kiapi::common::types::LS_LOCKED
                        && placement.symbol_path().path_size() == 0
                        && placement.definition().items_size() == 0
                        && actualMask == expectedMask )
                    {
                        sawNarrowUpdate = true;
                        kiapi::common::commands::UpdateItemsResponse updated;
                        updated.mutable_header()->CopyFrom( update.header() );
                        updated.set_status( kiapi::common::types::IRS_OK );
                        auto* item = updated.add_updated_items();
                        item->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        item->mutable_item()->PackFrom( liveFootprint );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( updated );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;
                    request.message().UnpackTo( &end );

                    if( end.id().value() == commitId
                        && end.action() == kiapi::common::commands::CMA_COMMIT )
                    {
                        ++commitCount;
                    }

                    kiapi::common::commands::EndCommitResponse ended;
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( ended );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project placed_design)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (board (place R1 (at 12.5mm 7.5mm) (rotation 37.5deg) (side back) (locked true))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "placed.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    const std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", "placed.kicad_kds" },
                                                  { "boardPath", "design.kicad_pcb" },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    JSON data = envelope( applied )["data"];
    BOOST_CHECK_EQUAL( data["counts"]["placement"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( data["managedItems"].get<int>(), 0 );
    BOOST_CHECK( sawFootprintInventory );
    BOOST_CHECK( sawNarrowUpdate );
    BOOST_CHECK_EQUAL( commitCount, 1 );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( RefillsManagedKdsZonesAndRecoversARejectedRefill )
{
    TOOL_PROJECT_FIXTURE fixture;
    wxFileName socketPath( fixture.Root(), wxS( "api-zone-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-zone-token";
    const std::string commitId = "aaaaaaaa-aaaa-8aaa-8aaa-aaaaaaaaaaaa";
    kiapi::board::types::Zone liveZone;
    bool rejectNextRefill = true;
    bool sawRefill = false;
    int commitCount = 0;

    server.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( !request.ParseFromString( *aSerializedRequest ) )
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }
                else if( request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path( fixture.Root().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::GetItemsById>() )
                {
                    kiapi::common::commands::GetItemsById get;
                    kiapi::common::commands::GetItemsResponse items;
                    request.message().UnpackTo( &get );

                    for( const auto& id : get.items() )
                    {
                        if( !liveZone.id().value().empty() && id.value() == liveZone.id().value() )
                            items.add_items()->PackFrom( liveZone );
                    }

                    items.set_status( kiapi::common::types::IRS_OK );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( items );
                }
                else if( request.message().Is<kiapi::common::commands::GetItems>() )
                {
                    kiapi::common::commands::GetItems get;
                    kiapi::common::commands::GetItemsResponse items;
                    request.message().UnpackTo( &get );

                    if( get.types_size() == 1
                        && get.types( 0 ) == kiapi::common::types::KOT_PCB_ZONE )
                    {
                        if( !liveZone.id().value().empty() )
                            items.add_items()->PackFrom( liveZone );

                        items.set_status( kiapi::common::types::IRS_OK );
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( items );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    kiapi::common::commands::BeginCommitResponse begin;
                    begin.mutable_id()->set_value( commitId );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( begin );
                }
                else if( request.message().Is<kiapi::common::commands::CreateItems>() )
                {
                    kiapi::common::commands::CreateItems create;
                    kiapi::common::commands::CreateItemsResponse created;
                    request.message().UnpackTo( &create );
                    auto* result = created.add_created_items();

                    if( create.items_size() == 1 && create.items( 0 ).UnpackTo( &liveZone ) )
                    {
                        result->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        result->mutable_item()->PackFrom( liveZone );
                    }
                    else
                    {
                        result->mutable_status()->set_code(
                                kiapi::common::commands::ISC_INVALID_DATA );
                    }

                    created.set_status( kiapi::common::types::IRS_OK );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( created );
                }
                else if( request.message().Is<kiapi::common::commands::UpdateItems>() )
                {
                    kiapi::common::commands::UpdateItems update;
                    kiapi::common::commands::UpdateItemsResponse updated;
                    request.message().UnpackTo( &update );
                    auto* result = updated.add_updated_items();

                    if( update.items_size() == 1 && update.items( 0 ).UnpackTo( &liveZone ) )
                    {
                        result->mutable_status()->set_code( kiapi::common::commands::ISC_OK );
                        result->mutable_item()->PackFrom( liveZone );
                    }
                    else
                    {
                        result->mutable_status()->set_code(
                                kiapi::common::commands::ISC_INVALID_DATA );
                    }

                    updated.set_status( kiapi::common::types::IRS_OK );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( updated );
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;
                    request.message().UnpackTo( &end );

                    if( end.action() == kiapi::common::commands::CMA_COMMIT )
                        ++commitCount;

                    kiapi::common::commands::EndCommitResponse ended;
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( ended );
                }
                else if( request.message().Is<kiapi::board::commands::RefillZones>() )
                {
                    if( rejectNextRefill )
                    {
                        rejectNextRefill = false;
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                        response.mutable_status()->set_error_message( "injected refill failure" );
                    }
                    else
                    {
                        sawRefill = true;
                        liveZone.set_filled( true );
                        google::protobuf::Empty empty;
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( empty );
                    }
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); }, []() { return true; },
                                  [&fixture]() { return fixture.Root(); } );
    const std::string source = R"KDS((kichad_design
  (version 1)
  (project managed_zone)
  (component R1 (symbol "Device:R") (value "1k") (footprint "R:R"))
  (component R2 (symbol "Device:R") (value "2k") (footprint "R:R"))
  (net GND (pin R1 1) (pin R2 1))
  (board
    (zone GND
      (id ground-plane)
      (layers F.Cu)
      (outline
        (polygon
          (point 0mm 0mm) (point 20mm 0mm) (point 20mm 10mm) (point 0mm 10mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection solid)
      (islands keep_all)
      (fill solid))))
)KDS";
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "zone.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    const std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON failed = registry.Handle( "design", { { "operation", "apply" },
                                                { "path", "zone.kicad_kds" },
                                                { "boardPath", "design.kicad_pcb" },
                                                { "expectedSha256", hash } } );
    BOOST_REQUIRE( !failed.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( failed )["error"]["code"].get<std::string>(),
                       "zone_refill_failed" );
    BOOST_CHECK_EQUAL( commitCount, 1 );
    BOOST_CHECK( !liveZone.id().value().empty() );
    BOOST_CHECK( !liveZone.filled() );
    BOOST_CHECK( wxFileExists( wxFileName( fixture.Root(),
                                           wxS( "zone.kicad_kds_journal" ) ).GetFullPath() ) );

    JSON recovered = registry.Handle( "design", { { "operation", "apply" },
                                                   { "path", "zone.kicad_kds" },
                                                   { "boardPath", "design.kicad_pcb" },
                                                   { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( recovered.at( "success" ).get<bool>(), recovered.dump() );
    JSON data = envelope( recovered )["data"];
    BOOST_CHECK_EQUAL( data["counts"]["update"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( data["zonesRefilled"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( commitCount, 2 );
    BOOST_CHECK( sawRefill );
    BOOST_CHECK( liveZone.filled() );
    BOOST_CHECK( !wxFileExists( wxFileName( fixture.Root(),
                                            wxS( "zone.kicad_kds_journal" ) ).GetFullPath() ) );
    server.Stop();
}


BOOST_AUTO_TEST_CASE( AppliesReusableDesignAgainstLivePcbEditorWhenRequested )
{
    wxString projectPath;
    wxString boardPath;
    wxString sourcePath;

    if( !wxGetEnv( wxS( "KICHAD_QA_LIVE_PROJECT" ), &projectPath )
        || !wxGetEnv( wxS( "KICHAD_QA_LIVE_BOARD" ), &boardPath )
        || !wxGetEnv( wxS( "KICHAD_QA_LIVE_KDS" ), &sourcePath ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in live KDS apply test" );
        return;
    }

    wxFileName project = wxFileName::DirName( projectPath );
    project.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName board( boardPath );
    board.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    wxFileName source( sourcePath );
    source.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    BOOST_REQUIRE( project.DirExists() );
    BOOST_REQUIRE( board.FileExists() );
    BOOST_REQUIRE( source.FileExists() );
    BOOST_REQUIRE_EQUAL( board.GetPath(), project.GetPath() );
    BOOST_REQUIRE_EQUAL( source.GetPath(), project.GetPath() );

    wxString socketDirectory;
    wxGetEnv( wxS( "KICHAD_QA_LIVE_SOCKET_DIR" ), &socketDirectory );
    CODEX_TOOL_REGISTRY registry( [project]() { return project.GetFullPath(); },
                                  []() { return true; },
                                  [socketDirectory]() { return socketDirectory; } );
    const std::string sourceName = source.GetFullName().ToStdString();
    const std::string boardName = board.GetFullName().ToStdString();
    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                  { "path", sourceName } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_REQUIRE( compileData["valid"].get<bool>() );
    const std::string hash = compileData["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                 { "path", sourceName },
                                                 { "boardPath", boardName },
                                                 { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    JSON firstData = envelope( applied )["data"];
    BOOST_CHECK_EQUAL( firstData["managedItems"].get<int>(), 6 );
    BOOST_CHECK_EQUAL( firstData["counts"]["placement"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["zonesRefilled"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( firstData["transaction"].get<std::string>(), "committed" );

    JSON repeated = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", sourceName },
                                                  { "boardPath", boardName },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( repeated.at( "success" ).get<bool>(), repeated.dump() );
    JSON repeatedData = envelope( repeated )["data"];
    BOOST_CHECK_EQUAL( repeatedData["counts"]["create"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["update"].get<int>(), 6 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["delete"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( repeatedData["counts"]["placement"].get<int>(), 1 );

    auto getOne = [&]( const std::string& aItemType )
    {
        JSON response = registry.Handle( "pcb", { { "operation", "get" },
                                                   { "path", boardName },
                                                   { "itemType", aItemType },
                                                   { "limit", 10 } } );
        BOOST_REQUIRE_MESSAGE( response.at( "success" ).get<bool>(), response.dump() );
        JSON data = envelope( response )["data"];
        BOOST_REQUIRE_EQUAL( data["totalItems"].get<int>(), 1 );
        BOOST_REQUIRE_EQUAL( data["items"].size(), 1 );
        return data["items"][0];
    };

    const auto stableUuid = []( const std::string& aType, const std::string& aLogicalId )
    {
        return KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                "live_apply", aType, aLogicalId );
    };

    JSON shape = getOne( "shape" );
    BOOST_CHECK_EQUAL( shape["id"]["value"].get<std::string>(),
                       stableUuid( "shape", "board-edge" ) );
    BOOST_CHECK_EQUAL( shape["layer"].get<std::string>(), "BL_Edge_Cuts" );
    BOOST_CHECK_EQUAL( shape["shape"]["rectangle"]["topLeft"]["xNm"].get<std::string>(),
                       "20000000" );
    BOOST_CHECK_EQUAL( shape["shape"]["rectangle"]["bottomRight"]["yNm"].get<std::string>(),
                       "50000000" );

    JSON trace = getOne( "trace" );
    BOOST_CHECK_EQUAL( trace["id"]["value"].get<std::string>(),
                       stableUuid( "trace", "first-trace" ) );
    BOOST_CHECK_EQUAL( trace["start"]["xNm"].get<std::string>(), "25000000" );
    BOOST_CHECK_EQUAL( trace["end"]["xNm"].get<std::string>(), "35000000" );
    BOOST_CHECK_EQUAL( trace["width"]["valueNm"].get<std::string>(), "250000" );
    BOOST_CHECK_EQUAL( trace["net"]["name"].get<std::string>(), "Net1" );

    JSON arc = getOne( "arc" );
    BOOST_CHECK_EQUAL( arc["id"]["value"].get<std::string>(),
                       stableUuid( "arc", "first-arc" ) );
    BOOST_CHECK_EQUAL( arc["mid"]["xNm"].get<std::string>(), "40000000" );
    BOOST_CHECK_EQUAL( arc["mid"]["yNm"].get<std::string>(), "30000000" );

    JSON via = getOne( "via" );
    BOOST_CHECK_EQUAL( via["id"]["value"].get<std::string>(),
                       stableUuid( "via", "first-via" ) );
    BOOST_CHECK_EQUAL( via["position"]["xNm"].get<std::string>(), "45000000" );
    BOOST_CHECK_EQUAL( via["padStack"]["drill"]["diameter"]["xNm"].get<std::string>(),
                       "400000" );
    BOOST_CHECK_EQUAL( via["net"]["name"].get<std::string>(), "Net1" );

    JSON zone = getOne( "zone" );
    BOOST_CHECK_EQUAL( zone["id"]["value"].get<std::string>(),
                       stableUuid( "zone", "ground-plane" ) );
    BOOST_CHECK_EQUAL( zone["type"].get<std::string>(), "ZT_COPPER" );
    BOOST_REQUIRE_EQUAL( zone["layers"].size(), 1 );
    BOOST_CHECK_EQUAL( zone["layers"][0].get<std::string>(), "BL_F_Cu" );
    BOOST_CHECK( zone["filled"].get<bool>() );
    BOOST_CHECK_EQUAL( zone["name"].get<std::string>(), "Net1 live fill proof" );
    BOOST_CHECK_EQUAL( zone["copperSettings"]["net"]["name"].get<std::string>(), "Net1" );
    BOOST_CHECK_EQUAL(
            zone["copperSettings"]["connection"]["thermalSpokes"]["width"]["valueNm"]
                    .get<std::string>(),
            "350000" );
    BOOST_CHECK_EQUAL( zone["copperSettings"]["minIslandArea"].get<std::string>(),
                       "1000000000000" );
    BOOST_REQUIRE_EQUAL( zone["outline"]["polygons"].size(), 1 );
    BOOST_REQUIRE_EQUAL( zone["outline"]["polygons"][0]["outline"]["nodes"].size(), 4 );

    JSON keepout = getOne( "rule_area" );
    BOOST_CHECK_EQUAL( keepout["id"]["value"].get<std::string>(),
                       stableUuid( "rule_area", "no-routing" ) );
    BOOST_CHECK_EQUAL( keepout["type"].get<std::string>(), "ZT_RULE_AREA" );
    BOOST_REQUIRE_EQUAL( keepout["layers"].size(), 1 );
    BOOST_CHECK_EQUAL( keepout["layers"][0].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutCopper"].get<bool>() );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutVias"].get<bool>() );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutTracks"].get<bool>() );
    BOOST_CHECK( !keepout["ruleAreaSettings"].value( "keepoutPads", false ) );
    BOOST_CHECK( keepout["ruleAreaSettings"]["keepoutFootprints"].get<bool>() );
    BOOST_CHECK( !keepout["ruleAreaSettings"].value( "placementEnabled", false ) );
    BOOST_CHECK_EQUAL( keepout["border"]["pitch"]["valueNm"].get<std::string>(), "400000" );
    BOOST_CHECK_EQUAL( keepout["locked"].get<std::string>(), "LS_LOCKED" );
    BOOST_CHECK( !keepout.value( "filled", false ) );
    BOOST_REQUIRE_EQUAL( keepout["outline"]["polygons"][0]["outline"]["nodes"].size(), 4 );

    JSON footprint = getOne( "footprint" );
    BOOST_CHECK_EQUAL( footprint["id"]["value"].get<std::string>(),
                       "11111111-1111-8111-8111-111111111111" );
    BOOST_CHECK_EQUAL( footprint["position"]["xNm"].get<std::string>(), "30000000" );
    BOOST_CHECK_EQUAL( footprint["position"]["yNm"].get<std::string>(), "40000000" );
    BOOST_CHECK_EQUAL( footprint["orientation"]["valueDegrees"].get<double>(), 135.0 );
    BOOST_CHECK_EQUAL( footprint["layer"].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK_EQUAL( footprint["locked"].get<std::string>(), "LS_LOCKED" );
    BOOST_CHECK_EQUAL(
            footprint["referenceField"]["text"]["text"]["text"].get<std::string>(),
            "R1" );
    BOOST_REQUIRE_EQUAL( footprint["symbolPath"]["path"].size(), 1 );
    BOOST_CHECK_EQUAL( footprint["symbolPath"]["path"][0]["value"].get<std::string>(),
                       "66666666-6666-8666-8666-666666666666" );

    JSON pad;

    for( const JSON& child : footprint["definition"]["items"] )
    {
        if( child.value( "@type", "" ).ends_with( ".Pad" )
            && child.value( "number", "" ) == "1" )
        {
            pad = child;
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE( !pad.is_null(), footprint.dump() );
    BOOST_CHECK_EQUAL( pad["id"]["value"].get<std::string>(),
                       "88888888-8888-8888-8888-888888888888" );
    BOOST_REQUIRE_EQUAL( pad["padStack"]["layers"].size(), 3 );
    BOOST_CHECK_EQUAL( pad["padStack"]["layers"][0].get<std::string>(), "BL_B_Cu" );
    BOOST_CHECK_EQUAL( pad["padStack"]["layers"][1].get<std::string>(), "BL_B_Mask" );
    BOOST_CHECK_EQUAL( pad["padStack"]["layers"][2].get<std::string>(), "BL_B_Paste" );
}


BOOST_AUTO_TEST_SUITE_END()
