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

#include <kicad/codex/legacy_bom_artifact_validator.h>

#include <filesystem>
#include <fstream>

#include <kiid.h>
#include <nlohmann/json.hpp>
#include <wx/filename.h>


namespace
{

using JSON = nlohmann::json;

class LEGACY_BOM_FIXTURE
{
public:
    LEGACY_BOM_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-legacy-bom-validator-"
                     + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~LEGACY_BOM_FIXTURE()
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
        { "fileStem", "controller" },
        { "expectedSchematicBomRows",
          JSON::array( {
              { { "reference", "R1" }, { "value", "10k & 1%" },
                { "footprint", "Resistor_SMD:R_0603" }, { "dnp", false } },
              { { "reference", "V1" }, { "value", "VIRTUAL" },
                { "footprint", "" }, { "dnp", false } }
          } ) }
    };
}


std::string validLegacyBom()
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<export version=\"E\"><design>"
           "<source>/private/controller.kicad_sch</source>"
           "<date>2026-07-19T12:00:00</date>"
           "<tool>Eeschema 10.0.4-KiChad</tool>"
           "<sheet number=\"1\" name=\"/\" tstamps=\"/\"/>"
           "</design><components>"
           "<comp ref=\"R1\"><value>10k &amp; 1%</value>"
           "<footprint>Resistor_SMD:R_0603</footprint></comp>"
           "<comp ref=\"V1\"><value>VIRTUAL</value></comp>"
           "</components><libparts/><libraries/><nets/></export>\n";
}

} // namespace


BOOST_AUTO_TEST_SUITE( LegacyBomArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsExactNativeComponentsAndRejectsUnsafeXml )
{
    LEGACY_BOM_FIXTURE fixture;
    const std::string valid = validLegacyBom();
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::LEGACY_BOM_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "controller-legacy-bom.xml", valid ), plan(), error ),
            error );

    error.clear();
    std::string wrongProducer = valid;
    wrongProducer.replace( wrongProducer.find( "10.0.4" ), 6, "11.0.0" );
    BOOST_CHECK( !KICHAD::LEGACY_BOM_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "wrong-producer.xml", wrongProducer ), plan(), error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    const std::string unsafe =
            "<!DOCTYPE export [<!ENTITY payload SYSTEM \"file:///etc/passwd\">]>\n"
            + valid;
    BOOST_CHECK( !KICHAD::LEGACY_BOM_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "unsafe.xml", unsafe ), plan(), error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
