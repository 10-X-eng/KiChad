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

#ifndef KICHAD_DESIGN_SCRIPT_CONTEXT_BUILDER_H
#define KICHAD_DESIGN_SCRIPT_CONTEXT_BUILDER_H

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Produces bounded, paged semantic KDS context without duplicating authored payloads. */
class DESIGN_SCRIPT_CONTEXT_BUILDER
{
public:
    using JSON = nlohmann::json;

    static JSON Build( const JSON& aIr, const JSON& aPlan,
                       const std::string& aDomain, const std::string& aQuery,
                       size_t aOffset, size_t aLimit );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_CONTEXT_BUILDER_H
