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

#include <kicad/codex/schematic_pdf_artifact_validator.h>

#include <filesystem>
#include <fstream>
#include <string_view>

#include <kiid.h>
#include <wx/filename.h>


namespace
{

constexpr std::string_view KICAD_JAVASCRIPT_NAMES = R"PDF(<< /JavaScript
 << /Names
    [ (JSInit) << /Type /Action /S /JavaScript /JS (
function ShM\(aEntries\) {
    var aParams = [];
    for \(var i = 0; i < aEntries.length; ++i\) {
        aParams.push\({
            cName: aEntries[i][0],
            cReturn: aEntries[i].length > 1 ? aEntries[i][1] : ''
        }\)
    }

    var cChoice = app.popUpMenuEx.apply\(app, aParams\);
    if \(cChoice == null || cChoice == ''\) return;

    if \(cChoice.substring\(0, 1\) == '#'\) {
        this.pageNum = parseInt\(cChoice.slice\(1\)\);
        return;
    }

    // Fallback: some viewers return cName instead of cReturn
    var url = cChoice;
    if \(url.substring\(0, 4\) != 'http' && url.substring\(0, 4\) != 'file'\) {
        var idx = url.indexOf\('http'\);
        if \(idx < 0\) idx = url.indexOf\('file:'\);
        if \(idx >= 0\) url = url.substring\(idx\);
        else return;
    }

    if \(url.substring\(0, 8\) == 'file:///'\) app.openDoc\(url.substring\(7\)\);
    else if \(url.substring\(0, 7\) == 'file://'\) app.openDoc\('//' + url.substring\(7\)\);
    else app.launchURL\(url\);
}
) >> ]
 >>
>>
endobj
)PDF";


std::string validPdf()
{
    std::string pdf =
            "%PDF-1.5\n"
            "1 0 obj\n<<\n/Type /Pages\n/Kids [2 0 R]\n/Count 1\n>>\nendobj\n"
            "2 0 obj\n<<\n/Type /Page\n/Parent 1 0 R\n>>\nendobj\n"
            "3 0 obj\n<<\n/Producer (KiCad PDF)\n/Creator (Eeschema-PDF)\n"
            "/Title (controller.pdf)\n/Author ()\n/Subject ()\n>>\nendobj\n"
            "4 0 obj\n";
    pdf += KICAD_JAVASCRIPT_NAMES;
    pdf += "5 0 obj\n<<\n/Type /Catalog\n/Pages 1 0 R\n>>\nendobj\n";
    const size_t xref = pdf.size();
    pdf += "xref\n0 1\n0000000000 65535 f \ntrailer\n"
           "<< /Size 6 /Root 5 0 R /Info 3 0 R >>\nstartxref\n"
           + std::to_string( xref ) + "\n%%EOF\n";
    return pdf;
}


class PDF_FIXTURE
{
public:
    PDF_FIXTURE()
    {
        m_root = std::filesystem::path( wxFileName::GetTempDir().ToStdString() )
                 / ( "kichad-schematic-pdf-validator-"
                     + KIID().AsString().ToStdString() );
        std::filesystem::create_directories( m_root );
    }

    ~PDF_FIXTURE()
    {
        std::error_code ignored;
        std::filesystem::remove_all( m_root, ignored );
    }

    std::filesystem::path Write( const std::string& aName,
                                 const std::string& aSource )
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


BOOST_AUTO_TEST_SUITE( SchematicPdfArtifactValidator )


BOOST_AUTO_TEST_CASE( AcceptsPinnedNativePdfAndRejectsExternalActions )
{
    PDF_FIXTURE fixture;
    const std::string valid = validPdf();
    std::string error;
    BOOST_CHECK_MESSAGE(
            KICHAD::SCHEMATIC_PDF_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "controller.pdf", valid ), "controller.pdf", error ),
            error );

    error.clear();
    std::string external = valid;
    external.insert( external.find( "xref\n" ), "/Subtype /Link\n/URI (https://invalid/)\n" );
    BOOST_CHECK( !KICHAD::SCHEMATIC_PDF_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "external.pdf", external ), "controller.pdf", error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    BOOST_CHECK( !KICHAD::SCHEMATIC_PDF_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "wrong-title.pdf", valid ), "motor.pdf", error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string alteredNames = valid;
    const size_t functionName = alteredNames.find( "function ShM" );
    BOOST_REQUIRE_NE( functionName, std::string::npos );
    alteredNames[functionName + 9] = 'X';
    BOOST_CHECK( !KICHAD::SCHEMATIC_PDF_ARTIFACT_VALIDATOR::ValidateFile(
            fixture.Write( "altered-names.pdf", alteredNames ),
            "controller.pdf", error ) );
    BOOST_CHECK( !error.empty() );

    error.clear();
    std::string unicodeTitle = valid;
    const size_t title = unicodeTitle.find( "/Title (controller.pdf)" );
    BOOST_REQUIRE_NE( title, std::string::npos );
    unicodeTitle.replace( title, std::string_view( "/Title (controller.pdf)" ).size(),
                          "/Title <FEFF004D00F80074006F0072>" );
    const size_t xref = unicodeTitle.find( "xref\n" );
    const size_t startXref = unicodeTitle.find( "startxref\n" );
    BOOST_REQUIRE_NE( xref, std::string::npos );
    BOOST_REQUIRE_NE( startXref, std::string::npos );
    const size_t xrefValue = startXref + 10;
    const size_t xrefValueEnd = unicodeTitle.find( '\n', xrefValue );
    BOOST_REQUIRE_NE( xrefValueEnd, std::string::npos );
    unicodeTitle.replace( xrefValue, xrefValueEnd - xrefValue,
                          std::to_string( xref ) );
    BOOST_CHECK_MESSAGE(
            KICHAD::SCHEMATIC_PDF_ARTIFACT_VALIDATOR::ValidateFile(
                    fixture.Write( "unicode-title.pdf", unicodeTitle ),
                    "M\xC3\xB8" "tor", error ),
            error );
}


BOOST_AUTO_TEST_SUITE_END()
