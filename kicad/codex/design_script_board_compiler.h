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

#ifndef KICHAD_DESIGN_SCRIPT_BOARD_COMPILER_H
#define KICHAD_DESIGN_SCRIPT_BOARD_COMPILER_H

#include "lossless_sexpr_document.h"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Type-checks KDS board statements and lowers them into editor-independent physical IR. */
class DESIGN_SCRIPT_BOARD_COMPILER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        JSON                     statements = JSON::array();
        JSON                     synthesis = nullptr;
        JSON                     diagnostics = JSON::array();
        std::vector<std::string> componentReferences;
        std::vector<std::string> netReferences;
        bool                     fullyTyped = true;
    };

    static RESULT Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aBoardNode );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_BOARD_COMPILER_H
