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

#ifndef KICHAD_DESIGN_SCRIPT_PCB_PLANNER_H
#define KICHAD_DESIGN_SCRIPT_PCB_PLANNER_H

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Lowers typed KDS board IR into deterministic KiCad 10 protobuf-JSON operations. */
class DESIGN_SCRIPT_PCB_PLANNER
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

    static RESULT Plan( const JSON& aCompilerIr,
                        const JSON& aResolvedSymbols = JSON::object() );

    /** RFC 9562 UUIDv8 identity used by both lowering and managed-state validation. */
    static std::string StableUuid( const std::string& aProject, const std::string& aKind,
                                   const std::string& aLogicalId );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_PCB_PLANNER_H
