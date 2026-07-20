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

#ifndef KICHAD_KDS_EDITOR_TEXT_H
#define KICHAD_KDS_EDITOR_TEXT_H

#include <cstddef>
#include <string>


namespace KICHAD::KDS_EDITOR_TEXT
{

/** Copy exactly the logical UTF-8 bytes reported by Scintilla, excluding its C terminator. */
std::string CopyExactUtf8( const char* aBuffer, size_t aTextBytes );

} // namespace KICHAD::KDS_EDITOR_TEXT

#endif // KICHAD_KDS_EDITOR_TEXT_H
