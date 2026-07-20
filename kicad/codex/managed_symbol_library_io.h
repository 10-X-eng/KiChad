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

#ifndef KICHAD_MANAGED_SYMBOL_LIBRARY_IO_H
#define KICHAD_MANAGED_SYMBOL_LIBRARY_IO_H

#include <string>

#include <wx/filename.h>
#include <wx/string.h>


namespace KICHAD::MANAGED_SYMBOL_LIBRARY_IO
{

/** Resolve a compiler-validated ${KIPRJMOD}/...kicad_sym URI without following a file symlink. */
bool Resolve( const wxString& aProjectPath, const std::string& aUri,
              wxFileName& aResolved, std::string& aRelativePath, std::string& aError );

/** Read exact prior bytes, bounded to the KDS native-library limit. */
bool ReadOptional( const wxFileName& aPath, bool& aPresent, std::string& aSource,
                   std::string& aError );

/** Atomically install exact bytes or restore absence, then byte-verify the result. */
bool InstallAtomically( const wxFileName& aPath, bool aPresent, const std::string& aSource,
                        std::string& aError );

/** Ask the sibling KiCad 10 CLI loader to parse and resave an isolated copy. */
bool ValidateNative( const wxFileName& aPath, std::string& aError );

} // namespace KICHAD::MANAGED_SYMBOL_LIBRARY_IO

#endif // KICHAD_MANAGED_SYMBOL_LIBRARY_IO_H
