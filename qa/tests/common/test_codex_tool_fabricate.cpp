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

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <kicad/codex/codex_tool_registry.h>

#include <algorithm>
#include <build_version.h>
#include <filesystem>
#include <fstream>
#include <kiid.h>
#include <map>
#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

using JSON = nlohmann::json;


JSON envelope( const JSON& aResult )
{
    return JSON::parse( aResult.at( "contentItems" ).at( 0 ).at( "text" ).get<std::string>() );
}


std::string readText( const wxFileName& aPath )
{
    wxFFile input( aPath.GetFullPath(), wxS( "rb" ) );
    BOOST_REQUIRE( input.IsOpened() );
    wxString text;
    BOOST_REQUIRE( input.ReadAll( &text, wxConvUTF8 ) );
    return text.ToStdString();
}


class FABRICATION_PROJECT_FIXTURE
{
public:
    FABRICATION_PROJECT_FIXTURE()
    {
        wxFileName root = wxFileName::DirName( wxFileName::GetTempDir() );
        root.AppendDir( wxS( "kichad-fabrication-tool-" ) + KIID().AsString() );
        m_root = root.GetFullPath();
        BOOST_REQUIRE( wxFileName::Mkdir( m_root, 0700, wxPATH_MKDIR_FULL ) );
        Write( wxS( "design.kicad_pro" ), "{}\n" );
        Write( wxS( "design.kicad_prl" ), "original local settings\n" );
        Write( wxS( "design.kicad_sch" ),
               "(kicad_sch (version 20260306)\n"
               "  (generator \"eeschema\")\n"
               "  (generator_version \"10.0\")\n"
               ")\n" );
        Write( wxS( "design.kicad_pcb" ),
               R"PCB((kicad_pcb
  (version 20260206)
  (generator "pcbnew")
  (generator_version "10.0")
  (general (thickness 1.6))
  (layers
    (0 "F.Cu" signal)
    (2 "B.Cu" signal)
    (13 "F.Paste" user)
    (15 "B.Paste" user)
    (5 "F.SilkS" user "F.Silkscreen")
    (7 "B.SilkS" user "B.Silkscreen")
    (1 "F.Mask" user)
    (3 "B.Mask" user)
    (25 "Edge.Cuts" user))
  (setup
    (stackup
      (layer "F.SilkS" (type "Top Silk Screen")
        (color "White") (material "Epoxy ink"))
      (layer "F.Paste" (type "Top Solder Paste"))
      (layer "F.Mask" (type "Top Solder Mask") (color "Green")
        (thickness 0.01) (material "LPI") (epsilon_r 3.5) (loss_tangent 0.025))
      (layer "F.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 1" (type "core") (thickness 1.51 locked)
        (material "FR4") (epsilon_r 4.5) (loss_tangent 0.02))
      (layer "B.Cu" (type "copper") (thickness 0.035))
      (layer "B.Mask" (type "Bottom Solder Mask") (color "Green")
        (thickness 0.01) (material "LPI") (epsilon_r 3.5) (loss_tangent 0.025))
      (layer "B.Paste" (type "Bottom Solder Paste"))
      (layer "B.SilkS" (type "Bottom Silk Screen")
        (color "White") (material "Epoxy ink"))
      (copper_finish "ENIG")
      (dielectric_constraints no))))
)PCB" );
    }

    ~FABRICATION_PROJECT_FIXTURE()
    {
        wxFileName::Rmdir( m_root, wxPATH_RMDIR_RECURSIVE );
    }

    const wxString& Root() const { return m_root; }

    void Write( const wxString& aRelativePath, const std::string& aText ) const
    {
        wxFileName path( m_root + wxFILE_SEP_PATH + aRelativePath );

        if( !path.GetPath().IsEmpty() )
            BOOST_REQUIRE( wxFileName::Mkdir( path.GetPath(), 0700, wxPATH_MKDIR_FULL ) );

        wxFFile output( path.GetFullPath(), wxS( "wb" ) );
        BOOST_REQUIRE( output.IsOpened() );
        BOOST_REQUIRE_EQUAL( output.Write( aText.data(), aText.size() ), aText.size() );
    }

private:
    wxString m_root;
};


std::string productionKds( const std::string& aVerifiedOn )
{
    return R"KDS((kichad_design
  (version 1)
  (project design)
  (component U1
    (symbol "Amplifier_Operational:LM358")
    (value "LM358")
    (footprint "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"))
  (source U1
    (manufacturer "Texas Instruments")
    (mpn "LM358DR")
    (datasheet "https://www.ti.com/lit/ds/symlink/lm358.pdf")
    (lifecycle active)
    (supplier "DigiKey")
    (sku "296-1395-1-ND")
    (product_url "https://www.digikey.com/en/products/detail/texas-instruments/LM358DR/277042")
    (available 1000)
    (verified_on )KDS"
           + aVerifiedOn
           + R"KDS()
    (quantity 1))
  (board
    (stackup
      (finish "ENIG")
      (impedance_controlled false)
      (edge_connector none)
      (edge_plating false)
      (layers
        (silkscreen F.SilkS (material "Epoxy ink") (color "White"))
        (solderpaste F.Paste)
        (soldermask F.Mask (thickness 10um) (material "LPI")
          (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.51mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked true))
        (copper B.Cu (thickness 35um))
        (soldermask B.Mask (thickness 10um) (material "LPI")
          (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))
        (solderpaste B.Paste)
        (silkscreen B.SilkS (material "Epoxy ink") (color "White")))))
  (check erc)
  (check drc)
  (check sourcing)
  (check fabrication)
  (output gerbers)
  (output drill)
  (output pick_place)
  (output bom)
  (output step)
  (output pdf))
)KDS";
}


std::string nativeReport( const std::string& aCheck, const wxFileName& aPath,
                          bool aDirty, bool aWaiver )
{
    JSON report = { { "$schema", aCheck == "erc"
                                         ? "https://schemas.kicad.org/erc.v1.json"
                                         : "https://schemas.kicad.org/drc.v1.json" },
                    { "source", aPath.GetFullName().ToStdString() },
                    { "date", "volatile test value" },
                    { "kicad_version",
                      std::string( GetMajorMinorPatchVersion().ToUTF8() ) },
                    { "coordinate_units", "mm" },
                    { "included_severities",
                      JSON::array( { "error", "warning", "exclusion" } ) },
                    { "ignored_checks", JSON::array() } };
    JSON violations = JSON::array();

    if( aDirty )
    {
        violations.push_back(
                { { "type", "injected_failure" },
                  { "description", "Injected release-gate failure" },
                  { "severity", "error" },
                  { "items",
                    JSON::array( { { { "uuid", "11111111-1111-4111-8111-111111111111" },
                                      { "description", "Injected item" },
                                      { "pos", { { "x", 1.0 }, { "y", 2.0 } } } } } ) } } );
    }

    if( aWaiver )
    {
        report["ignored_checks"].push_back(
                { { "key", "approved_test_waiver" }, { "description", "QA waiver" } } );
    }

    if( aCheck == "erc" )
    {
        report["sheets"] = JSON::array(
                { { { "path", "/" }, { "uuid_path", "/root" },
                    { "violations", std::move( violations ) } } } );
    }
    else
    {
        report["violations"] = std::move( violations );
        report["unconnected_items"] = JSON::array();
        report["schematic_parity"] = JSON::array();
    }

    return report.dump();
}


bool writeNativeArtifacts( const JSON& aPlan, const wxFileName& aStaging,
                           bool aMalformed, std::string& aError )
{
    const std::filesystem::path root( aStaging.GetFullPath().ToStdString() );
    const auto write = [&]( const std::filesystem::path& aPath, const std::string& aText )
    {
        std::error_code filesystemError;
        std::filesystem::create_directories( aPath.parent_path(), filesystemError );

        if( filesystemError )
            return false;

        std::ofstream output( aPath, std::ios::binary | std::ios::trunc );
        output.write( aText.data(), static_cast<std::streamsize>( aText.size() ) );
        output.flush();
        return static_cast<bool>( output );
    };

    for( const JSON& job : aPlan.at( "jobs" ) )
    {
        const std::string kind = job.at( "kind" ).get<std::string>();
        const std::filesystem::path output =
                root / job.at( "relativePath" ).get<std::string>();

        if( kind == "gerbers" )
        {
            size_t index = 0;
            JSON files = JSON::array();

            for( const JSON& layer : job.at( "layers" ) )
            {
                std::string name = layer.get<std::string>();
                std::replace( name.begin(), name.end(), '.', '_' );
                const std::string fileName = name + ".gbr";
                const std::string source =
                        "G04 #@! TF.GenerationSoftware,KiCad,Pcbnew,10.0.4*\n"
                        "%FSLAX46Y46*%\n"
                        + std::string( aMalformed && index == 0 ? "M00*\n" : "M02*\n" );

                if( !write( output / fileName, source ) )
                {
                    aError = "could not write fake Gerber";
                    return false;
                }

                files.push_back( { { "Path", fileName },
                                   { "FileFunction", "Other" },
                                   { "FilePolarity", "Positive" } } );
                ++index;
            }

            JSON gerberJob = {
                { "Header",
                  { { "GenerationSoftware",
                      { { "Vendor", "KiCad" }, { "Application", "Pcbnew" },
                        { "Version", "10.0.4" } } },
                    { "CreationDate", "deterministic QA value" } } },
                { "FilesAttributes", std::move( files ) }
            };

            if( !write( output / "design-job.gbrjob", gerberJob.dump( 2 ) + "\n" ) )
            {
                aError = "could not write fake Gerber job";
                return false;
            }
        }
        else if( kind == "drill" )
        {
            if( !write( output / "design-PTH.drl", "M48\nMETRIC\n%\nM30\n" )
                || !write( output / "design-PTH-drl_map.pdf", "%PDF-1.4\n%%EOF\n" )
                || !write( output / "drill-report.rpt", "Drill report for design.kicad_pcb\n" ) )
            {
                aError = "could not write fake drill outputs";
                return false;
            }
        }
        else if( kind == "pick_place" )
        {
            if( !write( output,
                        "Ref,Val,Package,PosX,PosY,Rot,Side\n"
                        "U1,LM358,SOIC-8,10.0,10.0,0.0,top\n" ) )
            {
                aError = "could not write fake position output";
                return false;
            }
        }
        else if( kind == "step" )
        {
            if( !write( output, "ISO-10303-21;\nEND-ISO-10303-21;\n" ) )
            {
                aError = "could not write fake STEP output";
                return false;
            }
        }
        else if( kind == "pdf" )
        {
            if( !write( output, "%PDF-1.4\n%%EOF\n" ) )
            {
                aError = "could not write fake documentation output";
                return false;
            }
        }
    }

    return true;
}


bool hasHiddenFabricationTransaction( const wxString& aRoot )
{
    for( const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator( aRoot.ToStdString() ) )
    {
        const std::string name = entry.path().filename().string();

        if( name.starts_with( ".kichad-fabrication-staging-" )
            || name.starts_with( ".kichad-fabrication-backup-" ) )
        {
            return true;
        }
    }

    return false;
}

} // namespace


BOOST_AUTO_TEST_SUITE( CodexToolFabricate )


BOOST_AUTO_TEST_CASE( RequestsConfirmationOnlyForFinalExport )
{
    BOOST_CHECK( !CODEX_TOOL_REGISTRY::RequiresFinalConfirmation(
            "fabricate", { { "operation", "plan" } } ) );
    BOOST_CHECK( CODEX_TOOL_REGISTRY::RequiresFinalConfirmation(
            "fabricate", { { "operation", "export" } } ) );
    BOOST_CHECK( !CODEX_TOOL_REGISTRY::RequiresFinalConfirmation(
            "verify", { { "operation", "export" } } ) );
    BOOST_CHECK( !CODEX_TOOL_REGISTRY::RequiresFinalConfirmation(
            "fabricate", { { "operation", 7 } } ) );
}


BOOST_AUTO_TEST_CASE( PlansCompleteProductionIntentAndRejectsLegacyNativeInputs )
{
    FABRICATION_PROJECT_FIXTURE fixture;
    CODEX_TOOL_REGISTRY registry( [&fixture]() { return fixture.Root(); },
                                  []() { return true; } );
    const std::string source = productionKds(
            std::string( wxDateTime::Today().FormatISODate().ToUTF8() ) );
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "design.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    const std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON arguments = { { "operation", "plan" },
                       { "path", "design.kicad_kds" },
                       { "boardPath", "design.kicad_pcb" },
                       { "schematicPath", "design.kicad_sch" },
                       { "expectedSha256", hash } };
    JSON planned = registry.Handle( "fabricate", arguments );
    BOOST_REQUIRE_MESSAGE( planned.at( "success" ).get<bool>(), planned.dump() );
    JSON data = envelope( planned )["data"];
    BOOST_CHECK( data["productionReady"].get<bool>() );
    BOOST_CHECK_EQUAL( data["profile"].get<std::string>(),
                       "kichad-production-10.0.4-v1" );
    BOOST_CHECK_EQUAL( data["nativeInputFormats"]["board"].get<std::string>(),
                       "20260206" );
    BOOST_REQUIRE_EQUAL( data["jobs"].size(), 6 );
    BOOST_CHECK( std::find( data["jobs"][0]["layers"].begin(),
                            data["jobs"][0]["layers"].end(), JSON( "F.Paste" ) )
                 != data["jobs"][0]["layers"].end() );

    wxFileName boardPath( fixture.Root(), wxS( "design.kicad_pcb" ) );
    const std::string validBoard = readText( boardPath );
    std::string mismatchedBoard = validBoard;
    const size_t nativeThickness = mismatchedBoard.find( "(thickness 1.6)" );
    BOOST_REQUIRE_NE( nativeThickness, std::string::npos );
    mismatchedBoard.replace( nativeThickness, std::string( "(thickness 1.6)" ).size(),
                             "(thickness 1.7)" );
    fixture.Write( wxS( "design.kicad_pcb" ), mismatchedBoard );
    JSON mismatched = registry.Handle( "fabricate", arguments );
    BOOST_REQUIRE_MESSAGE( mismatched.at( "success" ).get<bool>(), mismatched.dump() );
    JSON mismatchData = envelope( mismatched )["data"];
    BOOST_CHECK( !mismatchData["productionReady"].get<bool>() );
    BOOST_CHECK( !mismatchData["nativeBoardIntentValid"].get<bool>() );
    fixture.Write( wxS( "design.kicad_pcb" ), validBoard );

    fixture.Write( wxS( "design.kicad_pcb" ),
                   "(kicad_pcb (version 20240108) (generator \"pcbnew\"))\n" );
    JSON rejected = registry.Handle( "fabricate", arguments );
    BOOST_REQUIRE( !rejected.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( rejected )["error"]["code"].get<std::string>(),
                       "format_version_mismatch" );
}


BOOST_AUTO_TEST_CASE( GatesValidatesAndAtomicallyInstallsConfirmedFabrication )
{
    FABRICATION_PROJECT_FIXTURE fixture;
    std::string dirtyCheck;
    bool waiver = false;
    bool malformedArtifacts = false;
    bool fabricationFailure = false;
    bool mutateLiveInputDuringExport = false;
    bool checksUsedSnapshot = true;
    bool fabricationUsedSnapshot = true;
    int nativeCheckCalls = 0;
    int fabricationCalls = 0;
    CODEX_TOOL_REGISTRY registry(
            [&fixture]() { return fixture.Root(); }, []() { return true; }, {}, {},
            [&]( const std::string& aCheck, const wxFileName& aPath, std::string& aReport,
                 std::string& )
            {
                ++nativeCheckCalls;
                checksUsedSnapshot = checksUsedSnapshot && aPath.GetPath() != fixture.Root();

                if( aCheck == "drc" )
                {
                    std::ofstream localSettings( std::filesystem::path(
                            aPath.GetPath().ToStdString() ) / "design.kicad_prl",
                                                std::ios::binary | std::ios::trunc );
                    localSettings << "native check changed only its snapshot\n";
                }

                aReport = nativeReport( aCheck, aPath, dirtyCheck == aCheck, waiver );
                return true;
            },
            [&]( const wxFileName& aBoard, const JSON& aPlan, const wxFileName& aStaging,
                 std::string& aError )
            {
                ++fabricationCalls;
                fabricationUsedSnapshot =
                        fabricationUsedSnapshot && aBoard.GetPath() != fixture.Root();

                if( fabricationFailure )
                {
                    aError = "injected native exporter failure";
                    return false;
                }

                if( mutateLiveInputDuringExport )
                {
                    fixture.Write( wxS( "design.kicad_pcb" ),
                                   "(kicad_pcb (version 20260206)\n"
                                   "  (generator \"pcbnew\")\n"
                                   "  (generator_version \"10.0\"))\n" );
                }

                return writeNativeArtifacts( aPlan, aStaging, malformedArtifacts, aError );
            } );
    wxFileName liveBoardPath( fixture.Root(), wxS( "design.kicad_pcb" ) );
    const std::string liveBoardBefore = readText( liveBoardPath );
    const std::string source = productionKds(
            std::string( wxDateTime::Today().FormatISODate().ToUTF8() ) );
    JSON saved = registry.Handle( "design", { { "operation", "save" },
                                                { "path", "design.kicad_kds" },
                                                { "source", source } } );
    BOOST_REQUIRE_MESSAGE( saved.at( "success" ).get<bool>(), saved.dump() );
    const std::string hash = envelope( saved )["data"]["sourceSha256"].get<std::string>();
    JSON arguments = { { "operation", "export" },
                       { "path", "design.kicad_kds" },
                       { "boardPath", "design.kicad_pcb" },
                       { "schematicPath", "design.kicad_sch" },
                       { "expectedSha256", hash } };

    JSON noSnapshot = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                   false, wxString(), true );
    BOOST_REQUIRE( !noSnapshot.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( noSnapshot )["error"]["code"].get<std::string>(),
                       "snapshot_required" );
    JSON noPermission = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                     true, wxString(), false );
    BOOST_REQUIRE( !noPermission.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( noPermission )["error"]["code"].get<std::string>(),
                       "permission_required" );
    BOOST_CHECK_EQUAL( nativeCheckCalls, 0 );
    BOOST_CHECK_EQUAL( fabricationCalls, 0 );

    fixture.Write( wxS( "fabrication/user-sentinel.txt" ), "must be replaced after approval\n" );
    JSON exported = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                true, wxString(), true );
    BOOST_REQUIRE_MESSAGE( exported.at( "success" ).get<bool>(), exported.dump() );
    JSON exportData = envelope( exported )["data"];
    BOOST_CHECK_EQUAL( exportData["releaseStatus"].get<std::string>(), "clean" );
    BOOST_CHECK_EQUAL( nativeCheckCalls, 2 );
    BOOST_CHECK_EQUAL( fabricationCalls, 1 );
    BOOST_CHECK( !wxFileExists( fixture.Root() + wxFILE_SEP_PATH
                                + wxS( "fabrication/user-sentinel.txt" ) ) );
    wxFileName manifestPath( fixture.Root() + wxFILE_SEP_PATH
                             + wxS( "fabrication/manifest.json" ) );
    BOOST_REQUIRE( manifestPath.FileExists() );
    JSON manifest = JSON::parse( readText( manifestPath ) );
    BOOST_CHECK_EQUAL( manifest["schema"].get<std::string>(),
                       "kichad.fabrication-manifest.v1" );
    BOOST_CHECK_EQUAL( manifest["source"]["board"]["formatVersion"].get<std::string>(),
                       "20260206" );
    BOOST_CHECK_EQUAL( manifest["bomRows"].get<int>(), 1 );
    BOOST_CHECK( checksUsedSnapshot );
    BOOST_CHECK( fabricationUsedSnapshot );
    BOOST_CHECK_EQUAL( readText( wxFileName( fixture.Root(), wxS( "design.kicad_prl" ) ) ),
                       "original local settings\n" );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    const std::string firstManifest = readText( manifestPath );

    JSON repeated = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                true, wxString(), true );
    BOOST_REQUIRE_MESSAGE( repeated.at( "success" ).get<bool>(), repeated.dump() );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK_EQUAL( nativeCheckCalls, 4 );
    BOOST_CHECK_EQUAL( fabricationCalls, 2 );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );

    malformedArtifacts = true;
    JSON malformed = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                 true, wxString(), true );
    BOOST_REQUIRE( !malformed.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( malformed )["error"]["code"].get<std::string>(),
                       "artifact_validation_failed" );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 3 );

    malformedArtifacts = false;
    fabricationFailure = true;
    JSON failedExport = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                    true, wxString(), true );
    BOOST_REQUIRE( !failedExport.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( failedExport )["error"]["code"].get<std::string>(),
                       "export_failed" );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 4 );

    fabricationFailure = false;
    mutateLiveInputDuringExport = true;
    JSON staleInput = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                  true, wxString(), true );
    BOOST_REQUIRE( !staleInput.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( staleInput )["error"]["code"].get<std::string>(),
                       "stale_inputs" );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 5 );
    fixture.Write( wxS( "design.kicad_pcb" ), liveBoardBefore );
    mutateLiveInputDuringExport = false;

    dirtyCheck = "drc";
    JSON dirty = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                             true, wxString(), true );
    BOOST_REQUIRE( !dirty.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( dirty )["error"]["code"].get<std::string>(),
                       "drc_gate_failed" );
    BOOST_CHECK_EQUAL( fabricationCalls, 5 );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );

    dirtyCheck.clear();
    waiver = true;
    JSON unapprovedWaiver = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                        true, wxString(), true );
    BOOST_REQUIRE( !unapprovedWaiver.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( unapprovedWaiver )["error"]["code"].get<std::string>(),
                       "waiver_confirmation_required" );
    arguments["allowWaivers"] = true;
    JSON approvedWaiver = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                      true, wxString(), true );
    BOOST_REQUIRE_MESSAGE( approvedWaiver.at( "success" ).get<bool>(),
                           approvedWaiver.dump() );
    BOOST_CHECK_EQUAL( envelope( approvedWaiver )["data"]["releaseStatus"].get<std::string>(),
                       "waived" );
    BOOST_CHECK_EQUAL( JSON::parse( readText( manifestPath ) )["releaseStatus"].get<std::string>(),
                       "waived" );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
}


BOOST_AUTO_TEST_CASE( ExportsWithSiblingNativeKiCadCliWhenExplicitlyRequested )
{
    wxString projectPath;

    if( !wxGetEnv( wxS( "KICHAD_QA_FABRICATION_PROJECT" ), &projectPath ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in native fabrication export test" );
        return;
    }

    wxFileName project = wxFileName::DirName( projectPath );
    project.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    BOOST_REQUIRE( project.DirExists() );
    wxFileName localSettingsPath( project.GetFullPath() + wxFILE_SEP_PATH
                                  + wxS( "fabrication_clean.kicad_prl" ) );
    BOOST_REQUIRE( localSettingsPath.FileExists() );
    const std::string localSettingsBefore = readText( localSettingsPath );
    CODEX_TOOL_REGISTRY registry( [project]() { return project.GetFullPath(); },
                                  []() { return true; } );
    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                   { "path",
                                                     "fabrication_clean.kicad_kds" } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_REQUIRE( compileData["valid"].get<bool>() );
    JSON arguments = {
        { "operation", "export" },
        { "path", "fabrication_clean.kicad_kds" },
        { "boardPath", "fabrication_clean.kicad_pcb" },
        { "schematicPath", "fabrication_clean.kicad_sch" },
        { "expectedSha256", compileData["sourceSha256"] },
        { "allowWaivers", true }
    };
    JSON exported = registry.HandleWithContext( "fabricate", arguments,
                                                project.GetFullPath(), true,
                                                wxString(), true );
    BOOST_REQUIRE_MESSAGE( exported.at( "success" ).get<bool>(), exported.dump() );
    JSON data = envelope( exported )["data"];
    BOOST_CHECK_EQUAL( data["releaseStatus"].get<std::string>(), "waived" );
    BOOST_CHECK_GE( data["artifactCount"].get<int>(), 16 );
    wxFileName manifestPath( project.GetFullPath() + wxFILE_SEP_PATH
                             + wxS( "fabrication/manifest.json" ) );
    BOOST_REQUIRE( manifestPath.FileExists() );
    JSON manifest = JSON::parse( readText( manifestPath ) );
    BOOST_CHECK_EQUAL( manifest["source"]["board"]["formatVersion"].get<std::string>(),
                       "20260206" );
    BOOST_CHECK_EQUAL( manifest["source"]["schematic"]["formatVersion"].get<std::string>(),
                       "20260306" );
    BOOST_CHECK_EQUAL( manifest["artifacts"].size(), data["artifactCount"].get<size_t>() );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/gerbers/"
                                      "fabrication_clean-job.gbrjob" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/assembly/"
                                      "fabrication_clean-bom.csv" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/model/fabrication_clean.step" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/documentation/"
                                      "fabrication_clean.pdf" ) ) );
    BOOST_CHECK_EQUAL( readText( localSettingsPath ), localSettingsBefore );
    BOOST_CHECK( !hasHiddenFabricationTransaction( project.GetFullPath() ) );
}


BOOST_AUTO_TEST_SUITE_END()
