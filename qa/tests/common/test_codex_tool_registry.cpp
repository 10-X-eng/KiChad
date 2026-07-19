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

#include <atomic>
#include <import_export.h>
#include <api/board/board_types.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <kiid.h>
#include <kinng.h>
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

    BOOST_REQUIRE_EQUAL( specs.size(), 4 );
    BOOST_CHECK_EQUAL( specs[0]["name"].get<std::string>(), "project" );
    BOOST_CHECK_EQUAL( specs[1]["name"].get<std::string>(), "inspect" );
    BOOST_CHECK_EQUAL( specs[2]["name"].get<std::string>(), "design" );
    BOOST_CHECK_EQUAL( specs[3]["name"].get<std::string>(), "pcb" );
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
  (check erc)
  (check drc)
  (output gerbers))
)KDS";

    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                  { "source", source } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_CHECK( compileData["valid"].get<bool>() );
    BOOST_CHECK( !compileData["irOmitted"].get<bool>() );
    const std::string firstHash = compileData["sourceSha256"].get<std::string>();

    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                               { "path", "reusable.kicad_kds" },
                                               { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    BOOST_CHECK_EQUAL( envelope( saved )["data"]["sourceSha256"].get<std::string>(),
                       firstHash );

    JSON loaded = registry.Handle( "design", { { "operation", "compile" },
                                                { "path", "reusable.kicad_kds" },
                                                { "includeIr", false } } );
    BOOST_REQUIRE_MESSAGE( loaded.at( "success" ).get<bool>(), loaded.dump() );
    BOOST_CHECK( envelope( loaded )["data"]["valid"].get<bool>() );
    BOOST_CHECK_EQUAL( envelope( loaded )["data"]["sourceSha256"].get<std::string>(),
                       firstHash );
    BOOST_CHECK( envelope( loaded )["data"]["irOmitted"].get<bool>() );
    BOOST_CHECK_EQUAL( envelope( loaded )["data"]["irOmissionReason"].get<std::string>(),
                       "not_requested" );

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


BOOST_AUTO_TEST_SUITE_END()
