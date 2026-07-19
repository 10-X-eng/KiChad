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

#include <kicad/codex/mechanical_artifact_validator.h>

#include <cstdint>
#include <filesystem>
#include <fstream>

#include <kiid.h>
#include <nlohmann/json.hpp>
#include <wx/filename.h>


namespace
{

using JSON = nlohmann::json;


class MECHANICAL_FIXTURE
{
public:
    MECHANICAL_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-mechanical-validator-" + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~MECHANICAL_FIXTURE()
    {
        std::error_code ignored;
        std::filesystem::remove_all( m_root, ignored );
    }

    std::filesystem::path Write( const std::string& aName, const std::string& aSource )
    {
        const std::filesystem::path path = m_root / aName;
        std::ofstream output( path, std::ios::binary | std::ios::trunc );
        output.write( aSource.data(), static_cast<std::streamsize>( aSource.size() ) );
        output.close();
        BOOST_REQUIRE( output.good() );
        return path;
    }

private:
    std::filesystem::path m_root;
};


void appendLittleEndian32( std::string& aOutput, uint32_t aValue )
{
    for( size_t byte = 0; byte < 4; ++byte )
        aOutput.push_back( static_cast<char>( aValue >> ( byte * 8 ) ) );
}


std::string validGlb()
{
    JSON scene = {
        { "asset",
          { { "version", "2.0" },
            { "extras", { { "generator", "KiCad 10.0.4-KiChad" },
                            { "pcb_name", "controller" } } } } },
        { "scene", 0 },
        { "scenes", JSON::array( { { { "nodes", JSON::array( { 0 } ) } } } ) },
        { "nodes", JSON::array( { { { "mesh", 0 } } } ) },
        { "meshes", JSON::array( { { { "primitives", JSON::array(
                { { { "attributes", { { "POSITION", 0 } } } } } ) } } } ) },
        { "bufferViews", JSON::array( { { { "buffer", 0 }, { "byteLength", 12 } } } ) },
        { "accessors", JSON::array( { { { "bufferView", 0 }, { "componentType", 5126 },
                                         { "count", 1 }, { "type", "VEC3" } } } ) },
        { "buffers", JSON::array( { { { "byteLength", 12 } } } ) }
    };
    std::string json = scene.dump();

    while( json.size() % 4 != 0 )
        json.push_back( ' ' );

    std::string result;
    appendLittleEndian32( result, 0x46546C67 );
    appendLittleEndian32( result, 2 );
    appendLittleEndian32( result, static_cast<uint32_t>( 12 + 8 + json.size() + 8 + 12 ) );
    appendLittleEndian32( result, static_cast<uint32_t>( json.size() ) );
    appendLittleEndian32( result, 0x4E4F534A );
    result += json;
    appendLittleEndian32( result, 12 );
    appendLittleEndian32( result, 0x004E4942 );
    result.append( 12, '\0' );
    return result;
}

} // namespace


BOOST_AUTO_TEST_SUITE( MechanicalArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsCompleteBrepAndRejectsMissingTopology )
{
    MECHANICAL_FIXTURE fixture;
    const std::string valid =
            "\nCASCADE Topology V1, (c) Matra-Datavision\n"
            "Locations 1\nCurve2ds 0\nCurves 3\nPolygon3D 0\n"
            "PolygonOnTriangulations 0\nSurfaces 1\nTriangulations 0\n"
            "TShapes 1\n1100000\n+1 0 \n";
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateBrep(
                    fixture.Write( "valid.brep", valid ), error ), error );

    error.clear();
    BOOST_CHECK( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateBrep(
            fixture.Write( "broken.brep", valid.substr( 0, valid.find( "TShapes" ) ) ),
            error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_CASE( AcceptsNativeGlbAndRejectsContainerLengthDrift )
{
    MECHANICAL_FIXTURE fixture;
    const std::string valid = validGlb();
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateGlb(
                    fixture.Write( "valid.glb", valid ), "controller", error ), error );

    error.clear();
    std::string drifted = valid;
    drifted[8] = static_cast<char>( drifted[8] + 4 );
    BOOST_CHECK( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateGlb(
            fixture.Write( "drifted.glb", drifted ), "controller", error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_CASE( AcceptsTriangularAsciiStlAndRejectsIncompleteFacet )
{
    MECHANICAL_FIXTURE fixture;
    const std::string valid =
            "solid controller\n"
            " facet normal 0 0 1\n"
            "  outer loop\n"
            "   vertex 0 0 0\n"
            "   vertex 1 0 0\n"
            "   vertex 0 1 0\n"
            "  endloop\n"
            " endfacet\n"
            "endsolid controller\n";
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateStl(
                    fixture.Write( "valid.stl", valid ), error ), error );

    error.clear();
    BOOST_CHECK( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateStl(
            fixture.Write( "broken.stl", valid.substr( 0, valid.find( "endloop" ) ) ),
            error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_CASE( AcceptsNativeXaoAndRejectsTopologyCountDrift )
{
    MECHANICAL_FIXTURE fixture;
    const std::string valid =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<XAO version=\"1.0\" author=\"KiCad\">"
            "<geometry name=\"controller\">"
            "<shape format=\"BREP\"><![CDATA[\n"
            "CASCADE Topology V1, (c) Matra-Datavision\n"
            "Locations 1\nCurve2ds 0\nCurves 3\nPolygon3D 0\n"
            "PolygonOnTriangulations 0\nSurfaces 1\nTriangulations 0\n"
            "TShapes 1\n+1 0\n]]></shape>"
            "<topology><vertices count=\"1\"><vertex index=\"0\" reference=\"1\"/>"
            "</vertices><edges count=\"0\"/><faces count=\"1\">"
            "<face index=\"0\" reference=\"1\"/></faces><solids count=\"1\">"
            "<solid index=\"0\" reference=\"1\"/></solids></topology>"
            "</geometry><groups count=\"0\"/><fields count=\"0\"/></XAO>\n";
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateXao(
                    fixture.Write( "valid.xao", valid ), "controller", error ), error );

    error.clear();
    std::string drifted = valid;
    drifted.replace( drifted.find( "vertices count=\"1\"" ),
                     std::string( "vertices count=\"1\"" ).size(),
                     "vertices count=\"2\"" );
    BOOST_CHECK( !KICHAD::MECHANICAL_ARTIFACT_VALIDATOR::ValidateXao(
            fixture.Write( "drifted.xao", drifted ), "controller", error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
