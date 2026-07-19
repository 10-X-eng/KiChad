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

#ifndef KICHAD_SCHEMATIC_PS_ARTIFACT_VALIDATOR_H
#define KICHAD_SCHEMATIC_PS_ARTIFACT_VALIDATOR_H

#include <filesystem>
#include <string>


namespace KICHAD::SCHEMATIC_PS_ARTIFACT_VALIDATOR
{

bool ValidateFile( const std::filesystem::path& aPath,
                   const std::string& aExpectedStem,
                   std::string& aError );

} // namespace KICHAD::SCHEMATIC_PS_ARTIFACT_VALIDATOR

#endif // KICHAD_SCHEMATIC_PS_ARTIFACT_VALIDATOR_H
