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

#ifndef KICHAD_DESIGN_SCRIPT_SCHEMATIC_PLANNER_H
#define KICHAD_DESIGN_SCRIPT_SCHEMATIC_PLANNER_H

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Lowers typed KDS hierarchy IR into deterministic KiCad 10 schematic expressions. */
class DESIGN_SCRIPT_SCHEMATIC_PLANNER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        bool fullyLowered = false;
        JSON operations = JSON::array();
        JSON diagnostics = JSON::array();
        JSON counts = JSON::object();
    };

    /** Existing screen UUIDs are keyed by project-relative schematic path for lossless apply. */
    static RESULT Plan( const JSON& aCompilerIr,
                        const JSON& aExistingScreenUuids = JSON::object() );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_SCHEMATIC_PLANNER_H
