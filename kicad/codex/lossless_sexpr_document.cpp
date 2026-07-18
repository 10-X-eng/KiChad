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

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <sstream>


namespace KICHAD
{

std::unique_ptr<LOSSLESS_SEXPR_DOCUMENT> LOSSLESS_SEXPR_DOCUMENT::Parse( std::string aSource,
                                                                         std::string* aError )
{
    std::unique_ptr<LOSSLESS_SEXPR_DOCUMENT> document(
            new LOSSLESS_SEXPR_DOCUMENT( std::move( aSource ) ) );

    if( !document->parse( aError ) )
        return nullptr;

    return document;
}


std::string LOSSLESS_SEXPR_DOCUMENT::RawText( size_t aNode ) const
{
    if( aNode >= m_nodes.size() )
        return {};

    const NODE& node = m_nodes[aNode];
    return m_source.substr( node.begin, node.end - node.begin );
}


std::string LOSSLESS_SEXPR_DOCUMENT::AtomText( size_t aNode ) const
{
    if( aNode >= m_nodes.size() )
        return {};

    const NODE& node = m_nodes[aNode];
    std::string raw = RawText( aNode );

    if( node.kind != NODE_KIND::STRING || raw.size() < 2 )
        return raw;

    std::string decoded;
    decoded.reserve( raw.size() - 2 );

    for( size_t i = 1; i + 1 < raw.size(); ++i )
    {
        if( raw[i] == '\\' && i + 2 < raw.size() )
        {
            char escaped = raw[++i];

            switch( escaped )
            {
            case 'n':  decoded.push_back( '\n' ); break;
            case 'r':  decoded.push_back( '\r' ); break;
            case 't':  decoded.push_back( '\t' ); break;
            default:   decoded.push_back( escaped ); break;
            }
        }
        else
        {
            decoded.push_back( raw[i] );
        }
    }

    return decoded;
}


std::string LOSSLESS_SEXPR_DOCUMENT::ListHead( size_t aNode ) const
{
    if( aNode >= m_nodes.size() || m_nodes[aNode].kind != NODE_KIND::LIST
        || m_nodes[aNode].children.empty() )
    {
        return {};
    }

    size_t head = m_nodes[aNode].children.front();

    if( m_nodes[head].kind == NODE_KIND::LIST )
        return {};

    return AtomText( head );
}


std::vector<size_t> LOSSLESS_SEXPR_DOCUMENT::FindLists( const std::string& aHead ) const
{
    std::vector<size_t> matches;

    for( size_t i = 0; i < m_nodes.size(); ++i )
    {
        if( m_nodes[i].kind == NODE_KIND::LIST && ListHead( i ) == aHead )
            matches.emplace_back( i );
    }

    return matches;
}


bool LOSSLESS_SEXPR_DOCUMENT::ReplaceNode( size_t aNode, const std::string& aReplacement,
                                           std::string* aError )
{
    if( aNode >= m_nodes.size() )
        return fail( m_source.size(), "Replacement node index is out of range", aError );

    std::string replacementError;
    std::unique_ptr<LOSSLESS_SEXPR_DOCUMENT> replacement = Parse( aReplacement, &replacementError );

    if( !replacement || replacement->Roots().size() != 1 )
    {
        if( aError )
        {
            *aError = replacementError.empty() ? "Replacement must contain exactly one expression"
                                               : "Invalid replacement: " + replacementError;
        }

        return false;
    }

    const NODE& target = m_nodes[aNode];

    for( const EDIT& edit : m_edits )
    {
        if( target.begin < edit.end && edit.begin < target.end )
            return fail( target.begin, "Replacement overlaps an existing edit", aError );
    }

    m_edits.push_back( { target.begin, target.end, aReplacement } );
    return true;
}


bool LOSSLESS_SEXPR_DOCUMENT::Render( std::string& aOutput, std::string* aError ) const
{
    std::vector<EDIT> edits = m_edits;
    std::sort( edits.begin(), edits.end(),
               []( const EDIT& aLeft, const EDIT& aRight )
               {
                   return aLeft.begin > aRight.begin;
               } );

    aOutput = m_source;

    for( const EDIT& edit : edits )
        aOutput.replace( edit.begin, edit.end - edit.begin, edit.replacement );

    std::string parseError;

    if( !Parse( aOutput, &parseError ) )
    {
        if( aError )
            *aError = "Rendered document is invalid: " + parseError;

        aOutput.clear();
        return false;
    }

    return true;
}


bool LOSSLESS_SEXPR_DOCUMENT::parse( std::string* aError )
{
    size_t cursor = 0;

    while( true )
    {
        skipTrivia( cursor );

        if( cursor == m_source.size() )
            break;

        size_t node = NO_NODE;

        if( !parseNode( cursor, NO_NODE, node, aError ) )
            return false;

        m_roots.emplace_back( node );
    }

    if( m_roots.empty() )
        return fail( 0, "Document contains no expressions", aError );

    return true;
}


bool LOSSLESS_SEXPR_DOCUMENT::parseNode( size_t& aCursor, size_t aParent, size_t& aNode,
                                         std::string* aError )
{
    if( aCursor >= m_source.size() )
        return fail( aCursor, "Expected expression", aError );

    const size_t begin = aCursor;

    if( m_source[aCursor] == ')' )
        return fail( aCursor, "Unexpected closing parenthesis", aError );

    if( m_source[aCursor] == '(' )
    {
        aNode = m_nodes.size();
        m_nodes.push_back( { NODE_KIND::LIST, begin, begin, aParent, {} } );
        ++aCursor;

        while( true )
        {
            skipTrivia( aCursor );

            if( aCursor >= m_source.size() )
                return fail( begin, "Unterminated list", aError );

            if( m_source[aCursor] == ')' )
            {
                ++aCursor;
                m_nodes[aNode].end = aCursor;
                return true;
            }

            size_t child = NO_NODE;

            if( !parseNode( aCursor, aNode, child, aError ) )
                return false;

            m_nodes[aNode].children.emplace_back( child );
        }
    }

    NODE_KIND kind = NODE_KIND::ATOM;

    if( m_source[aCursor] == '"' )
    {
        kind = NODE_KIND::STRING;
        ++aCursor;
        bool closed = false;

        while( aCursor < m_source.size() )
        {
            if( m_source[aCursor] == '\\' )
            {
                ++aCursor;

                if( aCursor >= m_source.size() )
                    break;

                ++aCursor;
            }
            else if( m_source[aCursor] == '"' )
            {
                ++aCursor;
                closed = true;
                break;
            }
            else
            {
                ++aCursor;
            }
        }

        if( !closed )
            return fail( begin, "Unterminated string", aError );
    }
    else
    {
        while( aCursor < m_source.size() && m_source[aCursor] != '('
               && m_source[aCursor] != ')' && m_source[aCursor] != ';'
               && !std::isspace( static_cast<unsigned char>( m_source[aCursor] ) ) )
        {
            ++aCursor;
        }

        if( aCursor == begin )
            return fail( begin, "Expected atom", aError );
    }

    aNode = m_nodes.size();
    m_nodes.push_back( { kind, begin, aCursor, aParent, {} } );
    return true;
}


void LOSSLESS_SEXPR_DOCUMENT::skipTrivia( size_t& aCursor ) const
{
    while( aCursor < m_source.size() )
    {
        if( std::isspace( static_cast<unsigned char>( m_source[aCursor] ) ) )
        {
            ++aCursor;
        }
        else if( m_source[aCursor] == ';' )
        {
            while( aCursor < m_source.size() && m_source[aCursor] != '\n' )
                ++aCursor;
        }
        else
        {
            break;
        }
    }
}


bool LOSSLESS_SEXPR_DOCUMENT::fail( size_t aCursor, const std::string& aMessage,
                                     std::string* aError ) const
{
    if( aError )
    {
        size_t line = 1;
        size_t column = 1;

        for( size_t i = 0; i < std::min( aCursor, m_source.size() ); ++i )
        {
            if( m_source[i] == '\n' )
            {
                ++line;
                column = 1;
            }
            else
            {
                ++column;
            }
        }

        std::ostringstream error;
        error << aMessage << " at " << line << ':' << column;
        *aError = error.str();
    }

    return false;
}

} // namespace KICHAD
