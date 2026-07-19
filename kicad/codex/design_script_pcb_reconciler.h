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

#ifndef KICHAD_DESIGN_SCRIPT_PCB_RECONCILER_H
#define KICHAD_DESIGN_SCRIPT_PCB_RECONCILER_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Plans collision-safe idempotent changes for PCB items managed by a KDS sidecar. */
class DESIGN_SCRIPT_PCB_RECONCILER
{
public:
    using JSON = nlohmann::json;

    struct CONTEXT
    {
        std::string sourcePath;
        std::string boardPath;
        std::string projectName;
        std::string sourceSha256;
    };

    struct RESULT
    {
        bool ok = false;
        JSON actions = JSON::array();
        JSON nextState = JSON::object();
        JSON diagnostics = JSON::array();
        JSON counts = JSON::object();
    };

    /**
     * @param aDesiredOperations deterministic operations returned by the PCB planner
     * @param aPreviousState null for a first apply, otherwise the prior managed-state manifest
     * @param aLiveInventory objects containing itemId and itemType for every relevant live UUID
     * @param aContext identifies the source, board, project, and exact source revision
     */
    static RESULT Reconcile( const JSON& aDesiredOperations, const JSON& aPreviousState,
                             const JSON& aLiveInventory, const CONTEXT& aContext );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_PCB_RECONCILER_H
