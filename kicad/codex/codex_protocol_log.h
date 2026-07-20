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

#ifndef KICHAD_CODEX_PROTOCOL_LOG_H
#define KICHAD_CODEX_PROTOCOL_LOG_H

#include <string>

#include <nlohmann/json.hpp>


namespace KICHAD::CODEX_PROTOCOL_LOG
{

void Protocol( const char* aDirection, const nlohmann::json& aMessage );
void Event( const char* aEvent, const nlohmann::json& aDetail = nlohmann::json::object() );
void Text( const char* aEvent, const std::string& aText );

} // namespace KICHAD::CODEX_PROTOCOL_LOG

#endif // KICHAD_CODEX_PROTOCOL_LOG_H
