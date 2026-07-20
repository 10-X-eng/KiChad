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

#ifndef KICHAD_DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR_H
#define KICHAD_DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR_H

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Lowers complete KDS-owned footprint libraries to deterministic KiCad 10 source files. */
class DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        bool ok = false;
        JSON sources = JSON::object();
        JSON libraries = JSON::object();
        JSON counts = { { "libraries", 0 }, { "footprints", 0 }, { "pads", 0 },
                        { "models", 0 } };
        JSON diagnostics = JSON::array();
    };

    static RESULT Generate( const JSON& aCompilerIr );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_FOOTPRINT_LIBRARY_GENERATOR_H
