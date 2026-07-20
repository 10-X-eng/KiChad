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

#include "codex_panel.h"

#include <build_version.h>

#include <algorithm>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/thread.h>
#include <wx/utils.h>


wxDECLARE_EVENT( KICHAD_CODEX_TOOL_COMPLETED, wxThreadEvent );
wxDEFINE_EVENT( KICHAD_CODEX_TOOL_COMPLETED, wxThreadEvent );


namespace
{

using JSON = nlohmann::json;


JSON agentConfig()
{
    return {
        // Keep this narrow capability configuration aligned with VibeCAD's proven app-server
        // integration.  Dotted keys are app-server config overrides, not nested JSON paths.
        { "features.apps", false },
        { "features.browser_use", false },
        { "features.code_mode", true },
        { "features.computer_use", false },
        { "features.enable_mcp_apps", false },
        { "features.goals", true },
        { "features.hooks", false },
        { "features.image_generation", false },
        { "features.multi_agent", false },
        { "features.multi_agent_v2", false },
        { "features.plugins", false },
        { "features.shell_tool", false },
        { "features.skill_mcp_dependency_install", false },
        { "features.tool_suggest", false },
        { "features.unified_exec", false },
        { "features.workspace_dependencies", false },
        { "include_apps_instructions", false },
        { "include_collaboration_mode_instructions", true },
        { "include_environment_context", true },
        { "include_permissions_instructions", true },
        { "mcp_servers", JSON::object() },
        { "orchestrator.mcp.enabled", false },
        { "orchestrator.skills.enabled", false },
        { "project_doc_fallback_filenames", JSON::array() },
        { "project_doc_max_bytes", 0 },
        { "skills.bundled.enabled", false },
        { "skills.include_instructions", false },
        { "tools.experimental_request_user_input.enabled", false },
        { "tools.web_search.context_size", "high" },
        { "web_search", "live" },
    };
}


const char* agentInstructions()
{
    return "You are the KiChad electronics design agent. Use only the native KiChad dynamic "
           "tools advertised by the host for design work. The sole exception is Codex's built-in "
           "live web search, which you must proactively use before selecting or retaining every "
           "component to verify its manufacturer, exact MPN, primary datasheet, lifecycle, and "
           "current orderable availability from real distributors. Cache the exact datasheet "
           "URL, distributor product URL, supplier SKU, reported stock, lifecycle, and verification "
           "date in that component's KDS source form, then run verify with operation sourcing. "
           "Never guess "
           "component facts or treat an unverified component as production-ready. "
           "Treat the versioned project.kicad_kds KiChad Design Script sidecar as the only "
           "representation of design intent: read and edit the exact KDS source, describe the "
           "language when needed, compile before saving, and use native backends to produce KiCad "
           "artifacts. Before a final release, call fabricate plan with the exact compiled KDS "
           "digest and request fabricate export only when its production plan is ready. Never set "
           "allowWaivers unless the user explicitly approved releasing ignored checks or "
           "exclusions. Never create or depend on a separate context projection. Never use shell, "
           "arbitrary code execution, GUI automation, MCP, or direct ad-hoc file rewriting.";
}


const char* agentDeveloperInstructions()
{
    return "Operate only through the supplied native KiChad dynamic tools for electronics "
           "design and Codex live web search for component research. Built-in goals, planning, "
           "and read-only image inspection are available for normal Codex workflows. Do not use "
           "or propose shell execution, general filesystem mutation, plugins, apps, skills, MCP, "
           "browser automation, computer control, or multi-agent delegation. KDS changes must go "
           "through the native design tool, never through generic file editing.";
}


wxString responseErrorMessage( const JSON& aResponse, const wxString& aFallback )
{
    if( !aResponse.contains( "error" ) || !aResponse["error"].is_object() )
        return aFallback;

    const JSON& error = aResponse["error"];

    if( error.contains( "message" ) && error["message"].is_string() )
    {
        const std::string message = error["message"].get<std::string>();

        if( !message.empty() )
            return wxString::FromUTF8( message );
    }

    return aFallback;
}

} // namespace


CODEX_PANEL::CODEX_PANEL( wxWindow* aParent, std::function<wxString()> aProjectPathProvider,
                          SNAPSHOT_PROVIDER aSnapshotProvider, RESTORE_HANDLER aRestoreHandler ) :
        wxPanel( aParent ),
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_snapshotProvider( std::move( aSnapshotProvider ) ),
        m_restoreHandler( std::move( aRestoreHandler ) ),
        m_toolRegistry( m_projectPathProvider,
                        [this]() { return !m_turnSnapshotHash.IsEmpty(); } ),
        m_status( nullptr ),
        m_processStatus( nullptr ),
        m_loginButton( nullptr ),
        m_deviceLoginButton( nullptr ),
        m_cancelLoginButton( nullptr ),
        m_modelChoice( nullptr ),
        m_reasoningChoice( nullptr ),
        m_transcript( nullptr ),
        m_input( nullptr ),
        m_sendButton( nullptr ),
        m_stopButton( nullptr ),
        m_revertButton( nullptr ),
        m_shuttingDown( false ),
        m_nextToolTaskId( 1 ),
        m_initialized( false ),
        m_authenticated( false ),
        m_threadLoaded( false ),
        m_reasoningSummaryOpen( false ),
        m_agentResponseOpen( false )
{
    wxBoxSizer* root = new wxBoxSizer( wxVERTICAL );

    wxBoxSizer* statusRow = new wxBoxSizer( wxHORIZONTAL );
    m_status = new wxStaticText( this, wxID_ANY, _( "Codex is starting..." ) );
    m_loginButton = new wxButton( this, wxID_ANY, _( "Sign in" ) );
    m_deviceLoginButton = new wxButton( this, wxID_ANY, _( "Device code" ) );
    m_cancelLoginButton = new wxButton( this, wxID_ANY, _( "Cancel" ) );
    m_loginButton->Disable();
    m_deviceLoginButton->Disable();
    m_cancelLoginButton->Disable();
    statusRow->Add( m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 8 ) );
    statusRow->Add( m_loginButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    statusRow->Add( m_deviceLoginButton, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );
    statusRow->Add( m_cancelLoginButton, 0, wxALIGN_CENTER_VERTICAL );
    root->Add( statusRow, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_processStatus = new wxStaticText( this, wxID_ANY, _( "Codex service: connecting..." ) );
    root->Add( m_processStatus, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    wxBoxSizer* modelRow = new wxBoxSizer( wxHORIZONTAL );
    m_modelChoice = new wxChoice( this, wxID_ANY );
    m_reasoningChoice = new wxChoice( this, wxID_ANY );
    m_modelChoice->Append( _( "Loading models..." ) );
    m_modelChoice->SetSelection( 0 );
    m_modelChoice->Disable();
    m_reasoningChoice->Append( _( "Default reasoning" ) );
    m_reasoningChoice->SetSelection( 0 );
    m_reasoningChoice->Disable();
    modelRow->Add( m_modelChoice, 1, wxRIGHT, FromDIP( 6 ) );
    modelRow->Add( m_reasoningChoice, 1 );
    root->Add( modelRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    root->Add( new wxStaticLine( this ), 0, wxEXPAND );

    m_transcript = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                   wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_BESTWRAP );
    root->Add( m_transcript, 1, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_input = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              FromDIP( wxSize( -1, 90 ) ), wxTE_MULTILINE | wxTE_PROCESS_ENTER );
    m_input->SetHint( _( "Describe the board you want Codex to design..." ) );
    root->Add( m_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    wxBoxSizer* actionRow = new wxBoxSizer( wxHORIZONTAL );
    m_revertButton = new wxButton( this, wxID_ANY, _( "Revert turn" ) );
    m_stopButton = new wxButton( this, wxID_ANY, _( "Stop" ) );
    m_sendButton = new wxButton( this, wxID_ANY, _( "Send" ) );
    m_revertButton->Disable();
    m_stopButton->Disable();
    m_sendButton->Disable();
    actionRow->Add( m_revertButton );
    actionRow->AddStretchSpacer();
    actionRow->Add( m_stopButton, 0, wxRIGHT, FromDIP( 6 ) );
    actionRow->Add( m_sendButton );
    root->Add( actionRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    SetSizer( root );

    m_loginButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onLogin, this );
    m_deviceLoginButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onDeviceLogin, this );
    m_cancelLoginButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onCancelLogin, this );
    m_sendButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onSend, this );
    m_stopButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onStop, this );
    m_revertButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onRevertTurn, this );
    m_modelChoice->Bind( wxEVT_CHOICE, &CODEX_PANEL::onModelChanged, this );
    Bind( KICHAD_CODEX_TOOL_COMPLETED, &CODEX_PANEL::onToolCompleted, this );

    m_client.SetMessageHandler( [this]( const JSON& aMessage ) { onAppServerMessage( aMessage ); } );
    m_client.SetStateHandler(
            [this]( bool aRunning, const wxString& aDetail )
            {
                onAppServerState( aRunning, aDetail );
            } );

    if( m_client.Start() )
        initializeAppServer();
}


CODEX_PANEL::~CODEX_PANEL()
{
    m_shuttingDown.store( true );
    m_client.SetMessageHandler( {} );
    m_client.SetStateHandler( {} );

    // Synchronize with the worker's final shutdown check before wxPanel destruction begins.
    {
        std::lock_guard<std::mutex> lock( m_toolEventMutex );
    }

    for( auto& entry : m_toolWorkers )
    {
        if( entry.second.joinable() )
            entry.second.join();
    }

    m_toolWorkers.clear();
    m_toolRequestIds.clear();
    DeletePendingEvents();
    Unbind( KICHAD_CODEX_TOOL_COMPLETED, &CODEX_PANEL::onToolCompleted, this );
}


void CODEX_PANEL::initializeAppServer()
{
    JSON params = {
        { "clientInfo",
          { { "name", "kichad" }, { "title", "KiChad" },
            { "version", std::string( GetBuildVersion().ToUTF8() ) } } },
        { "capabilities", { { "experimentalApi", true }, { "requestAttestation", false } } }
    };

    m_client.SendRequest(
            "initialize", params,
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "error" ) )
                {
                    setStatus( _( "Codex protocol initialization failed." ) );
                    return;
                }

                m_initialized = true;
                m_client.SendNotification( "initialized" );
                setStatus( _( "Checking Codex account..." ) );
                readAccount();
                readModels();
            } );
}


void CODEX_PANEL::readAccount( bool aRefreshToken )
{
    m_client.SendRequest(
            "account/read", { { "refreshToken", aRefreshToken } },
            [this]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    setStatus( _( "Could not read the Codex account." ) );
                    setLoginPending( false );
                    return;
                }

                const JSON& account = aResponse["result"].value( "account", JSON() );
                m_authenticated = account.is_object()
                                  && account.value( "type", "" ) == "chatgpt";

                if( m_authenticated )
                {
                    std::string email;

                    if( account.contains( "email" ) && account["email"].is_string() )
                        email = account["email"].get<std::string>();

                    m_loginId.clear();
                    setStatus( email.empty() ? _( "Signed in with ChatGPT" )
                                             : wxString::Format( _( "ChatGPT: %s" ),
                                                                 wxString::FromUTF8( email ) ) );
                    m_loginButton->SetLabel( _( "Sign out" ) );
                    m_sendButton->Enable( m_initialized );
                }
                else
                {
                    setStatus( _( "Sign in with ChatGPT to use Codex." ) );
                    m_loginButton->SetLabel( _( "Sign in" ) );
                    m_sendButton->Disable();
                }

                setLoginPending( false );
            } );
}


void CODEX_PANEL::readModels()
{
    m_client.SendRequest(
            "model/list", { { "includeHidden", false } },
            [this]( const JSON& aResponse )
            {
                m_models.clear();
                m_modelChoice->Clear();

                if( aResponse.contains( "result" ) && aResponse["result"].contains( "data" ) )
                {
                    for( const JSON& model : aResponse["result"]["data"] )
                    {
                        if( !model.value( "hidden", false ) )
                        {
                            m_models.emplace_back( model );
                            m_modelChoice->Append( wxString::FromUTF8( model.value( "displayName",
                                                                                  model.value( "model", "" ) ) ) );
                        }
                    }
                }

                if( m_models.empty() )
                {
                    m_modelChoice->Append( _( "No models available" ) );
                    m_modelChoice->SetSelection( 0 );
                    m_modelChoice->Disable();
                    return;
                }

                size_t defaultIndex = 0;

                for( size_t i = 0; i < m_models.size(); ++i )
                {
                    if( m_models[i].value( "isDefault", false ) )
                    {
                        defaultIndex = i;
                        break;
                    }
                }

                m_modelChoice->SetSelection( static_cast<int>( defaultIndex ) );
                m_modelChoice->Enable();
                updateReasoningChoices();
            } );
}


void CODEX_PANEL::updateReasoningChoices()
{
    m_reasoningChoice->Clear();

    int selection = m_modelChoice->GetSelection();

    if( selection < 0 || static_cast<size_t>( selection ) >= m_models.size() )
    {
        m_reasoningChoice->Append( _( "Default reasoning" ) );
        m_reasoningChoice->SetSelection( 0 );
        m_reasoningChoice->Disable();
        return;
    }

    const JSON& model = m_models[selection];
    std::string defaultEffort = model.value( "defaultReasoningEffort", "" );
    int         defaultSelection = 0;

    for( const JSON& option : model.value( "supportedReasoningEfforts", JSON::array() ) )
    {
        std::string effort = option.value( "reasoningEffort", "" );

        if( effort.empty() )
            continue;

        int index = m_reasoningChoice->Append( wxString::FromUTF8( effort ) );

        if( effort == defaultEffort )
            defaultSelection = index;
    }

    if( m_reasoningChoice->GetCount() == 0 )
        m_reasoningChoice->Append( _( "Default reasoning" ) );

    m_reasoningChoice->SetSelection( defaultSelection );
    m_reasoningChoice->Enable( m_reasoningChoice->GetCount() > 1 );
}


void CODEX_PANEL::ensureThreadLoaded( const wxString& aDisplayedMessage,
                                     std::function<void()> aReadyHandler )
{
    if( m_threadId.empty() )
        startThread( std::move( aReadyHandler ) );
    else if( !m_threadLoaded )
        resumeThread( aDisplayedMessage, std::move( aReadyHandler ) );
    else
        aReadyHandler();
}


void CODEX_PANEL::startThread( std::function<void()> aReadyHandler )
{
    wxString cwd = projectPath();

    JSON params = {
        { "cwd", std::string( cwd.ToUTF8() ) },
        { "runtimeWorkspaceRoots", JSON::array( { std::string( cwd.ToUTF8() ) } ) },
        { "approvalPolicy", "never" },
        { "allowProviderModelFallback", false },
        { "sandbox", "read-only" },
        { "ephemeral", false },
        // The local app-server build advertises paginated history in its experimental schema,
        // but may not include the paginated thread-store backend.  Legacy history is the stable,
        // persistent rollout contract and remains resumable across KiChad launches.
        { "historyMode", "legacy" },
        { "serviceName", "KiChad" },
        { "baseInstructions", agentInstructions() },
        { "developerInstructions", agentDeveloperInstructions() },
        { "config", agentConfig() }
    };

    params["dynamicTools"] = m_toolRegistry.Specs();

    int modelSelection = m_modelChoice->GetSelection();

    if( modelSelection >= 0 && static_cast<size_t>( modelSelection ) < m_models.size() )
        params["model"] = m_models[modelSelection].value( "model", "" );

    m_client.SendRequest(
            "thread/start", params,
            [this, aReadyHandler = std::move( aReadyHandler )]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    appendTranscript( wxS( "\n[" )
                                      + responseErrorMessage(
                                                aResponse,
                                                _( "Could not start a persistent Codex "
                                                   "conversation." ) )
                                      + wxS( "]\n" ) );
                    setBusy( false );
                    return;
                }

                m_threadId = aResponse["result"]["thread"].value( "id", "" );

                if( m_threadId.empty() )
                {
                    appendTranscript( _( "\n[Codex returned no conversation identifier.]\n" ) );
                    setBusy( false );
                    return;
                }

                m_threadLoaded = true;

                wxString saveError;

                if( !m_threadStore.Save( m_threadProjectPath, m_threadId,
                                         CODEX_TOOL_REGISTRY::SCHEMA_VERSION, &saveError ) )
                    appendTranscript( wxString::Format( _( "\n[Conversation persistence: %s]\n" ),
                                                        saveError ) );

                aReadyHandler();
            } );
}


void CODEX_PANEL::resumeThread( const wxString& aDisplayedMessage,
                               std::function<void()> aReadyHandler )
{
    wxString cwd = projectPath();
    JSON params = {
        { "threadId", m_threadId },
        { "cwd", std::string( cwd.ToUTF8() ) },
        { "runtimeWorkspaceRoots", JSON::array( { std::string( cwd.ToUTF8() ) } ) },
        { "approvalPolicy", "never" },
        { "sandbox", "read-only" },
        { "baseInstructions", agentInstructions() },
        { "developerInstructions", agentDeveloperInstructions() },
        { "config", agentConfig() }
    };

    m_client.SendRequest(
            "thread/resume", params,
            [this, aDisplayedMessage,
             aReadyHandler = std::move( aReadyHandler )]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    appendTranscript( wxS( "\n[" )
                                      + responseErrorMessage(
                                                aResponse,
                                                _( "Could not resume the saved Codex "
                                                   "conversation." ) )
                                      + wxS( "]\n" ) );
                    m_threadId.clear();
                    m_threadLoaded = false;
                    setBusy( false );
                    return;
                }

                m_threadLoaded = true;
                const JSON& thread = aResponse["result"].value( "thread", JSON::object() );
                const JSON& turns = thread.value( "turns", JSON::array() );

                if( turns.is_array() )
                {
                    m_transcript->Clear();

                    for( const JSON& turn : turns )
                    {
                        for( const JSON& item : turn.value( "items", JSON::array() ) )
                        {
                            std::string type = item.value( "type", "" );

                            if( type == "userMessage" )
                            {
                                for( const JSON& content : item.value( "content", JSON::array() ) )
                                {
                                    if( content.value( "type", "" ) == "text" )
                                    {
                                        appendTranscript( wxString::Format(
                                                _( "\nYou: %s\n" ),
                                                wxString::FromUTF8( content.value( "text", "" ) ) ) );
                                    }
                                }
                            }
                            else if( type == "agentMessage" )
                            {
                                appendTranscript( wxString::Format(
                                        _( "\nCodex: %s\n" ),
                                        wxString::FromUTF8( item.value( "text", "" ) ) ) );
                            }
                            else if( type == "dynamicToolCall" )
                            {
                                appendTranscript( wxString::Format(
                                        _( "\n[tool: %s — %s]\n" ),
                                        wxString::FromUTF8( item.value( "tool", "" ) ),
                                        item.value( "success", false ) ? _( "success" )
                                                                        : _( "failed" ) ) );
                            }
                        }
                    }

                    // onSend displayed this prompt before the asynchronous resume.  Re-add it
                    // after replacing the transcript with the persisted history.
                    appendTranscript( wxString::Format(
                            _( "\nYou: %s\n" ), aDisplayedMessage ) );
                }

                aReadyHandler();
            } );
}


void CODEX_PANEL::startTurn( const std::string& aMessage )
{
    JSON params = {
        { "threadId", m_threadId },
        { "input", JSON::array( { { { "type", "text" }, { "text", aMessage },
                                     { "text_elements", JSON::array() } } } ) }
    };

    int modelSelection = m_modelChoice->GetSelection();

    if( modelSelection >= 0 && static_cast<size_t>( modelSelection ) < m_models.size() )
        params["model"] = m_models[modelSelection].value( "model", "" );

    int reasoningSelection = m_reasoningChoice->GetSelection();

    if( reasoningSelection >= 0 && m_reasoningChoice->IsEnabled() )
        params["effort"] = std::string( m_reasoningChoice->GetString( reasoningSelection ).ToUTF8() );

    beginTurnDisplay();
    m_client.SendRequest(
            "turn/start", params,
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                    m_turnId = aResponse["result"]["turn"].value( "id", "" );
                else
                {
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage(
                                                aResponse, _( "Codex turn failed to start." ) )
                                      + wxS( "]\n" ) );
                    setBusy( false );
                }
            } );
}


void CODEX_PANEL::beginTurnDisplay()
{
    m_reasoningSummaryOpen = false;
    m_agentResponseOpen = false;
    appendTranscript( _( "\n[Starting Codex turn...]\n" ) );
    setStatus( _( "Starting Codex turn..." ) );
}


void CODEX_PANEL::finishReasoningDisplay()
{
    if( !m_reasoningSummaryOpen )
        return;

    appendTranscript( wxS( "\n" ) );
    m_reasoningSummaryOpen = false;
}


void CODEX_PANEL::appendGoal( const JSON& aGoal )
{
    if( !aGoal.is_object() )
    {
        appendTranscript( _( "[No active goal.]\n" ) );
        return;
    }

    const wxString objective = wxString::FromUTF8( aGoal.value( "objective", "" ) );
    const wxString status = wxString::FromUTF8( aGoal.value( "status", "" ) );
    const long long tokensUsed = aGoal.value( "tokensUsed", 0LL );
    const long long timeUsedSeconds = aGoal.value( "timeUsedSeconds", 0LL );
    wxString usage = wxString::Format( _( "%lld tokens, %lld seconds" ), tokensUsed,
                                       timeUsedSeconds );

    if( aGoal.contains( "tokenBudget" ) && aGoal["tokenBudget"].is_number_integer() )
    {
        usage = wxString::Format( _( "%lld / %lld tokens, %lld seconds" ), tokensUsed,
                                  aGoal["tokenBudget"].get<long long>(), timeUsedSeconds );
    }

    appendTranscript( wxString::Format( _( "[Goal — %s: %s (%s)]\n" ), status, objective,
                                        usage ) );
}


void CODEX_PANEL::showGoal()
{
    m_client.SendRequest(
            "thread/goal/get", { { "threadId", m_threadId } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                    appendGoal( aResponse["result"].value( "goal", JSON() ) );
                else
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage( aResponse,
                                                              _( "Could not read the goal." ) )
                                      + wxS( "]\n" ) );

                setBusy( !m_turnId.empty() );
            } );
}


void CODEX_PANEL::setGoal( const wxString& aObjective, bool aActivate )
{
    JSON params = { { "threadId", m_threadId },
                    { "objective", std::string( aObjective.ToUTF8() ) } };

    if( aActivate )
        params["status"] = "active";

    m_client.SendRequest(
            "thread/goal/set", params,
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    const JSON& goal = aResponse["result"].value( "goal", JSON() );
                    appendGoal( goal );

                    if( goal.value( "status", "" ) == "active" && m_turnId.empty() )
                        setStatus( _( "Goal active; starting Codex..." ) );
                    else
                        setBusy( !m_turnId.empty() );
                }
                else
                {
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage( aResponse,
                                                              _( "Could not set the goal." ) )
                                      + wxS( "]\n" ) );
                    setBusy( !m_turnId.empty() );
                }
            } );
}


void CODEX_PANEL::setGoalStatus( const std::string& aStatus )
{
    m_client.SendRequest(
            "thread/goal/set", { { "threadId", m_threadId }, { "status", aStatus } },
            [this, aStatus]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    appendGoal( aResponse["result"].value( "goal", JSON() ) );

                    if( aStatus == "active" && m_turnId.empty() )
                        setStatus( _( "Goal active; starting Codex..." ) );
                    else
                        setBusy( !m_turnId.empty() );
                }
                else
                {
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage(
                                                aResponse, _( "Could not update the goal." ) )
                                      + wxS( "]\n" ) );
                    setBusy( !m_turnId.empty() );
                }
            } );
}


void CODEX_PANEL::clearGoal()
{
    m_client.SendRequest(
            "thread/goal/clear", { { "threadId", m_threadId } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                    appendTranscript( aResponse["result"].value( "cleared", false )
                                              ? _( "[Goal cleared.]\n" )
                                              : _( "[No active goal.]\n" ) );
                else
                    appendTranscript( wxS( "[" )
                                      + responseErrorMessage( aResponse,
                                                              _( "Could not clear the goal." ) )
                                      + wxS( "]\n" ) );

                setBusy( !m_turnId.empty() );
            } );
}


bool CODEX_PANEL::handleGoalCommand( const wxString& aMessage )
{
    if( aMessage != wxS( "/goal" ) && !aMessage.StartsWith( wxS( "/goal " ) ) )
        return false;

    wxString arguments = aMessage.Mid( 5 );
    arguments.Trim( true ).Trim( false );
    wxString command = arguments.BeforeFirst( ' ' ).Lower();
    wxString objective = arguments.AfterFirst( ' ' );
    objective.Trim( true ).Trim( false );

    enum class ACTION
    {
        SHOW,
        SET,
        EDIT,
        PAUSE,
        RESUME,
        CLEAR
    };

    ACTION action = ACTION::SHOW;

    if( arguments.IsEmpty() )
        action = ACTION::SHOW;
    else if( command == wxS( "clear" ) )
        action = ACTION::CLEAR;
    else if( command == wxS( "pause" ) )
        action = ACTION::PAUSE;
    else if( command == wxS( "resume" ) )
        action = ACTION::RESUME;
    else if( command == wxS( "edit" ) )
    {
        if( objective.IsEmpty() )
        {
            appendTranscript( _( "[Usage: /goal edit <objective>]\n" ) );
            return true;
        }

        action = ACTION::EDIT;
    }
    else
    {
        objective = arguments;
        action = ACTION::SET;
    }

    if( m_threadId.empty() && action != ACTION::SET )
    {
        appendTranscript( _( "[No active goal.]\n" ) );
        return true;
    }

    setBusy( true );
    ensureThreadLoaded(
            aMessage,
            [this, action, objective]()
            {
                switch( action )
                {
                case ACTION::SHOW:
                    showGoal();
                    break;

                case ACTION::SET:
                    setGoal( objective, true );
                    break;

                case ACTION::EDIT:
                    setGoal( objective, false );
                    break;

                case ACTION::PAUSE:
                    setGoalStatus( "paused" );
                    break;

                case ACTION::RESUME:
                    setGoalStatus( "active" );
                    break;

                case ACTION::CLEAR:
                    clearGoal();
                    break;
                }
            } );
    return true;
}


void CODEX_PANEL::appendTranscript( const wxString& aText )
{
    m_transcript->AppendText( aText );
    m_transcript->ShowPosition( m_transcript->GetLastPosition() );
}


void CODEX_PANEL::setBusy( bool aBusy )
{
    // Goal control remains available while Codex is working so users can issue /goal pause or
    // /goal clear without first interrupting the current goal turn.
    m_sendButton->Enable( m_initialized && m_authenticated );
    m_stopButton->Enable( aBusy );
    m_revertButton->Enable( !aBusy && !m_turnSnapshotHash.IsEmpty() );
    m_input->Enable();
}


void CODEX_PANEL::setLoginPending( bool aPending )
{
    const bool ready = m_initialized && m_client.IsRunning();
    m_loginButton->Enable( ready && !aPending );
    m_deviceLoginButton->Enable( ready && !aPending && !m_authenticated );
    m_cancelLoginButton->Enable( ready && aPending && !m_loginId.empty() );
}


void CODEX_PANEL::setStatus( const wxString& aStatus )
{
    m_status->SetLabel( aStatus );
    Layout();
}


wxString CODEX_PANEL::projectPath() const
{
    wxString path = m_projectPathProvider ? m_projectPathProvider() : wxString();

    if( path.IsEmpty() || !wxFileName::DirExists( path ) )
        path = wxGetCwd();

    return path;
}


void CODEX_PANEL::selectProjectThread()
{
    wxString activePath = projectPath();

    if( activePath == m_threadProjectPath )
        return;

    m_threadProjectPath = activePath;
    m_threadId = m_threadStore.Load( activePath, CODEX_TOOL_REGISTRY::SCHEMA_VERSION );
    m_turnId.clear();
    m_turnSnapshotHash.clear();
    m_revertButton->Disable();
    m_threadLoaded = false;

    if( !m_threadId.empty() )
        setStatus( _( "Persistent Codex conversation ready." ) );
}


void CODEX_PANEL::onAppServerMessage( const JSON& aMessage )
{
    const std::string method = aMessage.value( "method", "" );

    if( method == "account/updated" )
    {
        // Current app-server versions publish the authoritative authentication transition on
        // account/updated.  Always re-read the owned credential store instead of trying to build
        // an incomplete account from the notification's authMode and planType summary.
        readAccount();
        readModels();
    }
    else if( method == "account/login/completed" )
    {
        const JSON& params = aMessage.value( "params", JSON::object() );
        const std::string eventLoginId =
                params.contains( "loginId" ) && params["loginId"].is_string()
                        ? params["loginId"].get<std::string>()
                        : std::string();

        if( !m_loginId.empty() && !eventLoginId.empty() && eventLoginId != m_loginId )
            return;

        if( !params.value( "success", false ) )
        {
            const std::string error = params.contains( "error" ) && params["error"].is_string()
                                              ? params["error"].get<std::string>()
                                              : std::string();
            setStatus( error.empty() ? _( "ChatGPT sign-in failed." )
                                     : wxString::FromUTF8( error ) );
            m_loginId.clear();
            setLoginPending( false );
            return;
        }

        // Match the working VibeCAD flow: force one refresh after successful OAuth before
        // enabling inference, then reload the account-scoped model catalog.
        m_cancelLoginButton->Disable();
        readAccount( true );
        readModels();
    }
    else if( method == "item/agentMessage/delta" )
    {
        finishReasoningDisplay();

        if( !m_agentResponseOpen )
        {
            appendTranscript( _( "\nCodex: " ) );
            m_agentResponseOpen = true;
        }

        appendTranscript( wxString::FromUTF8( aMessage["params"].value( "delta", "" ) ) );
        setStatus( _( "Codex is responding..." ) );
    }
    else if( method == "item/reasoning/summaryPartAdded" )
    {
        finishReasoningDisplay();
        setStatus( _( "Codex is thinking..." ) );
    }
    else if( method == "item/reasoning/summaryTextDelta" )
    {
        if( !m_reasoningSummaryOpen )
        {
            appendTranscript( _( "\nThinking: " ) );
            m_reasoningSummaryOpen = true;
        }

        appendTranscript( wxString::FromUTF8( aMessage["params"].value( "delta", "" ) ) );
        setStatus( _( "Codex is thinking..." ) );
    }
    else if( method == "item/reasoning/textDelta" )
    {
        // Raw hidden reasoning is intentionally not rendered.  The app-server's supported
        // reasoning summaries arrive through item/reasoning/summaryTextDelta above.
        setStatus( _( "Codex is thinking..." ) );
    }
    else if( method == "item/started" )
    {
        const JSON& item = aMessage["params"]["item"];
        const std::string type = item.value( "type", "" );

        if( type == "reasoning" )
        {
            setStatus( _( "Codex is thinking..." ) );
        }
        else if( type == "dynamicToolCall" )
        {
            finishReasoningDisplay();
            const wxString tool = wxString::FromUTF8( item.value( "tool", "" ) );
            appendTranscript( wxString::Format( _( "\n[tool: %s — started]\n" ),
                                                tool ) );
            setStatus( wxString::Format( _( "Running KiChad tool: %s..." ), tool ) );
        }
        else if( type == "webSearch" )
        {
            finishReasoningDisplay();
            const wxString query = wxString::FromUTF8( item.value( "query", "" ) );
            appendTranscript( query.IsEmpty()
                                      ? _( "\n[Web research started.]\n" )
                                      : wxString::Format( _( "\n[Web research: %s]\n" ), query ) );
            setStatus( _( "Codex is researching the web..." ) );
        }
        else if( type == "imageView" )
        {
            finishReasoningDisplay();
            appendTranscript( wxString::Format( _( "\n[Viewing image: %s]\n" ),
                                                wxString::FromUTF8( item.value( "path", "" ) ) ) );
            setStatus( _( "Codex is inspecting an image..." ) );
        }
        else if( type == "contextCompaction" )
        {
            finishReasoningDisplay();
            appendTranscript( _( "\n[Codex is compacting conversation context...]\n" ) );
            setStatus( _( "Codex is compacting context..." ) );
        }
    }
    else if( method == "item/completed" )
    {
        const JSON& item = aMessage["params"]["item"];
        const std::string type = item.value( "type", "" );

        if( type == "reasoning" )
        {
            finishReasoningDisplay();
            setStatus( _( "Codex finished thinking; continuing..." ) );
        }
        else if( type == "dynamicToolCall" )
        {
            const wxString tool = wxString::FromUTF8( item.value( "tool", "" ) );
            const wxString status = wxString::FromUTF8( item.value( "status", "" ) );
            const long long durationMs = item.contains( "durationMs" )
                                                         && item["durationMs"].is_number_integer()
                                                 ? item["durationMs"].get<long long>()
                                                 : 0LL;
            const wxString duration = durationMs > 0
                                              ? wxString::Format( _( ", %lld ms" ), durationMs )
                                              : wxString();
            appendTranscript( wxString::Format( _( "[tool: %s — %s%s]\n" ), tool, status,
                                                duration ) );
            setStatus( status == wxS( "failed" )
                               ? wxString::Format( _( "KiChad tool failed: %s" ), tool )
                               : _( "Tool finished; Codex is continuing..." ) );
        }
        else if( type == "webSearch" )
        {
            appendTranscript( _( "[Web research completed.]\n" ) );
            setStatus( _( "Research finished; Codex is continuing..." ) );
        }
        else if( type == "imageView" )
        {
            appendTranscript( _( "[Image inspection completed.]\n" ) );
            setStatus( _( "Image inspected; Codex is continuing..." ) );
        }
        else if( type == "agentMessage" )
        {
            if( m_agentResponseOpen )
                appendTranscript( wxS( "\n" ) );

            m_agentResponseOpen = false;
        }
    }
    else if( method == "thread/status/changed" )
    {
        const JSON& status = aMessage["params"].value( "status", JSON::object() );
        const std::string type = status.value( "type", "" );

        if( type == "systemError" )
        {
            appendTranscript( _( "\n[Codex thread entered a system-error state.]\n" ) );
            setStatus( _( "Codex thread error" ) );
        }
        else if( type == "active" )
        {
            const JSON& flags = status.value( "activeFlags", JSON::array() );

            if( flags.is_array()
                && std::find( flags.begin(), flags.end(), "waitingOnUserInput" ) != flags.end() )
            {
                setStatus( _( "Codex is waiting for your input." ) );
            }
            else if( flags.is_array()
                     && std::find( flags.begin(), flags.end(), "waitingOnApproval" )
                                != flags.end() )
            {
                setStatus( _( "Codex is waiting for approval." ) );
            }
        }
    }
    else if( method == "turn/started" )
    {
        m_turnId = aMessage["params"]["turn"].value( "id", "" );
        m_reasoningSummaryOpen = false;
        m_agentResponseOpen = false;
        appendTranscript( _( "[Codex turn started.]\n" ) );
        setStatus( _( "Codex is working..." ) );
        setBusy( true );
    }
    else if( method == "turn/completed" )
    {
        finishReasoningDisplay();

        if( m_agentResponseOpen )
            appendTranscript( wxS( "\n" ) );

        const JSON& turn = aMessage["params"].value( "turn", JSON::object() );
        const std::string status = turn.value( "status", "" );
        const long long durationMs = turn.contains( "durationMs" )
                                             && turn["durationMs"].is_number_integer()
                                     ? turn["durationMs"].get<long long>()
                                     : 0LL;
        const wxString duration = durationMs > 0
                                          ? wxString::Format( _( " in %.1f seconds" ),
                                                              durationMs / 1000.0 )
                                          : wxString();

        if( status == "completed" )
        {
            appendTranscript( wxString::Format( _( "[Turn completed%s.]\n" ), duration ) );
            setStatus( _( "Codex is ready." ) );
        }
        else if( status == "interrupted" )
        {
            appendTranscript( wxString::Format( _( "[Turn interrupted%s.]\n" ), duration ) );
            setStatus( _( "Codex turn interrupted." ) );
        }
        else
        {
            const wxString error = responseErrorMessage( turn, _( "Unknown Codex turn error." ) );
            appendTranscript( wxString::Format( _( "[Turn failed%s: %s]\n" ), duration,
                                                error ) );
            setStatus( wxString::Format( _( "Codex turn failed: %s" ), error ) );
        }

        m_agentResponseOpen = false;
        m_turnId.clear();
        setBusy( false );
    }
    else if( method == "error" )
    {
        finishReasoningDisplay();
        const JSON& params = aMessage.value( "params", JSON::object() );
        const wxString error = responseErrorMessage( params, _( "Unknown Codex error." ) );
        const bool willRetry = params.value( "willRetry", false );
        appendTranscript( wxString::Format( willRetry ? _( "\n[Codex error; retrying: %s]\n" )
                                                        : _( "\n[Codex error: %s]\n" ),
                                            error ) );
        setStatus( willRetry ? wxString::Format( _( "Codex error; retrying: %s" ), error )
                             : wxString::Format( _( "Codex error: %s" ), error ) );
    }
    else if( method == "item/tool/call" && aMessage.contains( "id" ) )
    {
        if( !aMessage.contains( "params" ) || !aMessage["params"].is_object()
            || !aMessage["params"].contains( "tool" )
            || !aMessage["params"]["tool"].is_string() )
        {
            m_client.SendError( aMessage["id"], -32602,
                                "Native KiChad tool parameters are invalid" );
            appendTranscript( _( "[tool result: invalid request]\n" ) );
            return;
        }

        const JSON& params = aMessage["params"];
        std::string tool = params.value( "tool", "" );
        JSON arguments = params.value( "arguments", JSON::object() );
        appendTranscript( wxString::Format( _( "\n[tool request: %s %s]\n" ),
                                            wxString::FromUTF8( tool ),
                                            wxString::FromUTF8( arguments.dump() ) ) );

        // Serialize design operations so concurrent model calls cannot observe or mutate
        // partially overlapping project state.
        if( !m_toolWorkers.empty() )
        {
            m_client.SendError( aMessage["id"], -32001,
                                "Another native KiChad tool call is still running" );
            appendTranscript( _( "[tool result: executor busy]\n" ) );
            return;
        }

        const int taskId = m_nextToolTaskId++;
        JSON      requestId = aMessage["id"];
        wxString  activeProject =
                m_projectPathProvider ? m_projectPathProvider() : wxString();
        bool mutationAvailable = !m_turnSnapshotHash.IsEmpty();
        bool finalActionApproved = false;

        if( CODEX_TOOL_REGISTRY::RequiresFinalConfirmation( tool, arguments ) )
        {
            wxString confirmationMessage =
                    _( "Codex is requesting a final fabrication export. KiChad will rerun ERC, "
                       "DRC, and sourcing gates, then atomically replace the project's "
                       "fabrication output directory only if every requested artifact validates.\n\n" );

            if( arguments.is_object() && arguments.contains( "allowWaivers" )
                && arguments["allowWaivers"].is_boolean()
                && arguments["allowWaivers"].get<bool>() )
            {
                confirmationMessage +=
                        _( "This request also authorizes release when ERC or DRC contains "
                           "ignored checks or exclusions; the manifest will mark the package "
                           "as waived.\n\n" );
            }

            confirmationMessage += _( "Allow this export?" );
            wxMessageDialog confirmation(
                    this, confirmationMessage, _( "Confirm fabrication export" ),
                    wxYES_NO | wxNO_DEFAULT | wxICON_WARNING );
            finalActionApproved = confirmation.ShowModal() == wxID_YES;
        }

        try
        {
            auto worker = m_toolWorkers.try_emplace( taskId ).first;
            m_toolRequestIds.emplace( taskId, requestId );

            worker->second = std::thread(
                            [this, taskId, tool = std::move( tool ),
                             arguments = std::move( arguments ),
                             activeProject = std::move( activeProject ), mutationAvailable,
                             finalActionApproved]()
                            {
                                std::string serialized;

                                try
                                {
                                    JSON result = m_toolRegistry.HandleWithContext(
                                            tool, arguments, activeProject, mutationAvailable,
                                            wxString(), finalActionApproved );
                                    serialized = result.dump();
                                }
                                catch( const std::exception& )
                                {
                                    JSON error = { { "ok", false },
                                                   { "error",
                                                     { { "code", "tool_failed" },
                                                       { "message",
                                                         "Native tool result could not be serialized" } } } };
                                    JSON result = {
                                        { "contentItems",
                                          JSON::array( { { { "type", "inputText" },
                                                           { "text", error.dump() } } } ) },
                                        { "success", false }
                                    };
                                    serialized = result.dump();
                                }

                                std::lock_guard<std::mutex> lock( m_toolEventMutex );

                                if( !m_shuttingDown.load() )
                                {
                                    wxThreadEvent* event =
                                            new wxThreadEvent( KICHAD_CODEX_TOOL_COMPLETED );
                                    event->SetInt( taskId );
                                    event->SetString( wxString::FromUTF8( serialized.data(),
                                                                         serialized.size() ) );
                                    wxQueueEvent( this, event );
                                }
                            } );
        }
        catch( const std::exception& error )
        {
            m_toolWorkers.erase( taskId );
            m_toolRequestIds.erase( taskId );
            m_client.SendError( aMessage["id"], -32000,
                                std::string( "Could not start native tool worker: " ) + error.what() );
            appendTranscript( _( "[tool result: failed to start worker]\n" ) );
        }
    }
}


void CODEX_PANEL::onToolCompleted( wxThreadEvent& aEvent )
{
    JSON requestId;
    auto request = m_toolRequestIds.find( aEvent.GetInt() );

    if( request != m_toolRequestIds.end() )
    {
        requestId = std::move( request->second );
        m_toolRequestIds.erase( request );
    }

    auto worker = m_toolWorkers.find( aEvent.GetInt() );

    if( worker != m_toolWorkers.end() )
    {
        if( worker->second.joinable() )
            worker->second.join();

        m_toolWorkers.erase( worker );
    }

    try
    {
        JSON result = JSON::parse( std::string( aEvent.GetString().ToUTF8() ) );

        if( requestId.is_null() )
        {
            appendTranscript( _( "[tool result could not be delivered: request ID was lost]\n" ) );
            return;
        }

        m_client.SendResponse( requestId, result );
        appendTranscript( wxString::Format( _( "[tool result: %s]\n" ),
                                            result.value( "success", false ) ? _( "success" )
                                                                             : _( "failed" ) ) );
        setStatus( result.value( "success", false )
                           ? _( "Tool result delivered; Codex is continuing..." )
                           : _( "KiChad tool failed; Codex is handling the error..." ) );
    }
    catch( const JSON::exception& error )
    {
        if( !requestId.is_null() )
            m_client.SendError( requestId, -32000, "Native KiChad tool result could not be decoded" );

        appendTranscript( wxString::Format( _( "[tool result could not be decoded: %s]\n" ),
                                            wxString::FromUTF8( error.what() ) ) );
    }
}


void CODEX_PANEL::onAppServerState( bool aRunning, const wxString& aDetail )
{
    setStatus( aDetail );

    if( aRunning )
    {
        m_processStatus->SetLabel( _( "Codex service: connected" ) );
    }
    else
    {
        m_processStatus->SetLabel( _( "Codex service: unavailable" ) );
    }

    if( !aRunning )
    {
        finishReasoningDisplay();

        if( m_agentResponseOpen )
            appendTranscript( wxS( "\n" ) );

        appendTranscript( wxString::Format( _( "\n[Codex service unavailable: %s]\n" ),
                                            aDetail ) );
        m_agentResponseOpen = false;
        m_turnId.clear();
        m_initialized = false;
        m_authenticated = false;
        m_loginButton->Disable();
        m_deviceLoginButton->Disable();
        m_cancelLoginButton->Disable();
        m_sendButton->Disable();
        m_stopButton->Disable();
    }
}


void CODEX_PANEL::onLogin( wxCommandEvent& aEvent )
{
    if( m_authenticated )
    {
        setLoginPending( true );
        m_client.SendRequest( "account/logout", JSON::object(),
                              [this]( const JSON& ) { readAccount(); } );
        return;
    }

    m_loginId.clear();
    setLoginPending( true );
    setStatus( _( "Starting ChatGPT sign-in..." ) );
    m_client.SendRequest(
            "account/login/start", { { "type", "chatgpt" },
                                     { "useHostedLoginSuccessPage", true },
                                     { "appBrand", "chatgpt" } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    const JSON& result = aResponse["result"];
                    std::string authUrl = result.value( "authUrl", "" );
                    std::string loginId = result.value( "loginId", "" );

                    if( result.value( "type", "" ) == "chatgpt" && !authUrl.empty()
                        && !loginId.empty() )
                    {
                        m_loginId = std::move( loginId );
                        setLoginPending( true );
                        wxLaunchDefaultBrowser( wxString::FromUTF8( authUrl ) );
                        setStatus( _( "Complete ChatGPT sign-in in your browser..." ) );
                        return;
                    }
                }

                setStatus( responseErrorMessage(
                        aResponse, _( "Could not start ChatGPT sign-in." ) ) );
                m_loginId.clear();
                setLoginPending( false );
            } );
}


void CODEX_PANEL::onDeviceLogin( wxCommandEvent& aEvent )
{
    if( m_authenticated )
        return;

    m_loginId.clear();
    setLoginPending( true );
    setStatus( _( "Requesting a ChatGPT device code..." ) );
    m_client.SendRequest(
            "account/login/start", { { "type", "chatgptDeviceCode" } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    const JSON& result = aResponse["result"];
                    std::string loginId = result.value( "loginId", "" );
                    std::string verificationUrl = result.value( "verificationUrl", "" );
                    std::string userCode = result.value( "userCode", "" );

                    if( result.value( "type", "" ) == "chatgptDeviceCode"
                        && !loginId.empty() && !verificationUrl.empty() && !userCode.empty() )
                    {
                        m_loginId = std::move( loginId );
                        setLoginPending( true );

                        const wxString url = wxString::FromUTF8( verificationUrl );
                        const wxString code = wxString::FromUTF8( userCode );
                        wxLaunchDefaultBrowser( url );
                        setStatus( wxString::Format( _( "Enter device code %s" ), code ) );
                        appendTranscript( wxString::Format(
                                _( "\n[ChatGPT device sign-in: open %s and enter code %s.]\n" ),
                                url, code ) );
                        return;
                    }
                }

                setStatus( responseErrorMessage(
                        aResponse, _( "Could not request a ChatGPT device code." ) ) );
                m_loginId.clear();
                setLoginPending( false );
            } );
}


void CODEX_PANEL::onCancelLogin( wxCommandEvent& aEvent )
{
    if( m_loginId.empty() )
        return;

    const std::string loginId = m_loginId;
    m_cancelLoginButton->Disable();
    setStatus( _( "Cancelling ChatGPT sign-in..." ) );
    m_client.SendRequest(
            "account/login/cancel", { { "loginId", loginId } },
            [this, loginId]( const JSON& aResponse )
            {
                if( m_loginId == loginId )
                    m_loginId.clear();

                if( aResponse.contains( "result" ) )
                    setStatus( _( "ChatGPT sign-in cancelled." ) );
                else
                    setStatus( responseErrorMessage(
                            aResponse, _( "Could not cancel ChatGPT sign-in." ) ) );

                setLoginPending( false );
            } );
}


void CODEX_PANEL::onSend( wxCommandEvent& aEvent )
{
    selectProjectThread();

    wxString message = m_input->GetValue();
    message.Trim( true ).Trim( false );

    if( message.IsEmpty() )
        return;

    m_input->Clear();
    appendTranscript( wxString::Format( _( "\nYou: %s\n" ), message ) );

    if( handleGoalCommand( message ) )
        return;

    if( !m_turnId.empty() )
    {
        appendTranscript( _( "[Codex is still working. Use /goal pause, /goal clear, or Stop "
                             "before starting another request.]\n" ) );
        return;
    }

    m_turnSnapshotHash.clear();

    if( m_snapshotProvider )
    {
        m_turnSnapshotHash = m_snapshotProvider( _( "Before Codex turn" ) );

        const wxString activeProject =
                m_projectPathProvider ? m_projectPathProvider() : wxString();

        if( m_turnSnapshotHash.IsEmpty() && !activeProject.IsEmpty() )
        {
            appendTranscript( _( "[A turn snapshot could not be created; mutating tools will "
                                 "remain unavailable.]\n" ) );
        }
    }

    setBusy( true );

    std::string utf8Message( message.ToUTF8() );
    ensureThreadLoaded( message, [this, utf8Message]() { startTurn( utf8Message ); } );
}


void CODEX_PANEL::onStop( wxCommandEvent& aEvent )
{
    if( m_threadId.empty() || m_turnId.empty() )
        return;

    m_client.SendRequest( "turn/interrupt", { { "threadId", m_threadId }, { "turnId", m_turnId } } );
    setStatus( _( "Stopping Codex turn..." ) );
}


void CODEX_PANEL::onRevertTurn( wxCommandEvent& aEvent )
{
    if( m_turnSnapshotHash.IsEmpty() || !m_restoreHandler )
        return;

    const wxString snapshot = m_turnSnapshotHash;
    m_revertButton->Disable();

    if( m_restoreHandler( snapshot ) )
    {
        appendTranscript( _( "\n[Reverted the project to its pre-turn snapshot.]\n" ) );
        m_turnSnapshotHash.clear();
    }
    else
    {
        appendTranscript( _( "\n[The pre-turn snapshot was not restored.]\n" ) );
        m_revertButton->Enable();
    }
}


void CODEX_PANEL::onModelChanged( wxCommandEvent& aEvent )
{
    updateReasoningChoices();
}
