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

#ifndef KICHAD_PRODUCTION_PACKAGE_WRITER_H
#define KICHAD_PRODUCTION_PACKAGE_WRITER_H

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD
{

/** Builds and installs the non-KiCad firmware/programming/bring-up release artifacts. */
class PRODUCTION_PACKAGE_WRITER
{
public:
    using JSON = nlohmann::json;

    static JSON BuildPlan( const JSON& aProduction );

    static bool Write( const std::filesystem::path& aVerificationRoot,
                       const std::filesystem::path& aStaging,
                       const JSON& aProduction, const JSON& aPlan,
                       JSON& aArtifacts, std::string& aError );
};

} // namespace KICHAD

#endif // KICHAD_PRODUCTION_PACKAGE_WRITER_H
