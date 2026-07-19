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

#ifndef KICHAD_DESIGN_SCRIPT_SYMBOL_RESOLVER_H
#define KICHAD_DESIGN_SCRIPT_SYMBOL_RESOLVER_H

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Resolves exact project-local KiCad symbols into bounded native cache source and pin metadata. */
class DESIGN_SCRIPT_SYMBOL_RESOLVER
{
public:
    using JSON = nlohmann::json;

    struct RESULT
    {
        bool ok = false;
        JSON symbols = JSON::object();
        JSON diagnostics = JSON::array();
        JSON counts = JSON::object();
    };

    /** Library sources are keyed by the KDS symbol-library nickname. */
    static RESULT Resolve( const JSON& aCompilerIr, const JSON& aLibrarySources );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_SYMBOL_RESOLVER_H
