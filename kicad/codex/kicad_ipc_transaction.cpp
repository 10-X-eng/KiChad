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

#include "kicad_ipc_transaction.h"


KICHAD_IPC_COMMIT_GUARD::KICHAD_IPC_COMMIT_GUARD( const KICHAD_IPC_CLIENT& aClient,
                                                  const KICHAD_IPC_TARGET& aTarget ) :
        m_client( aClient ),
        m_target( aTarget )
{}


KICHAD_IPC_COMMIT_GUARD::~KICHAD_IPC_COMMIT_GUARD()
{
    if( m_active )
    {
        std::string ignored;
        m_client.EndCommit( m_target, m_id, false, "", ignored );
    }
}


bool KICHAD_IPC_COMMIT_GUARD::Begin( std::string& aError )
{
    m_active = m_client.BeginCommit( m_target, m_id, aError );

    if( !m_active )
    {
        // EndCommit deliberately accepts an empty ID for CMA_DROP. Recover a transaction whose
        // previous response was lost before refusing the new mutation.
        std::string recoveryError;

        if( m_client.EndCommit( m_target, "", false, "", recoveryError ) )
            m_active = m_client.BeginCommit( m_target, m_id, aError );
    }

    return m_active;
}


bool KICHAD_IPC_COMMIT_GUARD::Commit( const std::string& aMessage, std::string& aError )
{
    if( !m_active || !m_client.EndCommit( m_target, m_id, true, aMessage, aError ) )
        return false;

    m_active = false;
    return true;
}


bool KICHAD_IPC_COMMIT_GUARD::Drop( std::string& aError )
{
    if( !m_active )
        return true;

    if( !m_client.EndCommit( m_target, m_id, false, "", aError ) )
        return false;

    m_active = false;
    return true;
}
