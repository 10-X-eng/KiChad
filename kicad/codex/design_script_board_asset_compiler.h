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

#ifndef KICHAD_DESIGN_SCRIPT_BOARD_ASSET_COMPILER_H
#define KICHAD_DESIGN_SCRIPT_BOARD_ASSET_COMPILER_H

#include "lossless_sexpr_document.h"

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Typechecks self-contained board reference images and native barcodes. */
class DESIGN_SCRIPT_BOARD_ASSET_COMPILER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        bool ok = false;
        JSON statement = JSON::object();
        JSON diagnostics = JSON::array();
    };

    static bool IsAssetHead( const std::string& aHead );
    static RESULT Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_BOARD_ASSET_COMPILER_H
