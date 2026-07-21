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

#include <kicad/codex/kicad_ipc_client.h>

#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <chrono>
#include <kiid.h>
#include <kinng.h>
#include <memory>
#include <vector>
#include <wx/filename.h>
#include <wx/utils.h>


BOOST_AUTO_TEST_SUITE( KiChadIpcClient )


BOOST_AUTO_TEST_CASE( DiscoversBoardAndUsesTransactionEnvelope )
{
    wxFileName socketDirectory = wxFileName::DirName( wxFileName::GetTempDir() );
    socketDirectory.AppendDir( wxS( "kichad-ipc-test-" ) + KIID().AsString() );
    BOOST_REQUIRE( wxFileName::Mkdir( socketDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName projectDirectory( socketDirectory );
    projectDirectory.AppendDir( wxS( "project" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( projectDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName socketPath( socketDirectory.GetFullPath(), wxS( "api-test.sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    const std::string token = "qa-kicad-token";
    const std::string commitId = KIID().AsStdString();

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
                    document->mutable_project()->set_path(
                            projectDirectory.GetFullPath().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else if( request.message().Is<kiapi::common::commands::BeginCommit>() )
                {
                    kiapi::common::commands::BeginCommitResponse begin;
                    begin.mutable_id()->set_value( commitId );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( begin );
                }
                else if( request.message().Is<kiapi::common::commands::EndCommit>() )
                {
                    kiapi::common::commands::EndCommit end;

                    if( request.message().UnpackTo( &end ) && end.id().value() == commitId
                        && end.action() == kiapi::common::commands::CMA_COMMIT
                        && request.header().kicad_token() == token )
                    {
                        kiapi::common::commands::EndCommitResponse ended;
                        response.mutable_status()->set_status( kiapi::common::AS_OK );
                        response.mutable_message()->PackFrom( ended );
                    }
                    else
                    {
                        response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                    }
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_UNHANDLED );
                }

                server.Reply( response.SerializeAsString() );
            } );

    KICHAD_IPC_CLIENT client( "org.kichad.qa", socketDirectory.GetFullPath(),
                              std::chrono::milliseconds( 1000 ) );
    KICHAD_IPC_TARGET target;
    std::string       error;
    BOOST_REQUIRE_MESSAGE( client.FindOpenPcb( projectDirectory.GetFullPath(),
                                               wxS( "design.kicad_pcb" ), target, error ),
                           error );
    BOOST_CHECK_EQUAL( target.kicadToken, token );
    BOOST_CHECK_EQUAL( target.document.board_filename(), "design.kicad_pcb" );

    std::string returnedCommitId;
    BOOST_REQUIRE_MESSAGE( client.BeginCommit( target, returnedCommitId, error ), error );
    BOOST_CHECK_EQUAL( returnedCommitId, commitId );
    BOOST_CHECK_MESSAGE( client.EndCommit( target, returnedCommitId, true,
                                           "KiChad QA transaction", error ),
                         error );

    server.Stop();
    wxFileName::Rmdir( socketDirectory.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_CASE( DiscoveryTimeoutCoversAllUnresponsiveSockets )
{
    wxFileName socketDirectory = wxFileName::DirName( wxFileName::GetTempDir() );
    socketDirectory.AppendDir( wxS( "kichad-ipc-timeout-test-" ) + KIID().AsString() );
    BOOST_REQUIRE( wxFileName::Mkdir( socketDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    std::vector<std::unique_ptr<KINNG_REQUEST_SERVER>> servers;

    for( int i = 0; i < 3; ++i )
    {
        wxFileName socketPath( socketDirectory.GetFullPath(),
                               wxString::Format( wxS( "api-%d.sock" ), i ) );
        auto server = std::make_unique<KINNG_REQUEST_SERVER>(
                "ipc://" + socketPath.GetFullPath().ToStdString() );
        server->SetCallback( []( std::string* ) {} );
        servers.emplace_back( std::move( server ) );
    }

    KICHAD_IPC_CLIENT client( "org.kichad.qa", socketDirectory.GetFullPath(),
                              std::chrono::milliseconds( 100 ) );
    KICHAD_IPC_TARGET target;
    std::string       error;
    auto              start = std::chrono::steady_clock::now();

    BOOST_CHECK( !client.FindOpenPcb( socketDirectory.GetFullPath(),
                                      wxS( "missing.kicad_pcb" ), target, error ) );

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start );
    BOOST_CHECK_LT( elapsed.count(), 500 );
    BOOST_CHECK( !error.empty() );

    for( const auto& server : servers )
        server->Stop();

    wxFileName::Rmdir( socketDirectory.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_CASE( StaleSocketDoesNotHideLiveEditor )
{
    wxFileName socketDirectory = wxFileName::DirName( wxFileName::GetTempDir() );
    socketDirectory.AppendDir( wxS( "kichad-ipc-stale-test-" ) + KIID().AsString() );
    BOOST_REQUIRE( wxFileName::Mkdir( socketDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName projectDirectory( socketDirectory );
    projectDirectory.AppendDir( wxS( "project" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( projectDirectory.GetFullPath(), wxS_DIR_DEFAULT,
                                     wxPATH_MKDIR_FULL ) );

    wxFileName stalePath( socketDirectory.GetFullPath(), wxS( "api-0.sock" ) );
    KINNG_REQUEST_SERVER staleServer( "ipc://" + stalePath.GetFullPath().ToStdString() );
    staleServer.SetCallback( []( std::string* ) {} );

    wxFileName livePath( socketDirectory.GetFullPath(), wxS( "api-1.sock" ) );
    KINNG_REQUEST_SERVER liveServer( "ipc://" + livePath.GetFullPath().ToStdString() );
    const std::string token = "qa-live-after-stale";
    liveServer.SetCallback(
            [&]( std::string* aSerializedRequest )
            {
                kiapi::common::ApiRequest request;
                kiapi::common::ApiResponse response;
                response.mutable_header()->set_kicad_token( token );

                if( request.ParseFromString( *aSerializedRequest )
                    && request.message().Is<kiapi::common::commands::GetOpenDocuments>() )
                {
                    kiapi::common::commands::GetOpenDocumentsResponse documents;
                    auto* document = documents.add_documents();
                    document->set_type( kiapi::common::types::DOCTYPE_PCB );
                    document->set_board_filename( "design.kicad_pcb" );
                    document->mutable_project()->set_name( "design" );
                    document->mutable_project()->set_path(
                            projectDirectory.GetFullPath().ToStdString() );
                    response.mutable_status()->set_status( kiapi::common::AS_OK );
                    response.mutable_message()->PackFrom( documents );
                }
                else
                {
                    response.mutable_status()->set_status( kiapi::common::AS_BAD_REQUEST );
                }

                liveServer.Reply( response.SerializeAsString() );
            } );

    KICHAD_IPC_CLIENT client( "org.kichad.qa", socketDirectory.GetFullPath(),
                              std::chrono::milliseconds( 600 ) );
    KICHAD_IPC_TARGET target;
    std::string error;
    BOOST_REQUIRE_MESSAGE( client.FindOpenPcb( projectDirectory.GetFullPath(),
                                               wxS( "design.kicad_pcb" ), target, error ),
                           error );
    BOOST_CHECK_EQUAL( target.kicadToken, token );
    BOOST_CHECK_EQUAL( target.socketUrl,
                       "ipc://" + livePath.GetFullPath().ToStdString() );

    liveServer.Stop();
    staleServer.Stop();
    wxFileName::Rmdir( socketDirectory.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_CASE( LiveCreateUpdateDeleteWhenRequested )
{
    wxString projectPath;
    wxString boardPath;
    wxString socketDirectory;

    if( !wxGetEnv( wxS( "KICHAD_QA_LIVE_PROJECT" ), &projectPath )
        || !wxGetEnv( wxS( "KICHAD_QA_LIVE_BOARD" ), &boardPath ) )
    {
        BOOST_TEST_MESSAGE( "Set KICHAD_QA_LIVE_PROJECT and KICHAD_QA_LIVE_BOARD to run the "
                            "live mutating IPC smoke test" );
        BOOST_CHECK( true );
        return;
    }

    wxGetEnv( wxS( "KICHAD_QA_LIVE_SOCKET_DIR" ), &socketDirectory );

    KICHAD_IPC_CLIENT client( "org.kichad.qa-live", socketDirectory,
                              std::chrono::milliseconds( 2000 ) );
    KICHAD_IPC_TARGET target;
    std::string       error;
    BOOST_REQUIRE_MESSAGE( client.FindOpenPcb( projectPath, boardPath, target, error ), error );

    std::string commitId;
    BOOST_REQUIRE_MESSAGE( client.BeginCommit( target, commitId, error ), error );
    bool transactionActive = true;

    auto dropTransaction = [&]()
    {
        if( transactionActive )
        {
            std::string ignored;
            client.EndCommit( target, commitId, false, "", ignored );
            transactionActive = false;
        }
    };

    kiapi::board::types::Track trace;
    trace.mutable_start()->set_x_nm( 10000000 );
    trace.mutable_start()->set_y_nm( 10000000 );
    trace.mutable_end()->set_x_nm( 12000000 );
    trace.mutable_end()->set_y_nm( 10000000 );
    trace.mutable_width()->set_value_nm( 200000 );
    trace.set_locked( kiapi::common::types::LS_UNLOCKED );
    trace.set_layer( kiapi::board::types::BL_F_Cu );

    kiapi::common::commands::CreateItems createRequest;
    createRequest.mutable_header()->mutable_document()->CopyFrom( target.document );
    createRequest.add_items()->PackFrom( trace );

    kiapi::common::ApiResponse createEnvelope;

    if( !client.Call( target, createRequest, createEnvelope, error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    kiapi::common::commands::CreateItemsResponse created;

    if( !createEnvelope.message().UnpackTo( &created ) || created.created_items_size() != 1
        || created.created_items( 0 ).status().code() != kiapi::common::commands::ISC_OK
        || !created.created_items( 0 ).item().UnpackTo( &trace ) || trace.id().value().empty() )
    {
        dropTransaction();
        BOOST_FAIL( "KiCad returned an invalid live create response" );
    }

    if( !client.EndCommit( target, commitId, true, "KiChad live IPC create", error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    transactionActive = false;

    if( !client.BeginCommit( target, commitId, error ) )
        BOOST_FAIL( error );

    transactionActive = true;

    kiapi::board::types::Track update;
    update.mutable_id()->CopyFrom( trace.id() );
    update.mutable_width()->set_value_nm( 250000 );

    kiapi::common::commands::UpdateItems updateRequest;
    updateRequest.mutable_header()->mutable_document()->CopyFrom( target.document );
    updateRequest.mutable_header()->mutable_field_mask()->add_paths( "width" );
    updateRequest.add_items()->PackFrom( update );

    kiapi::common::ApiResponse updateEnvelope;

    if( !client.Call( target, updateRequest, updateEnvelope, error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    kiapi::common::commands::UpdateItemsResponse updated;

    if( !updateEnvelope.message().UnpackTo( &updated ) || updated.updated_items_size() != 1
        || updated.updated_items( 0 ).status().code() != kiapi::common::commands::ISC_OK )
    {
        dropTransaction();
        BOOST_FAIL( "KiCad returned an invalid live update response: envelope="
                    + updateEnvelope.ShortDebugString() + " updated="
                    + updated.ShortDebugString() );
    }

    kiapi::board::types::Track updatedTrace;
    BOOST_REQUIRE( updated.updated_items( 0 ).item().UnpackTo( &updatedTrace ) );
    BOOST_CHECK_EQUAL( updatedTrace.start().x_nm(), 10000000 );
    BOOST_CHECK_EQUAL( updatedTrace.end().x_nm(), 12000000 );
    BOOST_CHECK_EQUAL( updatedTrace.width().value_nm(), 250000 );

    if( !client.EndCommit( target, commitId, true, "KiChad live IPC update", error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    transactionActive = false;

    if( !client.BeginCommit( target, commitId, error ) )
        BOOST_FAIL( error );

    transactionActive = true;

    kiapi::common::commands::DeleteItems deleteRequest;
    deleteRequest.mutable_header()->mutable_document()->CopyFrom( target.document );
    deleteRequest.add_item_ids()->CopyFrom( trace.id() );

    kiapi::common::ApiResponse deleteEnvelope;

    if( !client.Call( target, deleteRequest, deleteEnvelope, error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    kiapi::common::commands::DeleteItemsResponse deleted;

    if( !deleteEnvelope.message().UnpackTo( &deleted ) || deleted.deleted_items_size() != 1
        || deleted.deleted_items( 0 ).status() != kiapi::common::commands::IDS_OK )
    {
        dropTransaction();
        BOOST_FAIL( "KiCad returned an invalid live delete response" );
    }

    if( !client.EndCommit( target, commitId, true, "KiChad live IPC delete", error ) )
    {
        dropTransaction();
        BOOST_FAIL( error );
    }

    transactionActive = false;
}


BOOST_AUTO_TEST_SUITE_END()
