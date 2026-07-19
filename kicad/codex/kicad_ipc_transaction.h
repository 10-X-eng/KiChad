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

#ifndef KICHAD_KICAD_IPC_TRANSACTION_H
#define KICHAD_KICAD_IPC_TRANSACTION_H

#include "kicad_ipc_client.h"

#include <string>


/** RAII guard for one KiCad IPC transaction, dropped automatically unless committed. */
class KICHAD_IPC_COMMIT_GUARD
{
public:
    KICHAD_IPC_COMMIT_GUARD( const KICHAD_IPC_CLIENT& aClient,
                             const KICHAD_IPC_TARGET& aTarget );
    ~KICHAD_IPC_COMMIT_GUARD();

    bool Begin( std::string& aError );
    bool Commit( const std::string& aMessage, std::string& aError );
    bool Drop( std::string& aError );

private:
    const KICHAD_IPC_CLIENT& m_client;
    const KICHAD_IPC_TARGET& m_target;
    std::string              m_id;
    bool                     m_active = false;
};

#endif // KICHAD_KICAD_IPC_TRANSACTION_H
