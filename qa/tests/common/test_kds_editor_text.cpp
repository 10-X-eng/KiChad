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

#include <boost/test/unit_test.hpp>

#include <kicad/codex/design_script_compiler.h>
#include <kicad/codex/kds_editor_text.h>


BOOST_AUTO_TEST_SUITE( KdsEditorText )


BOOST_AUTO_TEST_CASE( ExcludesScintillaTerminatorFromCompilerSource )
{
    const std::string prefix =
            "(kichad_design\n  (version 1)\n  (project test (title \"Motor Ω\"))\n";
    const std::string suffix = ")\n";
    std::string source = prefix;
    source.append( 23976 - prefix.size() - suffix.size(), ' ' );
    source += suffix;
    std::string scintillaBuffer = source;
    scintillaBuffer.push_back( '\0' );

    BOOST_REQUIRE_EQUAL( source.size(), 23976 );

    const std::string extracted = KICHAD::KDS_EDITOR_TEXT::CopyExactUtf8(
            scintillaBuffer.data(), source.size() );

    BOOST_CHECK_EQUAL( extracted, source );
    BOOST_CHECK_EQUAL( extracted.size(), source.size() );
    BOOST_CHECK_EQUAL( extracted.find( '\0' ), std::string::npos );

    const KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( extracted );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump() );
}


BOOST_AUTO_TEST_CASE( PreservesActualEmbeddedNulForExplicitValidation )
{
    const char buffer[] = { 'a', '\0', 'b', '\0' };
    const std::string extracted = KICHAD::KDS_EDITOR_TEXT::CopyExactUtf8( buffer, 3 );

    BOOST_CHECK_EQUAL( extracted.size(), 3 );
    BOOST_CHECK_EQUAL( extracted.find( '\0' ), 1 );
}


BOOST_AUTO_TEST_SUITE_END()
