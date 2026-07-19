/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <boost/test/unit_test.hpp>

#include <kicad/codex/board_ps_artifact_validator.h>

#include <filesystem>
#include <fstream>

#include <kiid.h>
#include <wx/filename.h>


namespace
{

std::string validBoardPostscript()
{
    return "%!PS-Adobe-3.0\n%%Creator: PCBNEW\n%%Pages: 1\n"
           "%%BoundingBox: 0 0 596 842\n%%DocumentMedia: A4 595 842 0 () ()\n"
           "%%Orientation: Landscape\n%%EndComments\n%%BeginProlog\n"
           "/line { newpath moveto lineto stroke } bind def\n%%EndProlog\n"
           "%%Page: (F.Cu) 1\n%%BeginPageSetup\ngsave\n%%EndPageSetup\n"
           "0 0 10 10 line\nshowpage\ngrestore\n%%EOF\n";
}


class PS_FIXTURE
{
public:
    PS_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-board-ps-validator-"
                     + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~PS_FIXTURE()
    {
        std::error_code ignored;
        std::filesystem::remove_all( m_root, ignored );
    }

    std::filesystem::path Write( const std::string& aName,
                                 const std::string& aSource )
    {
        const std::filesystem::path path = m_root / aName;
        std::ofstream output( path, std::ios::binary | std::ios::trunc );
        output << aSource;
        output.close();
        BOOST_REQUIRE( output.good() );
        return path;
    }

private:
    std::filesystem::path m_root;
};

} // namespace


BOOST_AUTO_TEST_SUITE( BoardPsArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsExactNativeLayerAndRejectsIdentityDrift )
{
    PS_FIXTURE fixture;
    const std::string valid = validBoardPostscript();
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::BOARD_PS_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "controller-F_Cu.ps", valid ), "F.Cu", error ), error );

    error.clear();
    BOOST_CHECK( !KICHAD::BOARD_PS_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "controller-B_Cu.ps", valid ), "B.Cu", error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string privileged = valid;
    privileged.insert( privileged.find( "showpage\n" ), "(payload) deletefile\n" );
    BOOST_CHECK( !KICHAD::BOARD_PS_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "controller-unsafe.ps", privileged ), "F.Cu", error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
