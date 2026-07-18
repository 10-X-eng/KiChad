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

#ifndef KICHAD_LOSSLESS_SEXPR_DOCUMENT_H
#define KICHAD_LOSSLESS_SEXPR_DOCUMENT_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>


namespace KICHAD
{

/**
 * Lossless structural index over a KiCad s-expression document.
 *
 * Nodes point into the original byte string.  Whitespace, comments, quoting, unknown expressions,
 * ordering, and all untouched UUIDs are therefore reproduced byte-for-byte.  Mutations are bounded
 * span replacements which are parsed before acceptance and reparsed after rendering.
 */
class LOSSLESS_SEXPR_DOCUMENT
{
public:
    enum class NODE_KIND
    {
        LIST,
        ATOM,
        STRING
    };

    struct NODE
    {
        NODE_KIND           kind;
        size_t              begin;
        size_t              end;
        size_t              parent;
        std::vector<size_t> children;
    };

    static constexpr size_t NO_NODE = static_cast<size_t>( -1 );

    static std::unique_ptr<LOSSLESS_SEXPR_DOCUMENT> Parse( std::string aSource,
                                                           std::string* aError = nullptr );

    const std::string& Source() const { return m_source; }
    const std::vector<NODE>& Nodes() const { return m_nodes; }
    const std::vector<size_t>& Roots() const { return m_roots; }

    std::string RawText( size_t aNode ) const;
    std::string AtomText( size_t aNode ) const;
    std::string ListHead( size_t aNode ) const;
    std::vector<size_t> FindLists( const std::string& aHead ) const;

    bool ReplaceNode( size_t aNode, const std::string& aReplacement,
                      std::string* aError = nullptr );
    bool Render( std::string& aOutput, std::string* aError = nullptr ) const;

private:
    struct EDIT
    {
        size_t      begin;
        size_t      end;
        std::string replacement;
    };

    explicit LOSSLESS_SEXPR_DOCUMENT( std::string aSource ) : m_source( std::move( aSource ) ) {}

    bool parse( std::string* aError );
    bool parseNode( size_t& aCursor, size_t aParent, size_t& aNode, std::string* aError );
    void skipTrivia( size_t& aCursor ) const;
    bool fail( size_t aCursor, const std::string& aMessage, std::string* aError ) const;

    std::string         m_source;
    std::vector<NODE>   m_nodes;
    std::vector<size_t> m_roots;
    std::vector<EDIT>   m_edits;
};

} // namespace KICHAD

#endif // KICHAD_LOSSLESS_SEXPR_DOCUMENT_H
