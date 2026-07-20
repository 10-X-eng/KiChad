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

#ifndef KICHAD_KDS_EDITOR_PANEL_H
#define KICHAD_KDS_EDITOR_PANEL_H

#include <functional>
#include <string>

#include <widgets/panel_notebook_base.h>
#include <wx/filename.h>


class wxButton;
class wxStaticText;
class wxStyledTextCtrl;
class wxStyledTextEvent;
class wxTextCtrl;


/** Integrated source editor for the single AI-native .kicad_kds representation. */
class KDS_EDITOR_PANEL : public PANEL_NOTEBOOK_BASE
{
public:
    using SNAPSHOT_PROVIDER = std::function<bool()>;

    KDS_EDITOR_PANEL( wxWindow* aParent, const wxFileName& aFile,
                      SNAPSHOT_PROVIDER aSnapshotProvider );

    const wxString& GetFilePath() const { return m_filePath; }
    bool GetCanClose() override;

private:
    bool load();
    bool save();
    void compile();
    std::string sourceUtf8() const;
    void updateModifiedStatus();

    void onCompile( wxCommandEvent& aEvent );
    void onSave( wxCommandEvent& aEvent );
    void onEditorChanged( wxStyledTextEvent& aEvent );
    void onCharHook( wxKeyEvent& aEvent );

    wxFileName        m_file;
    wxString          m_filePath;
    std::string       m_loadedSha256;
    SNAPSHOT_PROVIDER m_snapshotProvider;
    wxStyledTextCtrl* m_editor;
    wxTextCtrl*       m_diagnostics;
    wxStaticText*     m_status;
    wxButton*         m_compileButton;
    wxButton*         m_saveButton;
};

#endif // KICHAD_KDS_EDITOR_PANEL_H
