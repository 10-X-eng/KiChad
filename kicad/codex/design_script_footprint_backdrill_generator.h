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

#ifndef KICHAD_DESIGN_SCRIPT_FOOTPRINT_BACKDRILL_GENERATOR_H
#define KICHAD_DESIGN_SCRIPT_FOOTPRINT_BACKDRILL_GENERATOR_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Renders semantic top/bottom footprint-pad backdrilling in current KiCad 10 syntax. */
class DESIGN_SCRIPT_FOOTPRINT_BACKDRILL_GENERATOR
{
public:
    using JSON = nlohmann::json;

    static bool Render( const JSON& aBackdrills, std::string& aSource );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_FOOTPRINT_BACKDRILL_GENERATOR_H
