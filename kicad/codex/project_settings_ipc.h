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

#ifndef KICHAD_PROJECT_SETTINGS_IPC_H
#define KICHAD_PROJECT_SETTINGS_IPC_H

#include <string>

#include <import_export.h>
#include <api/common/types/project_settings.pb.h>

#include "kicad_ipc_client.h"


namespace KICHAD::PROJECT_SETTINGS_IPC
{

bool QueryTextVariables( const KICHAD_IPC_CLIENT& aClient,
                         const KICHAD_IPC_TARGET& aTarget,
                         kiapi::common::project::TextVariables& aVariables,
                         std::string& aError );

bool ReplaceTextVariables( const KICHAD_IPC_CLIENT& aClient,
                           const KICHAD_IPC_TARGET& aTarget,
                           const kiapi::common::project::TextVariables& aVariables,
                           std::string& aError );

bool QuerySchematicFieldTemplates(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::SchematicFieldTemplates& aTemplates,
        std::string& aError );

bool ReplaceSchematicFieldTemplates(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::SchematicFieldTemplates& aTemplates,
        std::string& aError );

bool QuerySchematicRuleSeverities(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::SchematicRuleSeverities& aSeverities,
        std::string& aError );

bool ReplaceSchematicRuleSeverities(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::SchematicRuleSeverities& aSeverities,
        std::string& aError );

} // namespace KICHAD::PROJECT_SETTINGS_IPC

#endif // KICHAD_PROJECT_SETTINGS_IPC_H
