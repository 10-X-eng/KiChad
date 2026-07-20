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

#ifndef KICHAD_MANAGED_FOOTPRINT_LIBRARY_IO_H
#define KICHAD_MANAGED_FOOTPRINT_LIBRARY_IO_H

#include <map>
#include <string>

#include <wx/filename.h>
#include <wx/string.h>


namespace KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO
{

using FILES = std::map<std::string, std::string>;

/** Resolve a compiler-validated ${KIPRJMOD}/...pretty URI without following a symlink. */
bool Resolve( const wxString& aProjectPath, const std::string& aUri,
              wxFileName& aResolved, std::string& aRelativePath, std::string& aError );

/** Snapshot a whole owned .pretty directory, rejecting unowned or unsafe entries. */
bool ReadOptional( const wxFileName& aPath, bool& aPresent, FILES& aFiles,
                   std::string& aError );

/** Atomically install or restore the complete owned .pretty directory. */
bool InstallAtomically( const wxFileName& aPath, bool aPresent, const FILES& aFiles,
                        std::string& aError );

/** Ask the sibling KiCad 10 CLI loader to parse and resave every footprint. */
bool ValidateNative( const wxFileName& aPath, std::string& aError );

} // namespace KICHAD::MANAGED_FOOTPRINT_LIBRARY_IO

#endif // KICHAD_MANAGED_FOOTPRINT_LIBRARY_IO_H
