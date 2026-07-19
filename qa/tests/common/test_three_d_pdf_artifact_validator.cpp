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

#include <kicad/codex/three_d_pdf_artifact_validator.h>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <kiid.h>
#include <wx/filename.h>
#include <zlib.h>


namespace
{

void append16( std::string& aOutput, uint16_t aValue )
{
    aOutput.push_back( static_cast<char>( aValue ) );
    aOutput.push_back( static_cast<char>( aValue >> 8 ) );
}


void append32( std::string& aOutput, uint32_t aValue )
{
    for( size_t byte = 0; byte < 4; ++byte )
        aOutput.push_back( static_cast<char>( aValue >> ( byte * 8 ) ) );
}


void append64( std::string& aOutput, uint64_t aValue )
{
    append32( aOutput, static_cast<uint32_t>( aValue ) );
    append32( aOutput, static_cast<uint32_t>( aValue >> 32 ) );
}


void appendBlock( std::string& aOutput, uint32_t aType, std::string aData )
{
    append32( aOutput, aType );
    append32( aOutput, static_cast<uint32_t>( aData.size() ) );
    append32( aOutput, 0 );
    aOutput += aData;

    while( aOutput.size() % 4 != 0 )
        aOutput.push_back( '\0' );
}


std::string validU3d()
{
    std::string header;
    append16( header, 256 );
    append16( header, 0 );
    append32( header, 8 );
    append32( header, 88 );
    append64( header, 104 );
    append32( header, 106 );
    append64( header,
              std::bit_cast<uint64_t>( static_cast<double>( 0.001F ) ) );

    std::string result;
    appendBlock( result, 0x00443355, header );
    appendBlock( result, 0xFFFFFF14, std::string( "groups\0\0", 8 ) );
    appendBlock( result, 0xFFFFFF53, std::string( 4, '\0' ) );
    appendBlock( result, 0xFFFFFF54, std::string( 4, '\0' ) );
    appendBlock( result, 0xFFFFFF3B, std::string( 4, '\0' ) );
    return result;
}


std::string zlibCompress( const std::string& aSource )
{
    uLongf bytes = compressBound( static_cast<uLong>( aSource.size() ) );
    std::string compressed( bytes, '\0' );
    BOOST_REQUIRE_EQUAL(
            compress2( reinterpret_cast<Bytef*>( compressed.data() ), &bytes,
                       reinterpret_cast<const Bytef*>( aSource.data() ),
                       static_cast<uLong>( aSource.size() ), Z_BEST_COMPRESSION ), Z_OK );
    compressed.resize( bytes );
    return compressed;
}


std::string valid3dPdf()
{
    const std::string model = zlibCompress( validU3d() );
    std::string pdf =
            "%PDF-1.5\n"
            "1 0 obj\n<< /Producer (KiCad PDF) >>\nendobj\n";

    for( size_t view = 2; view <= 5; ++view )
    {
        pdf += std::to_string( view )
               + " 0 obj\n<<\n/Type /3DView\n/XN (View)\n>>\nendobj\n";
    }

    pdf += "6 0 obj\n<<\n/Type /3D\n/Subtype /U3D\n/DV 0\n"
           "/VA [2 0 R 3 0 R 4 0 R 5 0 R]\n/Length 7 0 R\n"
           "/Filter /FlateDecode\n>>\nstream\n";
    pdf += model;
    pdf += "\nendstream\nendobj\n7 0 obj\n" + std::to_string( model.size() )
           + "\nendobj\n8 0 obj\n<<\n/Type /Annot\n/Subtype /3D\n"
             "/3DD 6 0 R\n>>\nendobj\n";
    const size_t xref = pdf.size();
    pdf += "xref\n0 1\n0000000000 65535 f \ntrailer\n<< /Size 9 /Root 8 0 R >>\n"
           "startxref\n" + std::to_string( xref ) + "\n%%EOF\n";
    return pdf;
}


class PDF_FIXTURE
{
public:
    PDF_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-3d-pdf-validator-" + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~PDF_FIXTURE()
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


BOOST_AUTO_TEST_SUITE( ThreeDPdfArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsNativeInteractivePdfAndRejectsActiveContent )
{
    PDF_FIXTURE fixture;
    const std::string valid = valid3dPdf();
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::THREE_D_PDF_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "valid.3d.pdf", valid ), error ), error );

    error.clear();
    std::string active = valid;
    active.insert( active.find( "xref\n" ), "/Launch /Unsafe\n" );
    BOOST_CHECK( !KICHAD::THREE_D_PDF_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "active.3d.pdf", active ), error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string wrongFilter = valid;
    wrongFilter.replace( wrongFilter.find( "/FlateDecode" ),
                         std::string_view( "/FlateDecode" ).size(), "/LZWDecode  " );
    BOOST_CHECK( !KICHAD::THREE_D_PDF_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "wrong-filter.3d.pdf", wrongFilter ), error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string corruptModel = valid;
    const size_t streamMarker = corruptModel.find( ">>\nstream\n" );
    BOOST_REQUIRE_NE( streamMarker, std::string::npos );
    const size_t modelStart = streamMarker + std::string_view( ">>\nstream\n" ).size();
    BOOST_REQUIRE_LT( modelStart, corruptModel.size() );
    corruptModel[modelStart] = static_cast<char>( corruptModel[modelStart] ^ 0x01 );
    BOOST_CHECK( !KICHAD::THREE_D_PDF_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "corrupt-model.3d.pdf", corruptModel ), error ) );
    BOOST_CHECK( !error.empty() );
}


BOOST_AUTO_TEST_SUITE_END()
