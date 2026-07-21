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

#ifndef KICHAD_DESIGN_RELEASE_WRITER_H
#define KICHAD_DESIGN_RELEASE_WRITER_H

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Copies the exact portable KDS/native project inputs from a private release snapshot. */
class DESIGN_RELEASE_WRITER
{
public:
    using JSON = nlohmann::json;

    static bool Write( const std::filesystem::path& aVerificationRoot,
                       const std::filesystem::path& aStaging,
                       JSON& aArtifacts, std::string& aError );
};

} // namespace KICHAD

#endif // KICHAD_DESIGN_RELEASE_WRITER_H
