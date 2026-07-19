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

#include <kicad/codex/schematic_bom_artifact_validator.h>

#include <filesystem>
#include <fstream>

#include <kiid.h>
#include <nlohmann/json.hpp>
#include <wx/filename.h>


namespace
{

using JSON = nlohmann::json;

class BOM_FIXTURE
{
public:
    BOM_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-schematic-bom-validator-"
                     + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~BOM_FIXTURE()
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


JSON plan()
{
    return {
        { "expectedSchematicBomRows",
          JSON::array( {
              { { "reference", "R1" }, { "value", "10k" },
                { "footprint", "Resistor_SMD:R_0603" }, { "dnp", false } },
              { { "reference", "U1" }, { "value", "Logic \"A\"" },
                { "footprint", "Package_SO:SOIC-8" }, { "dnp", true } }
          } ) }
    };
}

} // namespace


BOOST_AUTO_TEST_SUITE( SchematicBomArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsExactNativeRowsAndRejectsKdsDrift )
{
    BOM_FIXTURE fixture;
    const std::string valid =
            "\"Reference\",\"Value\",\"Footprint\",\"Quantity\",\"DNP\"\n"
            "\"R1\",\"10k\",\"Resistor_SMD:R_0603\",\"1\",\"\"\n"
            "\"U1\",\"Logic \"\"A\"\"\",\"Package_SO:SOIC-8\",\"1\",\"DNP\"\n";
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::SCHEMATIC_BOM_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "controller-bom.csv", valid ), plan(), error ), error );

    error.clear();
    std::string wrongValue = valid;
    wrongValue.replace( wrongValue.find( "10k" ), 3, "22k" );
    BOOST_CHECK( !KICHAD::SCHEMATIC_BOM_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "wrong-value.csv", wrongValue ), plan(), error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string duplicate = valid;
    duplicate.append( "\"R1\",\"10k\",\"Resistor_SMD:R_0603\",\"1\",\"\"\n" );
    BOOST_CHECK( !KICHAD::SCHEMATIC_BOM_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "duplicate.csv", duplicate ), plan(), error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
