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

#ifndef KICHAD_U3D_ARTIFACT_VALIDATOR_H
#define KICHAD_U3D_ARTIFACT_VALIDATOR_H

#include <filesystem>
#include <string>
#include <string_view>


namespace KICHAD::U3D_ARTIFACT_VALIDATOR
{

bool ValidateContents( std::string_view aContents, std::string& aError );
bool ValidateFile( const std::filesystem::path& aPath, std::string& aError );

} // namespace KICHAD::U3D_ARTIFACT_VALIDATOR

#endif // KICHAD_U3D_ARTIFACT_VALIDATOR_H
