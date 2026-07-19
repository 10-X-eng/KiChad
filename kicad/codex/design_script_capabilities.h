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

#ifndef KICHAD_DESIGN_SCRIPT_CAPABILITIES_H
#define KICHAD_DESIGN_SCRIPT_CAPABILITIES_H

#include <nlohmann/json.hpp>


namespace KICHAD
{

/**
 * Returns the authoritative, AI-readable KDS coverage inventory.
 *
 * This catalog describes compiler/backend coverage; it does not introduce another authored
 * design representation.  The same .kicad_kds source remains the only external design language.
 */
nlohmann::json DesignScriptCapabilities();

} // namespace KICHAD

#endif // KICHAD_DESIGN_SCRIPT_CAPABILITIES_H
