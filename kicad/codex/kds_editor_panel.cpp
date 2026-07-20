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
#include "kds_editor_text.h"

#include <algorithm>
#include <set>
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
        m_diagnosticBar( nullptr ),
        m_diagnosticSummary( nullptr ),
        m_compileButton( nullptr ),
        m_saveButton( nullptr ),
        m_goToIssueButton( nullptr ),
        m_repairNulsButton( nullptr ),
        m_compileTimer( this ),
        m_firstDiagnosticPosition( -1 ),
        m_pendingNulCount( 0 )
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

    m_compileButton->SetToolTip( _( "Check this KDS now (Ctrl+Enter)" ) );
    m_saveButton->SetToolTip( _( "Save this KDS (Ctrl+S)" ) );

    m_diagnosticBar = new wxPanel( this );
    wxBoxSizer* diagnosticBarSizer = new wxBoxSizer( wxHORIZONTAL );
    m_diagnosticSummary = new wxStaticText( m_diagnosticBar, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxDefaultSize,
                                            wxST_ELLIPSIZE_END );
    wxFont summaryFont = m_diagnosticSummary->GetFont();
    summaryFont.MakeBold();
    m_diagnosticSummary->SetFont( summaryFont );
    m_goToIssueButton = new wxButton( m_diagnosticBar, wxID_ANY, _( "Go to issue" ) );
    m_repairNulsButton = new wxButton( m_diagnosticBar, wxID_ANY, _( "Repair NUL bytes" ) );
    diagnosticBarSizer->Add( m_diagnosticSummary, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                             FromDIP( 8 ) );
    diagnosticBarSizer->Add( m_goToIssueButton, 0, wxRIGHT, FromDIP( 6 ) );
    diagnosticBarSizer->Add( m_repairNulsButton, 0 );
    m_diagnosticBar->SetSizer( diagnosticBarSizer );
    root->Add( m_diagnosticBar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    m_diagnostics = new wxTextCtrl(
            this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP( wxSize( -1, 92 ) ),
            wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_DONTWRAP );
    m_diagnostics->SetHint( _( "Compiler diagnostics" ) );
    root->Add( m_diagnostics, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

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
    SetSizer( root );

    m_diagnosticBar->Hide();
    m_diagnostics->Hide();

    m_compileButton->Bind( wxEVT_BUTTON, &KDS_EDITOR_PANEL::onCompile, this );
    m_goToIssueButton->Bind( wxEVT_BUTTON, &KDS_EDITOR_PANEL::onGoToIssue, this );
    m_repairNulsButton->Bind( wxEVT_BUTTON, &KDS_EDITOR_PANEL::onRepairNuls, this );
    m_saveButton->Bind( wxEVT_BUTTON, &KDS_EDITOR_PANEL::onSave, this );
    m_editor->Bind( wxEVT_STC_CHANGE, &KDS_EDITOR_PANEL::onEditorChanged, this );
    m_editor->Bind( wxEVT_STC_UPDATEUI, &KDS_EDITOR_PANEL::onEditorUpdateUi, this );
    Bind( wxEVT_TIMER, &KDS_EDITOR_PANEL::onCompileTimer, this );
    Bind( wxEVT_CHAR_HOOK, &KDS_EDITOR_PANEL::onCharHook, this );

    if( load() )
        compile();
}


std::string KDS_EDITOR_PANEL::sourceUtf8() const
{
    const wxCharBuffer utf8 = m_editor->GetTextRaw();
    const int          textBytes = m_editor->GetTextLength();

    if( textBytes <= 0 || utf8.length() < static_cast<size_t>( textBytes ) )
        return {};

    return KICHAD::KDS_EDITOR_TEXT::CopyExactUtf8( utf8.data(),
                                                   static_cast<size_t>( textBytes ) );
}


bool KDS_EDITOR_PANEL::load()
{
    std::string source;
    std::string error;
    m_compileTimer.Stop();
    m_pendingNulRepair.clear();
    m_pendingNulCount = 0;
    m_editor->SetReadOnly( false );

    if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( m_file, source, error ) )
    {
        m_status->SetLabel( _( "Could not load KDS" ) );
        showDiagnostics( _( "KiChad could not load this KDS file." ),
                         wxString::FromUTF8( error ) );
        m_editor->SetReadOnly( true );
        m_saveButton->Disable();
        m_compileButton->Disable();
        return false;
    }

    if( source.size() > MAX_KDS_BYTES )
    {
        m_status->SetLabel( _( "KDS exceeds the 16 MiB limit" ) );
        showDiagnostics( _( "This KDS file is too large to edit." ),
                         _( "KDS files may contain at most 16 MiB of UTF-8 text." ) );
        m_editor->SetReadOnly( true );
        m_saveButton->Disable();
        m_compileButton->Disable();
        return false;
    }

    const size_t firstNul = source.find( '\0' );

    if( firstNul != std::string::npos )
    {
        m_pendingNulRepair = source;
        m_pendingNulCount = static_cast<size_t>( std::count( source.begin(), source.end(), '\0' ) );
        std::replace( m_pendingNulRepair.begin(), m_pendingNulRepair.end(), '\0', ' ' );
        const wxString repairPreview = wxString::FromUTF8( m_pendingNulRepair.data(),
                                                          m_pendingNulRepair.size() );

        if( repairPreview.empty() )
        {
            m_pendingNulRepair.clear();
            m_pendingNulCount = 0;
            m_editor->Clear();
            m_status->SetLabel( _( "KDS is not valid UTF-8" ) );
            showDiagnostics( _( "This KDS contains both invalid UTF-8 and NUL bytes." ),
                             _( "Convert the file to UTF-8 in a binary-safe editor, then reopen it." ) );
            m_editor->SetReadOnly( true );
            m_saveButton->Disable();
            m_compileButton->Disable();
            return false;
        }

        m_editor->SetText( repairPreview );

        m_status->SetLabel( _( "KDS needs text repair" ) );
        showDiagnostics(
                wxString::Format( _( "This KDS contains %zu unsupported NUL byte(s)." ),
                                  m_pendingNulCount ),
                _( "KiChad can replace each NUL byte with a space, preserving token boundaries. "
                   "A project-history snapshot will be created before the repaired file is saved." ),
                static_cast<int>( firstNul ), true );
        m_compileTimer.Stop();
        m_editor->SetReadOnly( true );
        m_saveButton->Disable();
        m_compileButton->Disable();
        m_loadedSha256 = sha256( source );
        return false;
    }

    const wxString text = wxString::FromUTF8( source.data(), source.size() );

    if( text.empty() && !source.empty() )
    {
        m_status->SetLabel( _( "KDS is not valid UTF-8" ) );
        showDiagnostics( _( "This KDS file is not valid UTF-8 text." ),
                         _( "Convert the file to UTF-8 in a binary-safe editor, then reopen it." ) );
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
    m_saveButton->Enable();
    m_compileButton->Enable();
    clearDiagnostics();
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
    m_compileTimer.Stop();
    const std::string source = sourceUtf8();
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT result =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );
    wxString report;
    wxString firstSummary;
    std::set<std::string> displayed;
    size_t diagnosticCount = 0;
    m_firstDiagnosticPosition = -1;
    m_editor->MarkerDeleteAll( ERROR_MARKER );

    for( const nlohmann::json& diagnostic : result.diagnostics )
    {
        const wxString severity = wxString::FromUTF8( diagnostic.value( "severity", "error" ) );
        const wxString code = wxString::FromUTF8( diagnostic.value( "code", "invalid_kds" ) );
        const wxString message = wxString::FromUTF8(
                diagnostic.value( "message", "Unknown compiler diagnostic" ) );
        const std::string identity = diagnostic.dump();

        if( !displayed.insert( identity ).second )
            continue;

        ++diagnosticCount;
        report += wxString::Format( wxS( "%s %s: %s\n" ), severity.Upper(), code, message );

        if( firstSummary.IsEmpty() )
            firstSummary = message;

        int position = -1;

        if( diagnostic.contains( "byteOffset" ) && diagnostic["byteOffset"].is_number_unsigned() )
        {
            const size_t byteOffset = diagnostic["byteOffset"].get<size_t>();

            if( byteOffset <= static_cast<size_t>( m_editor->GetTextLength() ) )
                position = static_cast<int>( byteOffset );
        }

        if( diagnostic.contains( "line" ) && diagnostic["line"].is_number_integer() )
        {
            const int line = std::max( 0, diagnostic["line"].get<int>() - 1 );
            m_editor->MarkerAdd( line, ERROR_MARKER );

            if( position < 0 )
                position = m_editor->PositionFromLine( line );
        }

        if( m_firstDiagnosticPosition < 0 )
            m_firstDiagnosticPosition = position;
    }

    if( result.ok )
    {
        clearDiagnostics();
        const wxString state = m_editor->GetModify() ? _( "KDS valid (unsaved)" ) : _( "KDS valid" );
        m_status->SetLabel( wxString::Format( _( "%s — SHA-256 %s" ), state,
                                             abbreviatedSha( result.sourceSha256 ) ) );
    }
    else
    {
        if( report.IsEmpty() )
            report = _( "ERROR invalid_kds: The compiler did not provide diagnostic details.\n" );

        if( firstSummary.IsEmpty() )
            firstSummary = _( "This KDS has a compilation issue." );

        showDiagnostics( firstSummary, report, m_firstDiagnosticPosition );
        m_status->SetLabel( wxString::Format( _( "KDS invalid — %zu diagnostic(s)" ),
                                             diagnosticCount ) );
    }
}


void KDS_EDITOR_PANEL::showDiagnostics( const wxString& aSummary, const wxString& aDetails,
                                        int aPosition, bool aCanRepairNuls )
{
    m_firstDiagnosticPosition = aPosition;
    m_diagnosticSummary->SetLabel( aSummary );
    m_diagnostics->SetValue( aDetails );
    m_goToIssueButton->Show( aPosition >= 0 );
    m_repairNulsButton->Show( aCanRepairNuls );
    m_diagnosticBar->Show();
    m_diagnostics->Show();
    Layout();
}


void KDS_EDITOR_PANEL::clearDiagnostics()
{
    m_firstDiagnosticPosition = -1;
    m_diagnosticBar->Hide();
    m_diagnostics->Hide();
    Layout();
}


void KDS_EDITOR_PANEL::updateModifiedStatus()
{
    if( m_editor->GetModify() )
        m_status->SetLabel( _( "Modified — checking…" ) );
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


void KDS_EDITOR_PANEL::onCompileTimer( wxTimerEvent& aEvent )
{
    compile();
}


void KDS_EDITOR_PANEL::onGoToIssue( wxCommandEvent& aEvent )
{
    if( m_firstDiagnosticPosition < 0 )
        return;

    m_editor->GotoPos( m_firstDiagnosticPosition );
    m_editor->EnsureCaretVisible();
    m_editor->SetFocus();
}


void KDS_EDITOR_PANEL::onRepairNuls( wxCommandEvent& aEvent )
{
    if( m_pendingNulRepair.empty() || m_pendingNulCount == 0 )
        return;

    if( !m_snapshotProvider || !m_snapshotProvider() )
    {
        wxMessageBox( _( "KiChad could not create the required pre-repair project snapshot." ),
                      _( "Cannot repair KDS" ), wxOK | wxICON_ERROR, this );
        return;
    }

    std::string error;

    if( !KICHAD::CODEX_TOOLS::InstallDesignScriptSidecarAtomically(
                m_file, m_pendingNulRepair, error ) )
    {
        wxMessageBox( wxString::Format( _( "Could not repair KDS atomically:\n%s" ),
                                        wxString::FromUTF8( error ) ),
                      _( "Cannot repair KDS" ), wxOK | wxICON_ERROR, this );
        return;
    }

    const size_t repaired = m_pendingNulCount;

    if( load() )
    {
        compile();
        m_status->SetLabel( wxString::Format( _( "KDS repaired — replaced %zu NUL byte(s)" ),
                                             repaired ) );
    }
}


void KDS_EDITOR_PANEL::onSave( wxCommandEvent& aEvent )
{
    save();
}


void KDS_EDITOR_PANEL::onEditorChanged( wxStyledTextEvent& aEvent )
{
    updateModifiedStatus();
    m_compileTimer.StartOnce( 400 );
    aEvent.Skip();
}


void KDS_EDITOR_PANEL::onEditorUpdateUi( wxStyledTextEvent& aEvent )
{
    const int caret = m_editor->GetCurrentPos();
    int brace = -1;

    if( caret > 0 )
    {
        const int candidate = m_editor->GetCharAt( caret - 1 );

        if( candidate == '(' || candidate == ')' || candidate == '[' || candidate == ']'
            || candidate == '{' || candidate == '}' )
        {
            brace = caret - 1;
        }
    }

    if( brace < 0 )
    {
        const int candidate = m_editor->GetCharAt( caret );

        if( candidate == '(' || candidate == ')' || candidate == '[' || candidate == ']'
            || candidate == '{' || candidate == '}' )
        {
            brace = caret;
        }
    }

    if( brace < 0 )
    {
        m_editor->BraceHighlight( -1, -1 );
    }
    else
    {
        const int matching = m_editor->BraceMatch( brace );

        if( matching < 0 )
            m_editor->BraceBadLight( brace );
        else
            m_editor->BraceHighlight( brace, matching );
    }

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
