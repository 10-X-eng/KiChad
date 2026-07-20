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
#include <kicad/codex/design_script_symbol_library_generator.h>
#include <kicad/codex/managed_symbol_library_io.h>

#include <kiid.h>

#include <filesystem>

#include <wx/file.h>
#include <wx/utils.h>


namespace
{

class TEMPORARY_PROJECT
{
public:
    TEMPORARY_PROJECT()
    {
        m_root = wxFileName::DirName( wxFileName::GetTempDir() );
        m_root.AppendDir( wxS( "kichad-managed-symbol-qa-" ) + KIID().AsString() );
        BOOST_REQUIRE( wxFileName::Mkdir( m_root.GetFullPath(), 0700 ) );
        m_libraries = m_root;
        m_libraries.AppendDir( wxS( "libraries" ) );
        BOOST_REQUIRE( wxFileName::Mkdir( m_libraries.GetFullPath(), 0700 ) );
        m_project.Assign( m_root.GetFullPath(), wxS( "Project.kicad_pro" ) );
    }

    ~TEMPORARY_PROJECT()
    {
        std::error_code error;
        const std::string utf8( m_root.GetFullPath().ToUTF8() );
        const std::u8string path( reinterpret_cast<const char8_t*>( utf8.data() ), utf8.size() );
        std::filesystem::remove_all( std::filesystem::path( path ), error );
    }

    const wxFileName& Project() const { return m_project; }
    wxString Root() const { return m_root.GetFullPath(); }
    const wxFileName& Libraries() const { return m_libraries; }

private:
    wxFileName m_root;
    wxFileName m_libraries;
    wxFileName m_project;
};


class BUILD_CLI_ENVIRONMENT
{
public:
    BUILD_CLI_ENVIRONMENT()
    {
        m_hadPrevious = wxGetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), &m_previous );
        wxSetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), wxS( "1" ) );
    }

    ~BUILD_CLI_ENVIRONMENT()
    {
        if( m_hadPrevious )
            wxSetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ), m_previous );
        else
            wxUnsetEnv( wxS( "KICAD_RUN_FROM_BUILD_DIR" ) );
    }

private:
    bool m_hadPrevious = false;
    wxString m_previous;
};

} // namespace


BOOST_AUTO_TEST_SUITE( ManagedSymbolLibraryIo )


BOOST_AUTO_TEST_CASE( AtomicallyInstallsAndNativeLoadsGeneratedCurrentFormat )
{
    TEMPORARY_PROJECT project;
    BUILD_CLI_ENVIRONMENT environment;
    const std::string program = R"KDS((kichad_design
  (version 1) (project Project)
  (library symbol Product (table project)
    (uri "${KIPRJMOD}/libraries/Product.kicad_sym") (managed true))
  (symbol Product:R
    (reference R) (value R) (description "Resistor")
    (unit common
      (rectangle body (from -1.016mm -2.54mm) (to 1.016mm 2.54mm)
        (stroke 0.254mm default) (fill none))
      (circle center_mark (center 0mm 0mm) (radius 0.5mm))
      (arc top_arc (start -1mm 1mm) (mid 0mm 2mm) (end 1mm 1mm))
      (bezier accent (start -1mm -1mm) (control1 -0.5mm -2mm)
        (control2 0.5mm -2mm) (end 1mm -1mm))
      (polyline arrow (point -0.5mm 0mm) (point 0mm 0.5mm) (point 0.5mm 0mm))
      (text label "RESISTOR" (at 0mm 0mm) (rotation 90deg) (size 1mm 1.2mm)
        (thickness 0.1mm) (bold true) (italic true) (line_spacing 1.1)
        (color 10 20 30 0.75) (justify left top)
        (hyperlink "https://example.test/resistor"))
      (text_box note "R" (at -1mm -1mm) (box_size 2mm 2mm)
        (rotation 12.5deg) (margins 0.1mm 0.1mm 0.1mm 0.1mm)
        (stroke 0.1mm dash (color 40 50 60 0.5))
        (fill color (color 70 80 90 0.25))
        (justify right bottom) (hyperlink "#2")))
    (unit 1
      (pin 1 (at 0mm 3.81mm) (orientation up) (length 1.27mm))
      (pin 2 (at 0mm -3.81mm) (orientation down) (length 1.27mm)))))
)KDS";
    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( program );
    BOOST_REQUIRE_MESSAGE( compiled.ok, compiled.diagnostics.dump( 2 ) );
    KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT generated =
            KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( compiled.ir );
    BOOST_REQUIRE_MESSAGE( generated.ok, generated.diagnostics.dump( 2 ) );

    wxFileName resolved;
    std::string relative;
    std::string error;
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_SYMBOL_LIBRARY_IO::Resolve(
                                   project.Root(),
                                   "${KIPRJMOD}/libraries/Product.kicad_sym",
                                   resolved, relative, error ), error );
    BOOST_CHECK_EQUAL( relative, "libraries/Product.kicad_sym" );
    BOOST_CHECK( !resolved.FileExists() );
    const std::string source = generated.sources["Product"].get<std::string>();
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_SYMBOL_LIBRARY_IO::InstallAtomically(
                                   resolved, true, source, error ), error );
    BOOST_REQUIRE( resolved.FileExists() );
    bool present = false;
    std::string readBack;
    BOOST_REQUIRE_MESSAGE( KICHAD::MANAGED_SYMBOL_LIBRARY_IO::ReadOptional(
                                   resolved, present, readBack, error ), error );
    BOOST_CHECK( present );
    BOOST_CHECK_EQUAL( readBack, source );
    BOOST_CHECK_MESSAGE( KICHAD::MANAGED_SYMBOL_LIBRARY_IO::ValidateNative( resolved, error ),
                         error );
    BOOST_CHECK_MESSAGE( KICHAD::MANAGED_SYMBOL_LIBRARY_IO::InstallAtomically(
                                 resolved, false, {}, error ), error );
    BOOST_CHECK( !resolved.FileExists() );
}


BOOST_AUTO_TEST_CASE( RejectsTraversalMissingParentsAndNativeGarbage )
{
    TEMPORARY_PROJECT project;
    BUILD_CLI_ENVIRONMENT environment;
    wxFileName resolved;
    std::string relative;
    std::string error;
    BOOST_CHECK( !KICHAD::MANAGED_SYMBOL_LIBRARY_IO::Resolve(
            project.Root(), "${KIPRJMOD}/../escape.kicad_sym",
            resolved, relative, error ) );
    error.clear();
    BOOST_CHECK( !KICHAD::MANAGED_SYMBOL_LIBRARY_IO::Resolve(
            project.Root(), "${KIPRJMOD}/missing/Bad.kicad_sym",
            resolved, relative, error ) );

    error.clear();
    BOOST_REQUIRE( KICHAD::MANAGED_SYMBOL_LIBRARY_IO::Resolve(
            project.Root(), "${KIPRJMOD}/libraries/Bad.kicad_sym",
            resolved, relative, error ) );
    BOOST_REQUIRE( KICHAD::MANAGED_SYMBOL_LIBRARY_IO::InstallAtomically(
            resolved, true, "not a symbol library\n", error ) );
    error.clear();
    BOOST_CHECK( !KICHAD::MANAGED_SYMBOL_LIBRARY_IO::ValidateNative( resolved, error ) );
    BOOST_CHECK_NE( error.find( "rejected" ), std::string::npos );
}


BOOST_AUTO_TEST_SUITE_END()
