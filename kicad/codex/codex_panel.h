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

#ifndef KICHAD_CODEX_PANEL_H
#define KICHAD_CODEX_PANEL_H

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/panel.h>

#include "codex_app_server_client.h"
#include "codex_thread_store.h"
#include "codex_tool_registry.h"


class wxButton;
class wxChoice;
class wxStaticText;
class wxTextCtrl;
class wxThreadEvent;


/** Native docked Codex conversation surface owned by the KiChad project manager. */
class CODEX_PANEL : public wxPanel
{
public:
    using SNAPSHOT_PROVIDER = std::function<wxString( const wxString& )>;
    using RESTORE_HANDLER = std::function<bool( const wxString& )>;

    explicit CODEX_PANEL( wxWindow* aParent, std::function<wxString()> aProjectPathProvider,
                          SNAPSHOT_PROVIDER aSnapshotProvider, RESTORE_HANDLER aRestoreHandler );
    ~CODEX_PANEL() override;

private:
    using JSON = nlohmann::json;

    void initializeAppServer();
    void readAccount( bool aRefreshToken = false );
    void readModels();
    void updateReasoningChoices();
    void startThreadAndTurn( const std::string& aMessage );
    void resumeThreadAndTurn( const std::string& aMessage );
    void startTurn( const std::string& aMessage );
    void selectProjectThread();
    void appendTranscript( const wxString& aText );
    void setBusy( bool aBusy );
    void setLoginPending( bool aPending );
    void setStatus( const wxString& aStatus );
    wxString projectPath() const;

    void onAppServerMessage( const JSON& aMessage );
    void onAppServerState( bool aRunning, const wxString& aDetail );
    void onToolCompleted( wxThreadEvent& aEvent );
    void onLogin( wxCommandEvent& aEvent );
    void onDeviceLogin( wxCommandEvent& aEvent );
    void onCancelLogin( wxCommandEvent& aEvent );
    void onSend( wxCommandEvent& aEvent );
    void onStop( wxCommandEvent& aEvent );
    void onRevertTurn( wxCommandEvent& aEvent );
    void onModelChanged( wxCommandEvent& aEvent );

    std::function<wxString()> m_projectPathProvider;
    SNAPSHOT_PROVIDER         m_snapshotProvider;
    RESTORE_HANDLER           m_restoreHandler;
    CODEX_TOOL_REGISTRY       m_toolRegistry;
    CODEX_THREAD_STORE        m_threadStore;
    CODEX_APP_SERVER_CLIENT   m_client;
    wxStaticText*             m_status;
    wxStaticText*             m_processStatus;
    wxButton*                 m_loginButton;
    wxButton*                 m_deviceLoginButton;
    wxButton*                 m_cancelLoginButton;
    wxChoice*                 m_modelChoice;
    wxChoice*                 m_reasoningChoice;
    wxTextCtrl*               m_transcript;
    wxTextCtrl*               m_input;
    wxButton*                 m_sendButton;
    wxButton*                 m_stopButton;
    wxButton*                 m_revertButton;
    std::vector<JSON>         m_models;
    std::string               m_threadId;
    std::string               m_loginId;
    wxString                  m_threadProjectPath;
    std::string               m_turnId;
    wxString                  m_turnSnapshotHash;
    std::map<int, std::thread> m_toolWorkers;
    std::map<int, JSON>       m_toolRequestIds;
    std::mutex                m_toolEventMutex;
    std::atomic<bool>         m_shuttingDown;
    int                       m_nextToolTaskId;
    bool                      m_initialized;
    bool                      m_authenticated;
    bool                      m_threadLoaded;
};

#endif // KICHAD_CODEX_PANEL_H
