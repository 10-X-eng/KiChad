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

#ifndef KICHAD_DESIGN_SCRIPT_COMPILER_H
#define KICHAD_DESIGN_SCRIPT_COMPILER_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/**
 * Front end for the bounded, deterministic KiChad Design Script language.
 *
 * KDS is an s-expression language authored by Codex and compiled into a validated JSON IR.  The IR
 * is deliberately independent of editor state so later compiler passes can plan, preview, apply,
 * and verify the same program through KiCad-native schematic, library, PCB, sourcing, and
 * fabrication backends.
 */
class DESIGN_SCRIPT_COMPILER
{
public:
    using JSON = nlohmann::json;

    static constexpr int LANGUAGE_VERSION = 1;

    struct RESULT
    {
        bool        ok = false;
        std::string sourceSha256;
        JSON        ir = JSON::object();
        JSON        plan = JSON::object();
        JSON        diagnostics = JSON::array();
    };

    static JSON   Describe();
    static RESULT Compile( const std::string& aSource );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_COMPILER_H
