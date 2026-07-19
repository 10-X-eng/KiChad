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

#include <kicad/codex/stepz_artifact_validator.h>

#include <array>
#include <filesystem>
#include <fstream>

#include <kiid.h>
#include <wx/filename.h>
#include <zlib.h>


namespace
{

std::string gzip( const std::string& aSource )
{
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>( const_cast<char*>( aSource.data() ) );
    stream.avail_in = static_cast<uInt>( aSource.size() );
    BOOST_REQUIRE_EQUAL( deflateInit2( &stream, Z_BEST_COMPRESSION, Z_DEFLATED,
                                      MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY ), Z_OK );

    std::array<char, 4096> output{};
    std::string compressed;
    int status = Z_OK;

    do
    {
        stream.next_out = reinterpret_cast<Bytef*>( output.data() );
        stream.avail_out = static_cast<uInt>( output.size() );
        status = deflate( &stream, Z_FINISH );
        BOOST_REQUIRE( status == Z_OK || status == Z_STREAM_END );
        compressed.append( output.data(), output.size() - stream.avail_out );
    } while( status != Z_STREAM_END );

    BOOST_REQUIRE_EQUAL( deflateEnd( &stream ), Z_OK );
    return compressed;
}


class STEPZ_FIXTURE
{
public:
    STEPZ_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-stepz-validator-" + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~STEPZ_FIXTURE()
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

} // namespace


BOOST_AUTO_TEST_SUITE( StepzArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsNativeCompressedStepAndRejectsChecksumDrift )
{
    STEPZ_FIXTURE fixture;
    const std::string step =
            "ISO-10303-21;\nHEADER;\n"
            "FILE_DESCRIPTION(('KiCad electronic assembly'),'2;1');\n"
            "FILE_NAME('controller.stpz','2026-07-19T00:00:00',('Pcbnew'),('Kicad'),"
            "'Open CASCADE STEP processor 7.6','KiCad to STEP converter','Unknown');\n"
            "ENDSEC;\nDATA;\n#1=CARTESIAN_POINT('',(0.,0.,0.));\n"
            "ENDSEC;\nEND-ISO-10303-21;\n";
    const std::string valid = gzip( step );
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::STEPZ_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "valid.stpz", valid ), "controller", error ), error );

    error.clear();
    std::string drifted = valid;
    drifted.back() = static_cast<char>( drifted.back() ^ 0x01 );
    BOOST_CHECK( !KICHAD::STEPZ_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "drifted.stpz", drifted ), "controller", error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    BOOST_CHECK( !KICHAD::STEPZ_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "wrong-project.stpz", valid ), "motor", error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    BOOST_CHECK( !KICHAD::STEPZ_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "concatenated.stpz", valid + valid ), "controller", error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
