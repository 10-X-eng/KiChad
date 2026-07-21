/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KICAD_API_HANDLER_COMMON_H
#define KICAD_API_HANDLER_COMMON_H

#include <functional>

#include <google/protobuf/empty.pb.h>

#include <api/api_handler.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>

using namespace kiapi::common;
using google::protobuf::Empty;

enum API_PROJECT_SETTINGS_CHANGE : int
{
    APIPSC_NETCLASSES = 0x01,
    APIPSC_TEXT_VARIABLES = 0x02,
    APIPSC_FIELD_TEMPLATES = 0x04,
    APIPSC_ERC_SEVERITIES = 0x08
};

class API_HANDLER_COMMON : public API_HANDLER
{
public:
    // Editors that cache project settings must refresh them synchronously in this callback.  The
    // handler retains replaced NETCLASS objects until the callback returns.
    explicit API_HANDLER_COMMON( std::function<void( int )> aOnProjectSettingsChanged = {} );

    ~API_HANDLER_COMMON() override {}

private:
    std::function<void( int )> m_onProjectSettingsChanged;

    HANDLER_RESULT<commands::GetVersionResponse> handleGetVersion(
        const HANDLER_CONTEXT<commands::GetVersion>& aCtx );

    HANDLER_RESULT<commands::PathResponse> handleGetKiCadBinaryPath(
        const HANDLER_CONTEXT<commands::GetKiCadBinaryPath>& aCtx );

    HANDLER_RESULT<commands::NetClassesResponse> handleGetNetClasses(
        const HANDLER_CONTEXT<commands::GetNetClasses>& aCtx );

    HANDLER_RESULT<Empty> handleSetNetClasses(
        const HANDLER_CONTEXT<commands::SetNetClasses>& aCtx );

    HANDLER_RESULT<commands::NetClassSettingsResponse> handleGetNetClassSettings(
        const HANDLER_CONTEXT<commands::GetNetClassSettings>& aCtx );

    HANDLER_RESULT<commands::NetClassSettingsResponse> handleUpdateNetClassSettings(
        const HANDLER_CONTEXT<commands::UpdateNetClassSettings>& aCtx );

    HANDLER_RESULT<Empty> handlePing( const HANDLER_CONTEXT<commands::Ping>& aCtx );

    HANDLER_RESULT<types::Box2> handleGetTextExtents(
        const HANDLER_CONTEXT<commands::GetTextExtents>& aCtx );

    HANDLER_RESULT<commands::GetTextAsShapesResponse> handleGetTextAsShapes(
        const HANDLER_CONTEXT<commands::GetTextAsShapes>& aCtx );

    HANDLER_RESULT<commands::ExpandTextVariablesResponse> handleExpandTextVariables(
        const HANDLER_CONTEXT<commands::ExpandTextVariables>& aCtx );

    HANDLER_RESULT<commands::StringResponse> handleGetPluginSettingsPath(
        const HANDLER_CONTEXT<commands::GetPluginSettingsPath>& aCtx );

    HANDLER_RESULT<project::TextVariables> handleGetTextVariables(
        const HANDLER_CONTEXT<commands::GetTextVariables>& aCtx );

    HANDLER_RESULT<Empty> handleSetTextVariables(
        const HANDLER_CONTEXT<commands::SetTextVariables>& aCtx );

    HANDLER_RESULT<project::SchematicFieldTemplates> handleGetSchematicFieldTemplates(
        const HANDLER_CONTEXT<commands::GetSchematicFieldTemplates>& aCtx );

    HANDLER_RESULT<project::SchematicFieldTemplates> handleSetSchematicFieldTemplates(
        const HANDLER_CONTEXT<commands::SetSchematicFieldTemplates>& aCtx );

    HANDLER_RESULT<project::SchematicRuleSeverities> handleGetSchematicRuleSeverities(
        const HANDLER_CONTEXT<commands::GetSchematicRuleSeverities>& aCtx );

    HANDLER_RESULT<project::SchematicRuleSeverities> handleSetSchematicRuleSeverities(
        const HANDLER_CONTEXT<commands::SetSchematicRuleSeverities>& aCtx );
};

#endif //KICAD_API_HANDLER_COMMON_H
