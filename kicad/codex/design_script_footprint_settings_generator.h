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

#ifndef KICHAD_DESIGN_SCRIPT_FOOTPRINT_SETTINGS_GENERATOR_H
#define KICHAD_DESIGN_SCRIPT_FOOTPRINT_SETTINGS_GENERATOR_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Renders validated footprint-wide settings as current KiCad 10 source. */
class DESIGN_SCRIPT_FOOTPRINT_SETTINGS_GENERATOR
{
public:
    using JSON = nlohmann::json;

    static bool RenderRules( const JSON& aRules, std::string& aSource );
    static bool RenderStackup( const JSON& aStackup, std::string& aSource );
    static bool RenderPrivateLayers( const JSON& aLayers, std::string& aSource );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_FOOTPRINT_SETTINGS_GENERATOR_H
