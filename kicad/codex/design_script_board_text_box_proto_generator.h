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

#ifndef KICHAD_DESIGN_SCRIPT_BOARD_TEXT_BOX_PROTO_GENERATOR_H
#define KICHAD_DESIGN_SCRIPT_BOARD_TEXT_BOX_PROTO_GENERATOR_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Lowers one semantic KDS text box into exact BoardTextBox protobuf JSON. */
class DESIGN_SCRIPT_BOARD_TEXT_BOX_PROTO_GENERATOR
{
public:
    using JSON = nlohmann::json;

    static bool Render( const JSON& aTextBox, const std::string& aProject,
                        JSON& aItem );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_BOARD_TEXT_BOX_PROTO_GENERATOR_H
