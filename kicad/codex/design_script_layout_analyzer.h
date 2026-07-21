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

#ifndef KICHAD_DESIGN_SCRIPT_LAYOUT_ANALYZER_H
#define KICHAD_DESIGN_SCRIPT_LAYOUT_ANALYZER_H

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Evaluates compiled KDS physical intent without mutating editor state. */
class DESIGN_SCRIPT_LAYOUT_ANALYZER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        bool clean = false;
        JSON summary = JSON::object();
        JSON issues = JSON::array();
    };

    static RESULT Analyze( const JSON& aCompilerIr );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_LAYOUT_ANALYZER_H
