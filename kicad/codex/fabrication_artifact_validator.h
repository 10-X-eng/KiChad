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

#ifndef KICHAD_FABRICATION_ARTIFACT_VALIDATOR_H
#define KICHAD_FABRICATION_ARTIFACT_VALIDATOR_H

#include <filesystem>
#include <string>


namespace KICHAD::FABRICATION_ARTIFACT_VALIDATOR
{

bool ValidateAssemblySvg( const std::filesystem::path& aPath, std::string& aError );
bool ValidateAssemblyDxf( const std::filesystem::path& aPath, std::string& aError );
bool ValidateGenCad( const std::filesystem::path& aPath, std::string& aError );
bool ValidateVrml( const std::filesystem::path& aPath, std::string& aError );
bool ValidateBoardStatistics( const std::filesystem::path& aPath,
                              const std::string& aExpectedStem,
                              std::string& aError );

} // namespace KICHAD::FABRICATION_ARTIFACT_VALIDATOR

#endif // KICHAD_FABRICATION_ARTIFACT_VALIDATOR_H
