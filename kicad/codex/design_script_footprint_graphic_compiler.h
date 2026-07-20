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

#ifndef KICHAD_DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER_H
#define KICHAD_DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER_H

#include "lossless_sexpr_document.h"

#include <cstddef>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Typechecks one semantic KDS footprint graphic primitive. */
class DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        bool ok = false;
        JSON graphic = JSON::object();
        JSON diagnostics = JSON::array();
    };

    static bool IsGraphicHead( const std::string& aHead );
    static RESULT Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER_H
