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

#include "kds_editor_panel.h"

#include "codex_tool_internal.h"
#include "design_script_compiler.h"

#include <algorithm>
#include <utility>

#include <picosha2.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/wupdlock.h>


namespace
{

constexpr size_t MAX_KDS_BYTES = 16 * 1024 * 1024;
constexpr int ERROR_MARKER = 0;


std::string sha256( const std::string& aSource )
{
    std::string digest;
    picosha2::hash256_hex_string( aSource, digest );
    return digest;
}


wxString abbreviatedSha( const std::string& aDigest )
{
    return wxString::FromUTF8( aDigest.substr( 0, std::min<size_t>( 12, aDigest.size() ) ) );
}

} // namespace


KDS_EDITOR_PANEL::KDS_EDITOR_PANEL( wxWindow* aParent, const wxFileName& aFile,
                                    SNAPSHOT_PROVIDER aSnapshotProvider ) :
        PANEL_NOTEBOOK_BASE( aParent ),
        m_file( aFile ),
        m_filePath( aFile.GetFullPath() ),
        m_snapshotProvider( std::move( aSnapshotProvider ) ),
        m_editor( nullptr ),
        m_diagnostics( nullptr ),
        m_status( nullptr ),
        m_compileButton( nullptr ),
        m_saveButton( nullptr )
{
    SetProjectTied( true );
    SetClosable( true );

    wxBoxSizer* root = new wxBoxSizer( wxVERTICAL );
    wxBoxSizer* actions = new wxBoxSizer( wxHORIZONTAL );
    m_compileButton = new wxButton( this, wxID_ANY, _( "Compile" ) );
    m_saveButton = new wxButton( this, wxID_SAVE, _( "Save" ) );
    m_status = new wxStaticText( this, wxID_ANY, _( "Loading KDS..." ) );
    actions->Add( m_compileButton, 0, wxRIGHT, FromDIP( 6 ) );
    actions->Add( m_saveButton, 0, wxRIGHT, FromDIP( 10 ) );
    actions->Add( m_status, 1, wxALIGN_CENTER_VERTICAL );
    root->Add( actions, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_editor = new wxStyledTextCtrl( this, wxID_ANY );
    m_editor->SetLexer( wxSTC_LEX_LISP );
    m_editor->SetKeyWords(
            0,
            wxS( "kichad_design version project metadata libraries symbol footprint schematic "
                 "board components component nets net connect place route via zone outline rules "
                 "netclass stackup sourcing verify outputs output fabrication gerber drill bom "
                 "position step dnp variant title_block text_variables field_templates" ) );
    m_editor->SetTabWidth( 2 );
    m_editor->SetUseTabs( false );
    m_editor->SetIndent( 2 );
    m_editor->SetViewWhiteSpace( wxSTC_WS_INVISIBLE );
    m_editor->SetMarginType( 0, wxSTC_MARGIN_NUMBER );
    m_editor->SetMarginWidth( 0, FromDIP( 48 ) );
    m_editor->SetWrapMode( wxSTC_WRAP_NONE );
    m_editor->SetCaretLineVisible( true );
    m_editor->MarkerDefine( ERROR_MARKER, wxSTC_MARK_CIRCLE, *wxWHITE, *wxRED );
    root->Add( m_editor, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 8 ) );

    m_diagnostics = new wxTextCtrl(
            this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP( wxSize( -1, 120 ) ),
            wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_DONTWRAP );
    m_diagnostics->SetHint( _( "Compiler diagnostics" ) );
    root->Add( m_diagnostics, 0, wxEXPAND | wxALL, FromDIP( 8 ) );
    SetSizer( root );

    m_compileButton->Bind( wxEVT_BUTTON, &KDS_EDITOR_PANEL::onCompile, this );
    m_saveButton->Bind( wxEVT_BUTTON, &KDS_EDITOR_PANEL::onSave, this );
    m_editor->Bind( wxEVT_STC_CHANGE, &KDS_EDITOR_PANEL::onEditorChanged, this );
    Bind( wxEVT_CHAR_HOOK, &KDS_EDITOR_PANEL::onCharHook, this );

    if( load() )
        compile();
}


std::string KDS_EDITOR_PANEL::sourceUtf8() const
{
    const wxScopedCharBuffer utf8 = m_editor->GetText().ToUTF8();

    if( !utf8.data() )
        return {};

    return std::string( utf8.data(), utf8.length() );
}


bool KDS_EDITOR_PANEL::load()
{
    std::string source;
    std::string error;

    if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( m_file, source, error ) )
    {
        m_status->SetLabel( _( "Could not load KDS" ) );
        m_diagnostics->SetValue( wxString::FromUTF8( error ) );
        m_editor->SetReadOnly( true );
        m_saveButton->Disable();
        m_compileButton->Disable();
        return false;
    }

    if( source.size() > MAX_KDS_BYTES )
    {
        m_status->SetLabel( _( "KDS exceeds the 16 MiB limit" ) );
        m_editor->SetReadOnly( true );
        m_saveButton->Disable();
        m_compileButton->Disable();
        return false;
    }

    const wxString text = wxString::FromUTF8( source.data(), source.size() );

    if( text.empty() && !source.empty() )
    {
        m_status->SetLabel( _( "KDS is not valid UTF-8" ) );
        m_editor->SetReadOnly( true );
        m_saveButton->Disable();
        m_compileButton->Disable();
        return false;
    }

    wxWindowUpdateLocker freeze( m_editor );
    m_editor->SetText( text );
    m_editor->EmptyUndoBuffer();
    m_editor->SetSavePoint();
    m_loadedSha256 = sha256( source );
    updateModifiedStatus();
    return true;
}


bool KDS_EDITOR_PANEL::save()
{
    if( !m_editor->GetModify() )
    {
        compile();
        return true;
    }

    const std::string source = sourceUtf8();

    if( source.empty() || source.size() > MAX_KDS_BYTES )
    {
        wxMessageBox( _( "KDS must contain valid UTF-8 text between 1 byte and 16 MiB." ),
                      _( "Cannot save KDS" ), wxOK | wxICON_ERROR, this );
        return false;
    }

    std::string existing;
    std::string error;

    if( KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( m_file, existing, error )
        && sha256( existing ) != m_loadedSha256 )
    {
        wxMessageDialog changed(
                this,
                _( "This KDS file changed outside the editor.\n\n"
                   "Yes: overwrite it with this editor's contents.\n"
                   "No: reload the external version.\n"
                   "Cancel: keep editing without saving." ),
                _( "KDS changed on disk" ), wxYES_NO | wxCANCEL | wxNO_DEFAULT | wxICON_WARNING );
        const int answer = changed.ShowModal();

        if( answer == wxID_NO )
        {
            if( load() )
                compile();
        }

        if( answer != wxID_YES )
            return false;
    }

    if( !m_snapshotProvider || !m_snapshotProvider() )
    {
        wxMessageBox( _( "KiChad could not create the required pre-save project snapshot." ),
                      _( "Cannot save KDS" ), wxOK | wxICON_ERROR, this );
        return false;
    }

    if( !KICHAD::CODEX_TOOLS::InstallDesignScriptSidecarAtomically( m_file, source, error ) )
    {
        wxMessageBox( wxString::Format( _( "Could not save KDS atomically:\n%s" ),
                                        wxString::FromUTF8( error ) ),
                      _( "Cannot save KDS" ), wxOK | wxICON_ERROR, this );
        return false;
    }

    m_loadedSha256 = sha256( source );
    m_editor->SetSavePoint();
    compile();
    return true;
}


void KDS_EDITOR_PANEL::compile()
{
    const std::string source = sourceUtf8();
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    wxString report;
    m_editor->MarkerDeleteAll( ERROR_MARKER );

    for( const nlohmann::json& diagnostic : result.diagnostics )
    {
        const wxString severity = wxString::FromUTF8( diagnostic.value( "severity", "error" ) );
        const wxString code = wxString::FromUTF8( diagnostic.value( "code", "invalid_kds" ) );
        const wxString message = wxString::FromUTF8(
                diagnostic.value( "message", "Unknown compiler diagnostic" ) );
        report += wxString::Format( wxS( "%s %s: %s\n" ), severity.Upper(), code, message );

        if( diagnostic.contains( "line" ) && diagnostic["line"].is_number_integer() )
        {
            const int line = std::max( 0, diagnostic["line"].get<int>() - 1 );
            m_editor->MarkerAdd( line, ERROR_MARKER );
        }
    }

    if( report.IsEmpty() )
        report = _( "No compiler diagnostics." );

    m_diagnostics->SetValue( report );

    if( result.ok )
    {
        m_status->SetLabel( wxString::Format( _( "KDS valid — SHA-256 %s" ),
                                             abbreviatedSha( result.sourceSha256 ) ) );
    }
    else
    {
        m_status->SetLabel( wxString::Format( _( "KDS invalid — %zu diagnostic(s)" ),
                                             result.diagnostics.size() ) );
    }
}


void KDS_EDITOR_PANEL::updateModifiedStatus()
{
    if( m_editor->GetModify() )
        m_status->SetLabel( _( "Modified — compile before saving" ) );
    else
        m_status->SetLabel( wxString::Format( _( "Saved — SHA-256 %s" ),
                                             abbreviatedSha( m_loadedSha256 ) ) );
}


bool KDS_EDITOR_PANEL::GetCanClose()
{
    if( !m_editor->GetModify() )
        return true;

    wxMessageDialog dialog( this, _( "Save changes to this KDS file before closing?" ),
                            _( "Unsaved KDS changes" ),
                            wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_WARNING );
    const int answer = dialog.ShowModal();

    if( answer == wxID_CANCEL )
        return false;

    return answer == wxID_NO || save();
}


void KDS_EDITOR_PANEL::onCompile( wxCommandEvent& aEvent )
{
    compile();
}


void KDS_EDITOR_PANEL::onSave( wxCommandEvent& aEvent )
{
    save();
}


void KDS_EDITOR_PANEL::onEditorChanged( wxStyledTextEvent& aEvent )
{
    updateModifiedStatus();
    aEvent.Skip();
}


void KDS_EDITOR_PANEL::onCharHook( wxKeyEvent& aEvent )
{
    if( aEvent.GetModifiers() == wxMOD_CONTROL && ( aEvent.GetKeyCode() == 'S'
                                                     || aEvent.GetKeyCode() == 's' ) )
    {
        save();
        return;
    }

    if( aEvent.GetModifiers() == wxMOD_CONTROL && aEvent.GetKeyCode() == WXK_RETURN )
    {
        compile();
        return;
    }

    aEvent.Skip();
}
