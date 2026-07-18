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

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <kicad/codex/lossless_sexpr_document.h>


BOOST_AUTO_TEST_SUITE( LosslessSexprDocument )


BOOST_AUTO_TEST_CASE( ExactRoundTripPreservesUnknownContent )
{
    const std::string source =
            "; leading comment\n"
            "(kicad_sch (version 20250114)\n"
            "  (uuid 11111111-2222-3333-4444-555555555555)\n"
            "  (future_extension  42 \"quoted \\\"value\\\"\")\n"
            "  (symbol (lib_id \"Device:R\") (property \"Value\" \"10k\")))\n";

    std::string error;
    auto document = KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( source, &error );

    BOOST_REQUIRE_MESSAGE( document, error );
    BOOST_CHECK_EQUAL( document->ListHead( document->Roots().front() ), "kicad_sch" );

    std::string rendered;
    BOOST_REQUIRE_MESSAGE( document->Render( rendered, &error ), error );
    BOOST_CHECK_EQUAL( rendered, source );
}


BOOST_AUTO_TEST_CASE( BoundedMutationPreservesUuidAndTrivia )
{
    const std::string source =
            "(kicad_sch\n"
            "  (uuid 11111111-2222-3333-4444-555555555555)\n"
            "  (symbol\n"
            "    (property \"Reference\" \"R1\")\n"
            "    (property  \"Value\"   \"10k\")\n"
            "    (unknown keep-me)))\n";

    std::string error;
    auto document = KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( source, &error );
    BOOST_REQUIRE_MESSAGE( document, error );

    std::vector<size_t> properties = document->FindLists( "property" );
    BOOST_REQUIRE_EQUAL( properties.size(), 2 );
    BOOST_REQUIRE( document->ReplaceNode( properties[1], "(property \"Value\" \"12k\")", &error ) );

    std::string rendered;
    BOOST_REQUIRE_MESSAGE( document->Render( rendered, &error ), error );
    BOOST_CHECK( rendered.find( "11111111-2222-3333-4444-555555555555" ) != std::string::npos );
    BOOST_CHECK( rendered.find( "(unknown keep-me)" ) != std::string::npos );
    BOOST_CHECK( rendered.find( "(property \"Value\" \"12k\")" ) != std::string::npos );
    BOOST_CHECK( rendered.find( "(property \"Reference\" \"R1\")" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( RejectsInvalidAndOverlappingReplacements )
{
    std::string error;
    auto document = KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse(
            "(kicad_sch (symbol (property \"Value\" \"10k\")))", &error );
    BOOST_REQUIRE_MESSAGE( document, error );

    const size_t root = document->Roots().front();
    const size_t symbol = document->FindLists( "symbol" ).front();
    BOOST_REQUIRE( document->ReplaceNode( symbol, "(symbol (uuid abc) (in_bom yes))", &error ) );
    BOOST_CHECK( !document->ReplaceNode( root, "(kicad_sch)", &error ) );
    BOOST_CHECK_NE( error.find( "overlaps" ), std::string::npos );
    BOOST_CHECK( !document->ReplaceNode( root, "(kicad_sch", &error ) );
}


BOOST_AUTO_TEST_SUITE_END()
