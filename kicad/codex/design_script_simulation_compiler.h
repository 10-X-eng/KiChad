/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef KICHAD_DESIGN_SCRIPT_SIMULATION_COMPILER_H
#define KICHAD_DESIGN_SCRIPT_SIMULATION_COMPILER_H

#include <string>
#include <vector>

#include <nlohmann/json.hpp>


namespace KICHAD
{

class LOSSLESS_SEXPR_DOCUMENT;


/** Typechecks one deterministic SPICE simulation declared inside a KDS electrical contract. */
class DESIGN_SCRIPT_SIMULATION_COMPILER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        JSON                     simulation = JSON::object();
        JSON                     diagnostics = JSON::array();
        std::vector<std::string> referencedComponents;
        std::vector<std::string> referencedNets;
    };

    static RESULT Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_SIMULATION_COMPILER_H
