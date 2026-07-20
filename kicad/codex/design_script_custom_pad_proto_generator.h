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

#ifndef KICHAD_DESIGN_SCRIPT_CUSTOM_PAD_PROTO_GENERATOR_H
#define KICHAD_DESIGN_SCRIPT_CUSTOM_PAD_PROTO_GENERATOR_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Lowers one semantic custom-pad geometry into typed BoardGraphicShape protobuf JSON. */
class DESIGN_SCRIPT_CUSTOM_PAD_PROTO_GENERATOR
{
public:
    using JSON = nlohmann::json;

    static bool Render( const JSON& aCustom, const std::string& aProject,
                        const std::string& aOwnerId, const std::string& aLayer,
                        JSON& aShapes );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_CUSTOM_PAD_PROTO_GENERATOR_H
