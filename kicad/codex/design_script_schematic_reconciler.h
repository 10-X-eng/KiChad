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

#ifndef KICHAD_DESIGN_SCRIPT_SCHEMATIC_RECONCILER_H
#define KICHAD_DESIGN_SCRIPT_SCHEMATIC_RECONCILER_H

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Reconciles KDS-owned schematic expressions without rewriting unrelated KiCad content. */
class DESIGN_SCRIPT_SCHEMATIC_RECONCILER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        bool ok = false;
        JSON fileActions = JSON::array();
        JSON managedItems = JSON::array();
        JSON diagnostics = JSON::array();
        JSON counts = JSON::object();
    };

    /**
     * aOperation is one reconcile_schematic_hierarchy planner operation.  aLiveFiles rows contain
     * path, present, and exact source when present.  The returned actions contain complete desired
     * bytes but retain exact prior bytes for transaction journaling by the caller.
     */
    static RESULT Reconcile( const JSON& aOperation, const JSON& aPreviousManagedItems,
                             const JSON& aLiveFiles );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_SCHEMATIC_RECONCILER_H
