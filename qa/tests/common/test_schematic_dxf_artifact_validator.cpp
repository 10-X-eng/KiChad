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

#include <kicad/codex/schematic_dxf_artifact_validator.h>

#include <filesystem>
#include <fstream>
#include <string_view>

#include <kiid.h>
#include <wx/filename.h>


namespace
{

class DXF_FIXTURE
{
public:
    DXF_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-schematic-dxf-validator-"
                     + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~DXF_FIXTURE()
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


BOOST_AUTO_TEST_SUITE( SchematicDxfArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsCompleteMillimetreDrawingAndRejectsUnitDrift )
{
    DXF_FIXTURE fixture;
    const std::string valid =
            "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1018\n"
            "  9\n$INSUNITS\n 70\n4\n  0\nENDSEC\n"
            "  0\nSECTION\n  2\nTABLES\n  0\nLAYER\n  2\nKICAD\n  0\nENDSEC\n"
            "  0\nSECTION\n  2\nBLOCKS\n  0\nENDSEC\n"
            "  0\nSECTION\n  2\nENTITIES\n  0\nLINE\n  8\nKICAD\n"
            " 10\n0.0\n 20\n0.0\n 11\n1.0\n 21\n1.0\n  0\nENDSEC\n"
            "  0\nSECTION\n  2\nOBJECTS\n  0\nENDSEC\n  0\nEOF\n";
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::SCHEMATIC_DXF_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "controller.dxf", valid ), "controller", error ), error );

    error.clear();
    BOOST_CHECK_MESSAGE(
            KICHAD::SCHEMATIC_DXF_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "controller-power.dxf", valid ), "controller", error ),
            error );

    error.clear();
    std::string inches = valid;
    static constexpr std::string_view UNITS = "$INSUNITS\n 70\n4";
    const size_t units = inches.find( UNITS );
    BOOST_REQUIRE_NE( units, std::string::npos );
    inches.replace( units + UNITS.size() - 1, 1, "1" );
    BOOST_CHECK( !KICHAD::SCHEMATIC_DXF_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "controller-inches.dxf", inches ), "controller", error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
