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

#ifndef KICHAD_MECHANICAL_ARTIFACT_VALIDATOR_H
#define KICHAD_MECHANICAL_ARTIFACT_VALIDATOR_H

#include <filesystem>
#include <string>


namespace KICHAD::MECHANICAL_ARTIFACT_VALIDATOR
{

bool ValidateBrep( const std::filesystem::path& aPath, std::string& aError );
bool ValidateGlb( const std::filesystem::path& aPath,
                  const std::string& aExpectedStem,
                  std::string& aError );
bool ValidateStl( const std::filesystem::path& aPath, std::string& aError );
bool ValidateXao( const std::filesystem::path& aPath,
                  const std::string& aExpectedStem,
                  std::string& aError );

} // namespace KICHAD::MECHANICAL_ARTIFACT_VALIDATOR

#endif // KICHAD_MECHANICAL_ARTIFACT_VALIDATOR_H
