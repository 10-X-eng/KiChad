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
#include <kicad/codex/kicad_ipc_client.h>

#include <algorithm>
#include <cstdint>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <build_version.h>
#include <filesystem>
#include <fstream>
#include <kiid.h>
#include <map>
#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>


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


void appendLittleEndian32( std::string& aOutput, uint32_t aValue )
{
    for( size_t byte = 0; byte < 4; ++byte )
        aOutput.push_back( static_cast<char>( aValue >> ( byte * 8 ) ) );
}


std::string mockGlb( const std::string& aStem )
{
    JSON scene = {
        { "asset",
          { { "version", "2.0" },
            { "extras", { { "generator", "KiCad 10.0.4-KiChad" },
                            { "pcb_name", aStem } } } } },
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
        Write( wxS( "Project.kicad_sym" ),
               "(kicad_symbol_lib (version 20251024)\n"
               "  (generator \"kicad_symbol_editor\")\n"
               "  (generator_version \"10.0\")\n"
               ")\n" );
        Write( wxS( "Project.pretty/Project.kicad_mod" ),
               "(footprint \"Project\"\n"
               "  (version 20260206)\n"
               "  (generator \"pcbnew\")\n"
               "  (generator_version \"10.0\")\n"
               "  (layer \"F.Cu\")\n"
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
      (dielectric_constraints no)))
  (footprint "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
    (layer "F.Cu")
    (uuid "11111111-2222-4333-8444-555555555555")
    (at 10 10)
    (property "Reference" "U1"
      (at 0 0 0)
      (layer "F.SilkS")
      (uuid "66666666-7777-4888-8999-aaaaaaaaaaaa")
      (effects (font (size 1 1) (thickness 0.15))))))
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
  (output ipcd356)
  (output netlist)
  (output ipc2581)
  (output odbpp)
  (output pick_place)
  (output bom)
  (output step)
  (output brep)
  (output glb)
  (output stl)
  (output xao)
  (output pdf)
  (output assembly_svg)
  (output assembly_dxf)
  (output gencad)
  (output vrml)
  (output board_stats))
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
                           bool aMalformed, bool aForbiddenXmlDeclaration,
                           bool aWrongIpc2581Reference, bool aUnsafeOdbPath,
                           bool aWrongPlacementReference, std::string& aError )
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
            JSON files = JSON::array();

            for( const JSON& layer : job.at( "layers" ) )
            {
                std::string name = layer.get<std::string>();
                std::replace( name.begin(), name.end(), '.', '_' );
                const std::string fileName = name + ".gbr";
                const std::string source =
                        "G04 #@! TF.GenerationSoftware,KiCad,Pcbnew,10.0.4*\n"
                        "%FSLAX46Y46*%\n"
                        "M02*\n";

                if( !write( output / fileName, source ) )
                {
                    aError = "could not write fake Gerber";
                    return false;
                }

                files.push_back( { { "Path", fileName },
                                   { "FileFunction", "Other" },
                                   { "FilePolarity", "Positive" } } );
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
        else if( kind == "ipcd356" )
        {
            const std::string source = "P  CODE 00\nP  UNITS CUST 0\n"
                                       "327NET1             U1    -1          "
                                       "A01X+000000Y+000000X0100Y0100R000S1\n"
                                       "999\n";

            if( !write( output, source ) )
            {
                aError = "could not write fake IPC-D-356 output";
                return false;
            }
        }
        else if( kind == "netlist" )
        {
            const std::string stem = aPlan.at( "fileStem" ).get<std::string>();
            std::string source =
                    "(export (version \"E\")"
                    " (design (source \"" + stem
                    + ".kicad_sch\") (tool \"Eeschema 10.0.4-KiChad\"))"
                      " (components";

            for( const JSON& reference : aPlan.at( "expectedNetlistReferences" ) )
                source += " (comp (ref " + reference.dump() + "))";

            source += ") (nets";
            size_t code = 1;

            for( const JSON& net : aPlan.at( "expectedNetlistNets" ) )
            {
                source += " (net (code \"" + std::to_string( code++ ) + "\") (name "
                          + net.at( "name" ).dump() + ")";

                for( const JSON& node : net.at( "nodes" ) )
                {
                    source += " (node (ref " + node.at( "reference" ).dump()
                              + ") (pin " + node.at( "pin" ).dump() + "))";
                }

                source += ")";
            }

            source += "))\n";

            if( !write( output, source ) )
            {
                aError = "could not write fake KiCad netlist output";
                return false;
            }
        }
        else if( kind == "ipc2581" )
        {
            const std::string stackup = aMalformed
                                                ? std::string()
                                                : "<Stackup name=\"Primary_Stackup\"/>";
            const std::string declaration =
                    aForbiddenXmlDeclaration
                            ? "<!DOCTYPE IPC-2581 [<!ENTITY unsafe \"forbidden\">]>\n"
                            : std::string();
            const std::string componentReference =
                    aWrongIpc2581Reference ? "U2" : "U1";
            const std::string source =
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    + declaration
                    + "<IPC-2581 revision=\"C\" xmlns=\"http://webstds.ipc.org/2581\" "
                    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
                    "xsi:schemaLocation=\"http://webstds.ipc.org/2581 "
                    "http://webstds.ipc.org/2581/IPC-2581C.xsd\">"
                    "<Content roleRef=\"Owner\"><StepRef name=\"design\"/></Content>"
                    "<LogisticHeader/>"
                    "<HistoryRecord><SoftwarePackage name=\"KiCad\" revision=\"10.0.4\" "
                    "vendor=\"KiCad EDA\"/></HistoryRecord>"
                    "<Bom><BomItem><RefDes name=\""
                    + componentReference
                    + "\"/></BomItem></Bom>"
                    "<Ecad><CadHeader units=\"MILLIMETER\"/><CadData>"
                    "<Layer name=\"F.Cu\" layerFunction=\"CONDUCTOR\"/>"
                    "<Layer name=\"B.Cu\" layerFunction=\"CONDUCTOR\"/>"
                    "<Layer name=\"Edge.Cuts\" layerFunction=\"BOARD_OUTLINE\"/>"
                    + stackup
                    + "<Step name=\"design\" type=\"BOARD\"><Component refDes=\""
                    + componentReference
                    + "\"/>"
                    + "</Step></CadData></Ecad><Avl/></IPC-2581>\n";

            if( !write( output, source ) )
            {
                aError = "could not write fake IPC-2581 output";
                return false;
            }
        }
        else if( kind == "odbpp" )
        {
            std::error_code filesystemError;
            std::filesystem::create_directories( output.parent_path(), filesystemError );
            const std::string outputPath = output.string();
            wxFFileOutputStream archiveFile( wxString::FromUTF8( outputPath.c_str() ) );
            wxZipOutputStream archive( archiveFile );
            const auto putDirectory = [&]( const char* aPath )
            {
                return archive.PutNextDirEntry( wxString::FromUTF8( aPath ) );
            };
            const auto putFile = [&]( const char* aPath, const std::string& aSource )
            {
                if( !archive.PutNextEntry( wxString::FromUTF8( aPath ) ) )
                    return false;

                archive.Write( aSource.data(), aSource.size() );
                return archive.LastWrite() == aSource.size() && archive.CloseEntry();
            };
            static constexpr const char* DIRECTORIES[] = {
                "matrix/",
                "steps/",
                "steps/pcb/",
                "steps/pcb/layers/",
                "steps/pcb/layers/f.cu/",
                "steps/pcb/layers/b.cu/",
                "steps/pcb/layers/edge.cuts/",
                "steps/pcb/layers/comp_+_top/",
                "steps/pcb/netlists/",
                "steps/pcb/netlists/cadnet/",
                "steps/pcb/eda/",
                "fonts/",
                "misc/"
            };
            bool ok = !filesystemError && archiveFile.IsOk() && archive.IsOk();

            for( const char* directory : DIRECTORIES )
                ok = ok && putDirectory( directory );

            const std::string componentReference =
                    aWrongIpc2581Reference ? "U2" : "U1";
            ok = ok
                 && putFile( "matrix/matrix",
                             "STEP {\n    COL=1\n    NAME=PCB\n}\n"
                             "LAYER {\n    TYPE=SIGNAL\n    NAME=F.CU\n}\n"
                             "LAYER {\n    TYPE=SIGNAL\n    NAME=B.CU\n}\n"
                             "LAYER {\n    TYPE=DOCUMENT\n    NAME=EDGE.CUTS\n}\n" )
                 && putFile( "steps/pcb/stephdr",
                             "LEFT_ACTIVE=0\nX_ORIGIN=0\nY_ORIGIN=0\nUNITS=MM\n" )
                 && putFile( "steps/pcb/profile", "UNITS=MM\n#Num Features\nF 1\n" )
                 && putFile( "steps/pcb/eda/data",
                             "HDR KiCad EDA 10.0.4-KiChad\nUNITS=MM\nLYR f.cu b.cu\n" )
                 && putFile( "steps/pcb/netlists/cadnet/netlist",
                             "H optimize n staggered n\n#\n#Netlist points\n#\n" )
                 && putFile( "steps/pcb/layers/f.cu/features",
                             "UNITS=MM\n#Num Features\nF 0\n" )
                 && putFile( "steps/pcb/layers/b.cu/features",
                             "UNITS=MM\n#Num Features\nF 0\n" )
                 && putFile( "steps/pcb/layers/edge.cuts/features",
                             "UNITS=MM\n#Num Features\nF 1\n" )
                 && putFile( "steps/pcb/layers/comp_+_top/components",
                             "UNITS=MM\nCMP 0 10.0 -10.0 0 N " + componentReference
                                     + " PACKAGE\n" )
                 && putFile( "fonts/standard", "XSIZE 0.302000\nYSIZE 0.302000\n" )
                 && putFile( "misc/info",
                             "JOB_NAME=job\nUNITS=MM\nODB_VERSION_MAJOR=8\n"
                             "ODB_VERSION_MINOR=1\nODB_SOURCE=KiCad EDA\n"
                             "SAVE_APP=KiCad EDA 10.0.4-KiChad\n" );

            if( aUnsafeOdbPath )
                ok = ok && putFile( "../escape", "must be rejected\n" );

            ok = ok && archive.Close() && archiveFile.Close();

            if( !ok )
            {
                aError = "could not write fake ODB++ archive";
                return false;
            }
        }
        else if( kind == "pick_place" )
        {
            if( !write( output,
                        "Ref,Val,Package,PosX,PosY,Rot,Side\n"
                        + std::string( aWrongPlacementReference
                                               ? "U2,LM358,SOIC-8,10.0,10.0,0.0,top\n"
                                               : "U1,LM358,SOIC-8,10.0,10.0,0.0,top\n" ) ) )
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
        else if( kind == "brep" )
        {
            const std::string source =
                    "\nCASCADE Topology V1, (c) Matra-Datavision\n"
                    "Locations 1\nCurve2ds 0\nCurves 3\nPolygon3D 0\n"
                    "PolygonOnTriangulations 0\nSurfaces 1\nTriangulations 0\n"
                    "TShapes 1\n1100000\n+1 0 \n";

            if( !write( output, source ) )
            {
                aError = "could not write fake BREP output";
                return false;
            }
        }
        else if( kind == "glb" )
        {
            if( !write( output, mockGlb( aPlan.at( "fileStem" ).get<std::string>() ) ) )
            {
                aError = "could not write fake GLB output";
                return false;
            }
        }
        else if( kind == "stl" )
        {
            const std::string source =
                    "solid design\n facet normal 0 0 1\n  outer loop\n"
                    "   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n"
                    "  endloop\n endfacet\nendsolid design\n";

            if( !write( output, source ) )
            {
                aError = "could not write fake STL output";
                return false;
            }
        }
        else if( kind == "xao" )
        {
            const std::string stem = aPlan.at( "fileStem" ).get<std::string>();
            const std::string source =
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<XAO version=\"1.0\" author=\"KiCad\"><geometry name=\"" + stem
                    + "\"><shape format=\"BREP\"><![CDATA[\n"
                      "CASCADE Topology V1, (c) Matra-Datavision\n"
                      "Locations 1\nCurve2ds 0\nCurves 3\nPolygon3D 0\n"
                      "PolygonOnTriangulations 0\nSurfaces 1\nTriangulations 0\n"
                      "TShapes 1\n+1 0\n"
                      "]]></shape><topology><vertices count=\"1\">"
                      "<vertex index=\"0\" reference=\"1\"/></vertices>"
                      "<edges count=\"0\"/><faces count=\"1\">"
                      "<face index=\"0\" reference=\"1\"/></faces>"
                      "<solids count=\"1\"><solid index=\"0\" reference=\"1\"/>"
                      "</solids></topology></geometry><groups count=\"0\"/>"
                      "<fields count=\"0\"/></XAO>\n";

            if( !write( output, source ) )
            {
                aError = "could not write fake XAO output";
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
        else if( kind == "assembly_svg" )
        {
            const std::string stem = aPlan.at( "fileStem" ).get<std::string>();
            const std::string source =
                    "<?xml version=\"1.0\"?>\n"
                    "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\" "
                    "width=\"10mm\" height=\"10mm\" viewBox=\"0 0 10 10\">"
                    "<desc>Image generated by PCBNEW</desc>"
                    "<path d=\"M 0 0 L 10 0 L 10 10 Z\"/>"
                    "</svg>\n";

            if( !write( output / ( stem + "-F_Fab.svg" ), source )
                || !write( output / ( stem + "-B_Fab.svg" ), source ) )
            {
                aError = "could not write fake assembly SVG outputs";
                return false;
            }
        }
        else if( kind == "assembly_dxf" )
        {
            const std::string stem = aPlan.at( "fileStem" ).get<std::string>();
            const std::string source =
                    "  0\nSECTION\n  2\nHEADER\n  0\nENDSEC\n"
                    "  0\nSECTION\n  2\nENTITIES\n  0\nLINE\n"
                    " 10\n0.0\n 20\n0.0\n 11\n10.0\n 21\n10.0\n"
                    "  0\nENDSEC\n  0\nEOF\n";

            if( !write( output / ( stem + "-F_Fab.dxf" ), source )
                || !write( output / ( stem + "-B_Fab.dxf" ), source ) )
            {
                aError = "could not write fake assembly DXF outputs";
                return false;
            }
        }
        else if( kind == "gencad" )
        {
            const std::string source =
                    "$HEADER\nGENCAD 1.4\n$ENDHEADER\n"
                    "$BOARD\nLINE 0 0 1 0\n$ENDBOARD\n"
                    "$COMPONENTS\n$ENDCOMPONENTS\n"
                    "$SIGNALS\n$ENDSIGNALS\n"
                    "$TRACKS\n$ENDTRACKS\n"
                    "$ROUTES\n$ENDROUTES\n";

            if( !write( output, source ) )
            {
                aError = "could not write fake GenCAD output";
                return false;
            }
        }
        else if( kind == "vrml" )
        {
            const std::string source =
                    "#VRML V2.0 utf8\n"
                    "Transform { children [ Shape { geometry IndexedFaceSet { "
                    "coord Coordinate { point [ 0 0 0, 1 0 0, 0 1 0 ] } "
                    "coordIndex [ 0, 1, 2, -1 ] } } ] }\n";

            if( !write( output, source ) )
            {
                aError = "could not write fake VRML output";
                return false;
            }
        }
        else if( kind == "board_stats" )
        {
            const std::string stem = aPlan.at( "fileStem" ).get<std::string>();
            const JSON source = {
                { "metadata",
                  { { "date", "2026-07-19T00:00:00" },
                    { "generator", "KiCad 10.0.4-KiChad" },
                    { "project", stem }, { "board_name", stem } } },
                { "board",
                  { { "has_outline", true }, { "width", "10.0000 mm" },
                    { "height", "10.0000 mm" }, { "area", "100.000 mm²" },
                    { "board_thickness", "1.6000 mm" } } },
                { "pads",
                  { { "through_hole", 0 }, { "smd", 8 }, { "connector", 0 },
                    { "npth", 0 }, { "castellated", 0 }, { "press_fit", 0 } } },
                { "vias",
                  { { "through", 0 }, { "blind", 0 }, { "buried", 0 },
                    { "micro", 0 } } },
                { "components",
                  { { "tht", { { "front", 0 }, { "back", 0 }, { "total", 0 } } },
                    { "smd", { { "front", 1 }, { "back", 0 }, { "total", 1 } } },
                    { "unspecified",
                      { { "front", 0 }, { "back", 0 }, { "total", 0 } } },
                    { "total", { { "front", 1 }, { "back", 0 }, { "total", 1 } } } } },
                { "drill_holes", JSON::array() }
            };

            if( !write( output, source.dump( 2 ) + "\n" ) )
            {
                aError = "could not write fake board statistics output";
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
                       "kichad-production-10.0.4-v7" );
    BOOST_CHECK_EQUAL( data["nativeInputFormats"]["board"].get<std::string>(),
                       "20260206" );
    BOOST_REQUIRE_EQUAL( data["jobs"].size(), 19 );
    BOOST_CHECK_EQUAL( data["expectedBomReferences"].size(), 1 );
    BOOST_CHECK_EQUAL( data["expectedBomReferences"][0].get<std::string>(), "U1" );
    BOOST_CHECK_EQUAL( data["expectedPlacementReferences"].size(), 1 );
    BOOST_CHECK_EQUAL( data["expectedPlacementReferences"][0].get<std::string>(), "U1" );
    BOOST_CHECK_EQUAL( data["expectedNetlistReferences"].size(), 1 );
    BOOST_CHECK_EQUAL( data["expectedNetlistReferences"][0].get<std::string>(), "U1" );
    BOOST_CHECK( data["expectedNetlistNets"].empty() );
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

    std::string mismatchedFootprint = validBoard;
    const std::string expectedLibraryId =
            "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm";
    const size_t nativeFootprint = mismatchedFootprint.find( expectedLibraryId );
    BOOST_REQUIRE_NE( nativeFootprint, std::string::npos );
    mismatchedFootprint.replace( nativeFootprint, expectedLibraryId.size(),
                                 "Package_SO:SOIC-8_Wrong" );
    fixture.Write( wxS( "design.kicad_pcb" ), mismatchedFootprint );
    JSON footprintMismatch = registry.Handle( "fabricate", arguments );
    BOOST_REQUIRE_MESSAGE( footprintMismatch.at( "success" ).get<bool>(),
                           footprintMismatch.dump() );
    JSON footprintMismatchData = envelope( footprintMismatch )["data"];
    BOOST_CHECK( !footprintMismatchData["productionReady"].get<bool>() );
    BOOST_CHECK( !footprintMismatchData["nativeBoardIntentValid"].get<bool>() );
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
    bool forbiddenXmlDeclaration = false;
    bool wrongIpc2581Reference = false;
    bool unsafeOdbPath = false;
    bool wrongPlacementReference = false;
    bool fabricationFailure = false;
    bool mutateLiveInputDuringExport = false;
    bool checksUsedSnapshot = true;
    bool checksSawProjectLibraries = true;
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
                const std::filesystem::path snapshotRoot( aPath.GetPath().ToStdString() );
                checksSawProjectLibraries =
                        checksSawProjectLibraries
                        && std::filesystem::is_regular_file(
                                snapshotRoot / "Project.kicad_sym" )
                        && std::filesystem::is_regular_file(
                                snapshotRoot / "Project.pretty/Project.kicad_mod" );

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
            [&]( const wxFileName& aBoard, const wxFileName& aSchematic,
                 const JSON& aPlan, const wxFileName& aStaging, std::string& aError )
            {
                ++fabricationCalls;
                fabricationUsedSnapshot =
                        fabricationUsedSnapshot && aBoard.GetPath() != fixture.Root()
                        && aSchematic.GetPath() != fixture.Root();

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

                return writeNativeArtifacts( aPlan, aStaging, malformedArtifacts,
                                             forbiddenXmlDeclaration, wrongIpc2581Reference,
                                             unsafeOdbPath, wrongPlacementReference, aError );
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
    BOOST_CHECK( checksSawProjectLibraries );
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
    BOOST_CHECK_NE( envelope( malformed )["error"]["message"].get<std::string>().find(
                            "missing its native board, stackup, or producer structure" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 3 );

    malformedArtifacts = false;
    forbiddenXmlDeclaration = true;
    JSON forbiddenXml = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                     true, wxString(), true );
    BOOST_REQUIRE( !forbiddenXml.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( forbiddenXml )["error"]["code"].get<std::string>(),
                       "artifact_validation_failed" );
    BOOST_CHECK_NE( envelope( forbiddenXml )["error"]["message"].get<std::string>().find(
                            "forbidden document or entity declaration" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 4 );

    forbiddenXmlDeclaration = false;
    wrongIpc2581Reference = true;
    JSON mismatchedIpc2581 = registry.HandleWithContext( "fabricate", arguments,
                                                         fixture.Root(), true,
                                                         wxString(), true );
    BOOST_REQUIRE( !mismatchedIpc2581.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( mismatchedIpc2581 )["error"]["code"].get<std::string>(),
                       "artifact_validation_failed" );
    BOOST_CHECK_NE( envelope( mismatchedIpc2581 )["error"]["message"].get<std::string>().find(
                            "missing a compiled KDS component reference" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 5 );

    wrongIpc2581Reference = false;
    unsafeOdbPath = true;
    JSON unsafeOdb = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                 true, wxString(), true );
    BOOST_REQUIRE( !unsafeOdb.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( unsafeOdb )["error"]["code"].get<std::string>(),
                       "artifact_validation_failed" );
    BOOST_CHECK_NE( envelope( unsafeOdb )["error"]["message"].get<std::string>().find(
                            "unsafe entry path" ),
                    std::string::npos );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 6 );

    unsafeOdbPath = false;
    wrongPlacementReference = true;
    JSON mismatchedAssembly = registry.HandleWithContext( "fabricate", arguments,
                                                           fixture.Root(), true,
                                                           wxString(), true );
    BOOST_REQUIRE( !mismatchedAssembly.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( mismatchedAssembly )["error"]["code"].get<std::string>(),
                       "artifact_validation_failed" );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 7 );

    wrongPlacementReference = false;
    fabricationFailure = true;
    JSON failedExport = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                    true, wxString(), true );
    BOOST_REQUIRE( !failedExport.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( failedExport )["error"]["code"].get<std::string>(),
                       "export_failed" );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 8 );

    fabricationFailure = false;
    mutateLiveInputDuringExport = true;
    JSON staleInput = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                                  true, wxString(), true );
    BOOST_REQUIRE( !staleInput.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( staleInput )["error"]["code"].get<std::string>(),
                       "stale_inputs" );
    BOOST_CHECK_EQUAL( readText( manifestPath ), firstManifest );
    BOOST_CHECK( !hasHiddenFabricationTransaction( fixture.Root() ) );
    BOOST_CHECK_EQUAL( fabricationCalls, 9 );
    fixture.Write( wxS( "design.kicad_pcb" ), liveBoardBefore );
    mutateLiveInputDuringExport = false;

    dirtyCheck = "drc";
    JSON dirty = registry.HandleWithContext( "fabricate", arguments, fixture.Root(),
                                             true, wxString(), true );
    BOOST_REQUIRE( !dirty.at( "success" ).get<bool>() );
    BOOST_CHECK_EQUAL( envelope( dirty )["error"]["code"].get<std::string>(),
                       "drc_gate_failed" );
    BOOST_CHECK_EQUAL( fabricationCalls, 9 );
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
    BOOST_CHECK_GE( data["artifactCount"].get<int>(), 28 );
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
                               + wxS( "fabrication/electrical-test/"
                                      "fabrication_clean.d356" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/netlist/"
                                      "fabrication_clean.net" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/manufacturing/"
                                      "fabrication_clean.ipc2581.xml" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/manufacturing/"
                                      "fabrication_clean.odb.zip" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/assembly/"
                                      "fabrication_clean-bom.csv" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/model/fabrication_clean.step" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/model/fabrication_clean.brep" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/model/fabrication_clean.glb" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/model/fabrication_clean.stl" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/model/fabrication_clean.xao" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/documentation/"
                                      "fabrication_clean.pdf" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/assembly/drawings-svg/"
                                      "fabrication_clean-F_Fab.svg" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/assembly/drawings-svg/"
                                      "fabrication_clean-B_Fab.svg" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/assembly/drawings-dxf/"
                                      "fabrication_clean-F_Fab.dxf" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/assembly/drawings-dxf/"
                                      "fabrication_clean-B_Fab.dxf" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/interchange/"
                                      "fabrication_clean.cad" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/model/fabrication_clean.wrl" ) ) );
    BOOST_CHECK( wxFileExists( project.GetFullPath() + wxFILE_SEP_PATH
                               + wxS( "fabrication/reports/"
                                      "fabrication_clean-board-stats.json" ) ) );
    BOOST_CHECK_EQUAL( readText( localSettingsPath ), localSettingsBefore );
    BOOST_CHECK( !hasHiddenFabricationTransaction( project.GetFullPath() ) );
}


BOOST_AUTO_TEST_CASE( AppliesSavesAndFabricatesSourcedComponentWhenExplicitlyRequested )
{
    wxString projectPath;

    if( !wxGetEnv( wxS( "KICHAD_QA_FABRICATION_COMPONENT_PROJECT" ), &projectPath ) )
    {
        BOOST_TEST_MESSAGE( "Skipping opt-in KDS component fabrication test" );
        return;
    }

    wxFileName project = wxFileName::DirName( projectPath );
    project.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    BOOST_REQUIRE( project.DirExists() );
    const wxString projectName = wxS( "fabrication_component" );
    const wxString boardName = projectName + wxS( ".kicad_pcb" );
    const wxString schematicName = projectName + wxS( ".kicad_sch" );
    const wxString sourceName = projectName + wxS( ".kicad_kds" );
    wxFileName boardPath( project.GetFullPath(), boardName );
    wxFileName schematicPath( project.GetFullPath(), schematicName );
    wxFileName localSettingsPath( project.GetFullPath(),
                                  projectName + wxS( ".kicad_prl" ) );
    BOOST_REQUIRE( boardPath.FileExists() );
    BOOST_REQUIRE( schematicPath.FileExists() );
    BOOST_REQUIRE( localSettingsPath.FileExists() );
    BOOST_REQUIRE_NE( readText( boardPath ).find( "(version 20260206)" ),
                      std::string::npos );
    BOOST_REQUIRE_NE( readText( schematicPath ).find( "(version 20260306)" ),
                      std::string::npos );

    wxString socketDirectory;
    wxGetEnv( wxS( "KICHAD_QA_FABRICATION_COMPONENT_SOCKET_DIR" ), &socketDirectory );
    CODEX_TOOL_REGISTRY registry( [project]() { return project.GetFullPath(); },
                                  []() { return true; },
                                  [socketDirectory]() { return socketDirectory; },
                                  []( const wxFileName&, std::string& ) { return true; } );
    JSON compiled = registry.Handle( "design", { { "operation", "compile" },
                                                   { "path", sourceName.ToStdString() } } );
    BOOST_REQUIRE_MESSAGE( compiled.at( "success" ).get<bool>(), compiled.dump() );
    JSON compileData = envelope( compiled )["data"];
    BOOST_REQUIRE( compileData["valid"].get<bool>() );
    const std::string hash = compileData["sourceSha256"].get<std::string>();
    JSON applied = registry.Handle( "design", { { "operation", "apply" },
                                                  { "path", sourceName.ToStdString() },
                                                  { "boardPath", boardName.ToStdString() },
                                                  { "expectedSha256", hash } } );
    BOOST_REQUIRE_MESSAGE( applied.at( "success" ).get<bool>(), applied.dump() );
    JSON applyData = envelope( applied )["data"];
    BOOST_CHECK_EQUAL( applyData["counts"]["placement"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( applyData["transaction"].get<std::string>(), "committed" );
    BOOST_CHECK( applyData["stackupApplied"].get<bool>() );
    BOOST_CHECK_EQUAL( applyData["libraryTablesApplied"].get<int>(), 2 );

    KICHAD_IPC_CLIENT ipcClient( "org.kichad.codex.qa.fabrication-component",
                                socketDirectory );
    KICHAD_IPC_TARGET ipcTarget;
    std::string ipcError;
    BOOST_REQUIRE_MESSAGE(
            ipcClient.FindOpenPcb( project.GetFullPath(), boardPath.GetFullPath(),
                                   ipcTarget, ipcError ),
            ipcError );
    kiapi::common::commands::SaveDocument saveRequest;
    saveRequest.mutable_document()->CopyFrom( ipcTarget.document );
    kiapi::common::ApiResponse saveResponse;
    BOOST_REQUIRE_MESSAGE( ipcClient.Call( ipcTarget, saveRequest, saveResponse, ipcError ),
                           ipcError );

    const std::string savedBoard = readText( boardPath );
    BOOST_CHECK_NE( savedBoard.find( "(version 20260206)" ), std::string::npos );
    BOOST_CHECK_NE( savedBoard.find( "Resistor_SMD:R_0603_1608Metric" ),
                    std::string::npos );
    BOOST_CHECK_NE( savedBoard.find( "\"Reference\" \"R1\"" ), std::string::npos );
    BOOST_CHECK_NE( savedBoard.find( "\"Reference\" \"R2\"" ), std::string::npos );

    JSON nativeErc = registry.Handle(
            "verify", { { "operation", "erc" },
                          { "path", schematicName.ToStdString() } } );
    BOOST_REQUIRE_MESSAGE( nativeErc.at( "success" ).get<bool>(), nativeErc.dump() );
    JSON nativeErcData = envelope( nativeErc )["data"];
    BOOST_REQUIRE_MESSAGE( nativeErcData["clean"].get<bool>(), nativeErc.dump( 2 ) );
    BOOST_CHECK_EQUAL( nativeErcData["counts"]["total"].get<int>(), 0 );
    BOOST_CHECK( !nativeErcData["waiversPresent"].get<bool>() );

    JSON nativeDrc = registry.Handle(
            "verify", { { "operation", "drc" }, { "path", boardName.ToStdString() } } );
    BOOST_REQUIRE_MESSAGE( nativeDrc.at( "success" ).get<bool>(), nativeDrc.dump() );
    JSON nativeDrcData = envelope( nativeDrc )["data"];
    BOOST_REQUIRE_MESSAGE( nativeDrcData["clean"].get<bool>(), nativeDrc.dump( 2 ) );
    BOOST_CHECK_EQUAL( nativeDrcData["counts"]["total"].get<int>(), 0 );
    BOOST_CHECK( !nativeDrcData["waiversPresent"].get<bool>() );
    const std::string localSettingsBeforeExport = readText( localSettingsPath );

    JSON arguments = {
        { "operation", "export" },
        { "path", sourceName.ToStdString() },
        { "boardPath", boardName.ToStdString() },
        { "schematicPath", schematicName.ToStdString() },
        { "expectedSha256", hash }
    };
    JSON exported = registry.HandleWithContext( "fabricate", arguments,
                                                project.GetFullPath(), true,
                                                wxString(), true );
    BOOST_REQUIRE_MESSAGE( exported.at( "success" ).get<bool>(), exported.dump() );
    JSON data = envelope( exported )["data"];
    BOOST_CHECK_EQUAL( data["releaseStatus"].get<std::string>(), "clean" );
    BOOST_CHECK_GE( data["artifactCount"].get<int>(), 21 );

    wxFileName manifestPath( project.GetFullPath() + wxFILE_SEP_PATH
                             + wxS( "fabrication/manifest.json" ) );
    wxFileName bomPath( project.GetFullPath() + wxFILE_SEP_PATH
                        + wxS( "fabrication/assembly/fabrication_component-bom.csv" ) );
    wxFileName positionsPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/assembly/fabrication_component-positions.csv" ) );
    wxFileName electricalTestPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/electrical-test/fabrication_component.d356" ) );
    wxFileName netlistPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/netlist/fabrication_component.net" ) );
    wxFileName ipc2581Path(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/manufacturing/fabrication_component.ipc2581.xml" ) );
    wxFileName odbPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/manufacturing/fabrication_component.odb.zip" ) );
    wxFileName brepPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/model/fabrication_component.brep" ) );
    wxFileName glbPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/model/fabrication_component.glb" ) );
    wxFileName stlPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/model/fabrication_component.stl" ) );
    wxFileName xaoPath(
            project.GetFullPath() + wxFILE_SEP_PATH
            + wxS( "fabrication/model/fabrication_component.xao" ) );
    BOOST_REQUIRE( manifestPath.FileExists() );
    BOOST_REQUIRE( bomPath.FileExists() );
    BOOST_REQUIRE( positionsPath.FileExists() );
    BOOST_REQUIRE( electricalTestPath.FileExists() );
    BOOST_REQUIRE( netlistPath.FileExists() );
    BOOST_REQUIRE( ipc2581Path.FileExists() );
    BOOST_REQUIRE( odbPath.FileExists() );
    BOOST_REQUIRE( brepPath.FileExists() );
    BOOST_REQUIRE( glbPath.FileExists() );
    BOOST_REQUIRE( stlPath.FileExists() );
    BOOST_REQUIRE( xaoPath.FileExists() );
    JSON manifest = JSON::parse( readText( manifestPath ) );
    BOOST_CHECK_EQUAL( manifest["bomRows"].get<int>(), 2 );
    BOOST_CHECK_EQUAL( manifest["releaseStatus"].get<std::string>(), "clean" );
    BOOST_CHECK( !manifest["checks"]["erc"]["waiversPresent"].get<bool>() );
    BOOST_CHECK( !manifest["checks"]["drc"]["waiversPresent"].get<bool>() );
    const std::string electricalTest = readText( electricalTestPath );
    BOOST_CHECK_NE( electricalTest.find( "327TEST_A" ), std::string::npos );
    BOOST_CHECK_NE( electricalTest.find( "327TEST_B" ), std::string::npos );
    const std::string netlist = readText( netlistPath );
    BOOST_CHECK_NE( netlist.find( "(name \"TEST_A\")" ), std::string::npos );
    BOOST_CHECK_NE( netlist.find( "(name \"TEST_B\")" ), std::string::npos );
    BOOST_CHECK_NE( netlist.find( "(ref \"R1\")" ), std::string::npos );
    BOOST_CHECK_NE( netlist.find( "(ref \"R2\")" ), std::string::npos );
    const std::string ipc2581 = readText( ipc2581Path );
    BOOST_CHECK_NE( ipc2581.find( "<IPC-2581 revision=\"C\"" ), std::string::npos );
    BOOST_CHECK_NE( ipc2581.find( "<Component refDes=\"R1\"" ), std::string::npos );
    BOOST_CHECK_NE( ipc2581.find( "<Component refDes=\"R2\"" ), std::string::npos );
    const std::string bom = readText( bomPath );
    BOOST_CHECK_NE( bom.find( "\"R1\",\"10k\","
                              "\"Resistor_SMD:R_0603_1608Metric\",1,"
                              "\"KiChad QA Components\",\"KQA-0603-10K\"" ),
                    std::string::npos );
    BOOST_CHECK_NE( bom.find( "\"R2\",\"10k\","
                              "\"Resistor_SMD:R_0603_1608Metric\",1,"
                              "\"KiChad QA Components\",\"KQA-0603-10K\"" ),
                    std::string::npos );
    const std::string positions = readText( positionsPath );
    BOOST_CHECK_NE( positions.find( "R1" ), std::string::npos );
    BOOST_CHECK_NE( positions.find( "R2" ), std::string::npos );
    BOOST_CHECK_EQUAL( readText( localSettingsPath ), localSettingsBeforeExport );
    BOOST_CHECK( !hasHiddenFabricationTransaction( project.GetFullPath() ) );
}


BOOST_AUTO_TEST_SUITE_END()
