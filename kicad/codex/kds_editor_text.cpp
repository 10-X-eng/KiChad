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

#include "kds_editor_text.h"


std::string KICHAD::KDS_EDITOR_TEXT::CopyExactUtf8( const char* aBuffer, size_t aTextBytes )
{
    if( !aBuffer || aTextBytes == 0 )
        return {};

    return std::string( aBuffer, aTextBytes );
}
