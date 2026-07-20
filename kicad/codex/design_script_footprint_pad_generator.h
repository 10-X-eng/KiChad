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

#ifndef KICHAD_DESIGN_SCRIPT_FOOTPRINT_PAD_GENERATOR_H
#define KICHAD_DESIGN_SCRIPT_FOOTPRINT_PAD_GENERATOR_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Lowers one typed KDS footprint pad to current KiCad 10 native syntax. */
class DESIGN_SCRIPT_FOOTPRINT_PAD_GENERATOR
{
public:
    static bool Render( const nlohmann::json& aPad, const std::string& aUuid,
                        std::string& aSource );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_FOOTPRINT_PAD_GENERATOR_H
