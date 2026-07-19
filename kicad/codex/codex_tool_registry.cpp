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

#include "codex_tool_registry.h"
#include "design_script_compiler.h"
#include "design_script_pcb_planner.h"
#include "kicad_ipc_client.h"
#include "lossless_sexpr_document.h"

#include <build_version.h>
#include <kiid.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <google/protobuf/util/field_mask_util.h>
#include <google/protobuf/util/json_util.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <picosha2.h>


namespace
{

constexpr wxFileOffset MAX_INSPECTION_BYTES = 16 * 1024 * 1024;
constexpr size_t       MAX_EXPRESSION_BYTES = 32 * 1024;
constexpr size_t       MAX_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DISTINCT_HEADS = 512;
constexpr size_t       MAX_PCB_ARGUMENT_BYTES = 1024 * 1024;
constexpr size_t       MAX_PCB_RESULT_BYTES = 256 * 1024;
constexpr size_t       MAX_DESIGN_SCRIPT_BYTES = 1024 * 1024;
constexpr size_t       MAX_DESIGN_RESULT_BYTES = 256 * 1024;


bool isInspectableExtension( const wxString& aExtension )
{
    return aExtension == wxS( "kicad_sch" ) || aExtension == wxS( "kicad_pcb" )
           || aExtension == wxS( "kicad_sym" ) || aExtension == wxS( "kicad_mod" );
}


bool canonicalizeExisting( wxFileName& aPath, bool aDirectory = false )
{
    std::error_code         error;
    const std::string       utf8Path( aPath.GetFullPath().ToUTF8() );
    const std::u8string     filesystemPath(
            reinterpret_cast<const char8_t*>( utf8Path.data() ), utf8Path.size() );
    std::filesystem::path   canonical =
            std::filesystem::canonical( std::filesystem::path( filesystemPath ), error );

    if( error )
        return false;

    const std::u8string utf8Canonical = canonical.generic_u8string();
    wxString canonicalPath = wxString::FromUTF8(
            reinterpret_cast<const char*>( utf8Canonical.data() ), utf8Canonical.size() );

    if( aDirectory )
        aPath.AssignDir( canonicalPath );
    else
        aPath.Assign( canonicalPath );

    return true;
}


std::string expectedRootHead( const wxString& aExtension )
{
    if( aExtension == wxS( "kicad_sch" ) )
        return "kicad_sch";

    if( aExtension == wxS( "kicad_pcb" ) )
        return "kicad_pcb";

    if( aExtension == wxS( "kicad_sym" ) )
        return "kicad_symbol_lib";

    if( aExtension == wxS( "kicad_mod" ) )
        return "footprint";

    return {};
}


bool resolveProjectFile( const wxString& aProjectPath, const std::string& aRelativePath,
                         wxFileName& aResolved, std::string& aError )
{
    wxString   relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.size() > 4096 || aRelativePath.find( '\0' ) != std::string::npos
        || relative.IsEmpty() || candidate.IsAbsolute() )
    {
        aError = "path must be project-relative";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !candidate.FileExists() )
    {
        aError = "file does not exist";
        return false;
    }

    if( !canonicalizeExisting( candidate ) )
    {
        aError = "path could not be resolved";
        return false;
    }

    wxString candidatePath = candidate.GetFullPath();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    candidatePath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !candidatePath.StartsWith( rootPath ) )
    {
        aError = "path resolves outside the active project";
        return false;
    }

    if( !isInspectableExtension( candidate.GetExt() ) )
    {
        aError = "file type is not a supported KiCad s-expression format";
        return false;
    }

    aResolved = candidate;
    return true;
}


bool resolveProjectSidecar( const wxString& aProjectPath, const std::string& aRelativePath,
                            wxFileName& aResolved, std::string& aError )
{
    wxString   relative = wxString::FromUTF8( aRelativePath );
    wxFileName candidate( relative );

    if( aRelativePath.size() > 4096 || aRelativePath.find( '\0' ) != std::string::npos
        || relative.IsEmpty() || candidate.IsAbsolute()
        || candidate.GetExt() != wxS( "kicad_kds" ) )
    {
        aError = "path must be a project-relative .kicad_kds file";
        return false;
    }

    wxFileName root = wxFileName::DirName( aProjectPath );
    root.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    if( !canonicalizeExisting( root, true ) )
    {
        aError = "active project path could not be resolved";
        return false;
    }

    candidate.MakeAbsolute( root.GetFullPath() );
    candidate.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );

    wxFileName parent = wxFileName::DirName( candidate.GetPath() );

    if( !canonicalizeExisting( parent, true ) )
    {
        aError = "sidecar parent directory does not exist";
        return false;
    }

    wxString parentPath = parent.GetPathWithSep();
    wxString rootPath = root.GetPathWithSep();

#ifdef __WXMSW__
    parentPath.MakeLower();
    rootPath.MakeLower();
#endif

    if( !parentPath.StartsWith( rootPath ) && parent.GetFullPath() != root.GetFullPath() )
    {
        aError = "path resolves outside the active project";
        return false;
    }

    aResolved.Assign( parent.GetFullPath(), candidate.GetFullName() );

    if( aResolved.FileExists() )
    {
        wxFileName canonicalTarget = aResolved;

        if( !canonicalizeExisting( canonicalTarget ) )
        {
            aError = "existing sidecar path could not be resolved";
            return false;
        }

        wxString targetPath = canonicalTarget.GetFullPath();

#ifdef __WXMSW__
        targetPath.MakeLower();
#endif

        if( !targetPath.StartsWith( rootPath ) )
        {
            aError = "existing sidecar resolves outside the active project";
            return false;
        }

        aResolved = canonicalTarget;
    }

    return true;
}


kiapi::common::types::KiCadObjectType pcbObjectType( const std::string& aItemType )
{
    using namespace kiapi::common::types;

    if( aItemType == "footprint" )
        return KOT_PCB_FOOTPRINT;
    if( aItemType == "trace" )
        return KOT_PCB_TRACE;
    if( aItemType == "via" )
        return KOT_PCB_VIA;
    if( aItemType == "arc" )
        return KOT_PCB_ARC;
    if( aItemType == "zone" )
        return KOT_PCB_ZONE;
    if( aItemType == "shape" )
        return KOT_PCB_SHAPE;
    if( aItemType == "text" )
        return KOT_PCB_TEXT;

    return KOT_UNKNOWN;
}


std::unique_ptr<google::protobuf::Message> newPcbItem( const std::string& aItemType )
{
    using namespace kiapi::board::types;

    if( aItemType == "footprint" )
        return std::make_unique<FootprintInstance>();
    if( aItemType == "trace" )
        return std::make_unique<Track>();
    if( aItemType == "via" )
        return std::make_unique<Via>();
    if( aItemType == "arc" )
        return std::make_unique<Arc>();
    if( aItemType == "zone" )
        return std::make_unique<Zone>();
    if( aItemType == "shape" )
        return std::make_unique<BoardGraphicShape>();
    if( aItemType == "text" )
        return std::make_unique<BoardText>();

    return {};
}


std::string protobufFieldType( const google::protobuf::FieldDescriptor& aField )
{
    using TYPE = google::protobuf::FieldDescriptor::Type;

    switch( aField.type() )
    {
    case TYPE::TYPE_DOUBLE:   return "double";
    case TYPE::TYPE_FLOAT:    return "float";
    case TYPE::TYPE_INT64:    return "int64";
    case TYPE::TYPE_UINT64:   return "uint64";
    case TYPE::TYPE_INT32:    return "int32";
    case TYPE::TYPE_FIXED64:  return "fixed64";
    case TYPE::TYPE_FIXED32:  return "fixed32";
    case TYPE::TYPE_BOOL:     return "bool";
    case TYPE::TYPE_STRING:   return "string";
    case TYPE::TYPE_GROUP:    return "group";
    case TYPE::TYPE_MESSAGE:  return "message";
    case TYPE::TYPE_BYTES:    return "bytes";
    case TYPE::TYPE_UINT32:   return "uint32";
    case TYPE::TYPE_ENUM:     return "enum";
    case TYPE::TYPE_SFIXED32: return "sfixed32";
    case TYPE::TYPE_SFIXED64: return "sfixed64";
    case TYPE::TYPE_SINT32:   return "sint32";
    case TYPE::TYPE_SINT64:   return "sint64";
    }

    return "unknown";
}


nlohmann::json describePcbMessage( const std::string& aItemType,
                                   const std::string& aMessagePath, std::string& aError )
{
    std::unique_ptr<google::protobuf::Message> message = newPcbItem( aItemType );

    if( !message )
    {
        aError = "unsupported PCB item type";
        return {};
    }

    const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
    std::string                         normalizedPath;
    size_t                              begin = 0;
    size_t                              depth = 0;

    while( begin < aMessagePath.size() )
    {
        size_t end = aMessagePath.find( '.', begin );

        if( end == std::string::npos )
            end = aMessagePath.size();

        std::string segment = aMessagePath.substr( begin, end - begin );

        if( segment.empty() || ++depth > 8 )
        {
            aError = "pcb.messagePath must contain at most 8 non-empty field names";
            return {};
        }

        const google::protobuf::FieldDescriptor* selected = nullptr;

        for( int i = 0; i < descriptor->field_count(); ++i )
        {
            const google::protobuf::FieldDescriptor* field = descriptor->field( i );

            if( field->name() == segment || field->json_name() == segment )
            {
                selected = field;
                break;
            }
        }

        if( !selected || selected->cpp_type()
                                 != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            aError = "pcb.messagePath does not identify a message-typed field";
            return {};
        }

        if( !normalizedPath.empty() )
            normalizedPath += '.';

        normalizedPath += selected->json_name();
        descriptor = selected->message_type();
        begin = end + 1;
    }

    nlohmann::json fields = nlohmann::json::array();

    for( int i = 0; i < descriptor->field_count(); ++i )
    {
        const google::protobuf::FieldDescriptor* field = descriptor->field( i );
        nlohmann::json fieldDescription = {
            { "name", field->json_name() },
            { "protoName", field->name() },
            { "number", field->number() },
            { "type", protobufFieldType( *field ) },
            { "cardinality", field->is_repeated() ? "repeated" : "singular" }
        };

        if( field->containing_oneof() )
            fieldDescription["oneof"] = field->containing_oneof()->name();

        if( field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            fieldDescription["messageType"] = field->message_type()->full_name();
            fieldDescription["expandWith"] = normalizedPath.empty()
                                                     ? field->json_name()
                                                     : normalizedPath + '.' + field->json_name();
        }
        else if( field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM )
        {
            nlohmann::json values = nlohmann::json::array();

            for( int valueIndex = 0; valueIndex < field->enum_type()->value_count(); ++valueIndex )
            {
                const google::protobuf::EnumValueDescriptor* value =
                        field->enum_type()->value( valueIndex );
                values.push_back( { { "name", value->name() }, { "number", value->number() } } );
            }

            fieldDescription["enumType"] = field->enum_type()->full_name();
            fieldDescription["values"] = std::move( values );
        }

        if( field->type() == google::protobuf::FieldDescriptor::TYPE_INT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_UINT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_SINT64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_FIXED64
            || field->type() == google::protobuf::FieldDescriptor::TYPE_SFIXED64 )
        {
            fieldDescription["jsonEncoding"] = "decimal string";
        }

        fields.emplace_back( std::move( fieldDescription ) );
    }

    return { { "itemType", aItemType },
             { "messagePath", normalizedPath },
             { "messageType", descriptor->full_name() },
             { "fields", std::move( fields ) },
             { "units", "Coordinates and distances are nanometers" } };
}


bool parsePcbItem( const std::string& aItemType, const nlohmann::json& aJson,
                   std::unique_ptr<google::protobuf::Message>& aMessage, std::string& aError )
{
    if( !aJson.is_object() )
    {
        aError = "each PCB item must be a JSON object";
        return false;
    }

    aMessage = newPcbItem( aItemType );

    if( !aMessage )
    {
        aError = "unsupported PCB item type";
        return false;
    }

    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = false;
    google::protobuf::util::Status status =
            google::protobuf::util::JsonStringToMessage( aJson.dump(), aMessage.get(), options );

    if( !status.ok() )
    {
        aError = status.ToString();
        return false;
    }

    return true;
}


std::string pcbItemId( const google::protobuf::Any& aItem, const std::string& aItemType )
{
    std::unique_ptr<google::protobuf::Message> message = newPcbItem( aItemType );

    if( !message || !aItem.UnpackTo( message.get() ) )
        return {};

    const google::protobuf::FieldDescriptor* idField =
            message->GetDescriptor()->FindFieldByName( "id" );

    if( !idField || idField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        return {};

    const google::protobuf::Message& idMessage =
            message->GetReflection()->GetMessage( *message, idField );
    const google::protobuf::FieldDescriptor* valueField =
            idMessage.GetDescriptor()->FindFieldByName( "value" );

    if( !valueField || valueField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING )
        return {};

    return idMessage.GetReflection()->GetString( idMessage, valueField );
}


class KICHAD_IPC_COMMIT_GUARD
{
public:
    KICHAD_IPC_COMMIT_GUARD( const KICHAD_IPC_CLIENT& aClient,
                             const KICHAD_IPC_TARGET& aTarget ) :
            m_client( aClient ),
            m_target( aTarget ),
            m_active( false )
    {}

    ~KICHAD_IPC_COMMIT_GUARD()
    {
        if( m_active )
        {
            std::string ignored;
            m_client.EndCommit( m_target, m_id, false, "", ignored );
        }
    }

    bool Begin( std::string& aError )
    {
        m_active = m_client.BeginCommit( m_target, m_id, aError );

        if( !m_active )
        {
            // EndCommit deliberately accepts an empty ID for CMA_DROP.  Recover a transaction
            // whose previous response was lost before refusing the new mutation.
            std::string recoveryError;

            if( m_client.EndCommit( m_target, "", false, "", recoveryError ) )
                m_active = m_client.BeginCommit( m_target, m_id, aError );
        }

        return m_active;
    }

    bool Commit( const std::string& aMessage, std::string& aError )
    {
        if( !m_active || !m_client.EndCommit( m_target, m_id, true, aMessage, aError ) )
            return false;

        m_active = false;
        return true;
    }

private:
    const KICHAD_IPC_CLIENT& m_client;
    const KICHAD_IPC_TARGET& m_target;
    std::string              m_id;
    bool                     m_active;
};

} // namespace


CODEX_TOOL_REGISTRY::CODEX_TOOL_REGISTRY( std::function<wxString()> aProjectPathProvider,
                                          std::function<bool()> aMutationGuard,
                                          std::function<wxString()> aIpcSocketDirectoryProvider ) :
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_mutationGuard( std::move( aMutationGuard ) ),
        m_ipcSocketDirectoryProvider( std::move( aIpcSocketDirectoryProvider ) )
{}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Specs() const
{
    JSON specs = JSON::array();

    JSON projectSchema = { { "type", "object" },
                           { "additionalProperties", false },
                           { "required", JSON::array( { "operation" } ) } };
    projectSchema["properties"]["operation"] =
            { { "type", "string" }, { "enum", JSON::array( { "context" } ) } };

    specs.push_back( { { "type", "function" },
                       { "name", "project" },
                       { "description",
                         "Read the active KiChad project context. Use operation 'context' to "
                         "discover the stable KiCad version, design files, and whether a turn "
                         "snapshot allows mutation." },
                       { "inputSchema", std::move( projectSchema ) } } );

    JSON inspectSchema = { { "type", "object" },
                           { "additionalProperties", false },
                           { "required", JSON::array( { "operation", "path" } ) } };
    inspectSchema["properties"]["operation"] =
            { { "type", "string" }, { "enum", JSON::array( { "summary", "find" } ) } };
    inspectSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative KiCad design file path." } };
    inspectSchema["properties"]["head"] =
            { { "type", "string" }, { "maxLength", 128 },
              { "description", "List head required by operation 'find'." } };
    inspectSchema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 },
              { "description", "Maximum matches returned; defaults to 20." } };

    specs.push_back( { { "type", "function" },
                       { "name", "inspect" },
                       { "description",
                         "Inspect a KiCad 10 schematic, board, symbol library, or footprint "
                         "without changing it. Use 'summary' for structural counts or 'find' for "
                         "bounded raw expressions with a particular list head." },
                       { "inputSchema", std::move( inspectSchema ) } } );

    JSON designSchema = { { "type", "object" },
                          { "additionalProperties", false },
                          { "required", JSON::array( { "operation" } ) } };
    designSchema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", JSON::array( { "describe", "compile", "preview", "save" } ) } };
    designSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative reusable .kicad_kds sidecar path." } };
    designSchema["properties"]["source"] =
            { { "type", "string" }, { "maxLength", MAX_DESIGN_SCRIPT_BYTES },
              { "description", "Inline KiChad Design Script s-expression source." } };
    designSchema["properties"]["expectedSha256"] =
            { { "type", "string" }, { "minLength", 64 }, { "maxLength", 64 },
              { "description",
                "Required when replacing an existing sidecar to prevent stale writes." } };
    designSchema["properties"]["includeIr"] =
            { { "type", "boolean" },
              { "description", "Include bounded validated compiler IR; defaults to true." } };
    designSchema["properties"]["includeOperations"] =
            { { "type", "boolean" },
              { "description",
                "Include bounded KiCad protobuf-JSON operations in preview; defaults to true." } };

    specs.push_back( { { "type", "function" },
                       { "name", "design" },
                       { "description",
                         "Describe, compile, preview, import, or atomically save a reusable KiChad Design "
                         "Script sidecar. KDS programs declare the complete project—schematic, "
                         "libraries, PCB intent, sourcing, verification, and fabrication outputs—"
                         "for deterministic execution by KiChad compiler backends." },
                       { "inputSchema", std::move( designSchema ) } } );

    JSON pcbSchema = { { "type", "object" },
                       { "additionalProperties", false },
                       { "required", JSON::array( { "operation", "path" } ) } };
    pcbSchema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", JSON::array( { "status", "describe", "get", "mutate" } ) } };
    pcbSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative .kicad_pcb path." } };
    pcbSchema["properties"]["itemType"] =
            { { "type", "string" },
              { "enum", JSON::array(
                                { "footprint", "trace", "via", "arc", "zone", "shape", "text" } ) },
              { "description", "KiCad 10 protobuf board item type." } };
    pcbSchema["properties"]["messagePath"] =
            { { "type", "string" }, { "maxLength", 512 },
              { "description",
                "Dot-separated message field path to expand with operation 'describe'." } };
    pcbSchema["properties"]["action"] =
            { { "type", "string" },
              { "enum", JSON::array( { "create", "update", "delete" } ) } };
    pcbSchema["properties"]["items"] =
            { { "type", "array" }, { "minItems", 1 }, { "maxItems", 200 },
              { "items", { { "type", "object" } } },
              { "description", "Items encoded with official protobuf JSON field names." } };
    pcbSchema["properties"]["ids"] =
            { { "type", "array" }, { "minItems", 1 }, { "maxItems", 500 },
              { "items", { { "type", "string" }, { "maxLength", 36 } } } };
    pcbSchema["properties"]["fieldMask"] =
            { { "type", "array" }, { "maxItems", 32 },
              { "items", { { "type", "string" }, { "maxLength", 128 } } },
              { "description",
                "Protobuf protoName field paths; required for update. UUID fields are immutable." } };
    pcbSchema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } };
    pcbSchema["properties"]["commitMessage"] =
            { { "type", "string" }, { "maxLength", 256 } };

    specs.push_back( { { "type", "function" },
                       { "name", "pcb" },
                       { "description",
                         "Use the supported KiCad 10 protobuf IPC API for the live PCB Editor. "
                         "Use 'status' to verify the editor, 'describe' to discover exact typed "
                         "fields, 'get' to read live items, or 'mutate' to create, field-mask "
                         "update, or delete items atomically. "
                         "Coordinates and distances in protobuf JSON are nanometers." },
                       { "inputSchema", std::move( pcbSchema ) } } );

    return specs;
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Handle( const std::string& aTool,
                                                        const JSON& aArguments ) const
{
    wxString socketDirectory =
            m_ipcSocketDirectoryProvider ? m_ipcSocketDirectoryProvider() : wxString();
    return HandleWithContext( aTool, aArguments, projectPath(),
                              m_mutationGuard && m_mutationGuard(), socketDirectory );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::HandleWithContext(
        const std::string& aTool, const JSON& aArguments, const wxString& aProjectPath,
        bool aMutationAvailable, const wxString& aIpcSocketDirectory ) const
{
    try
    {
        if( aTool == "project" )
            return handleProject( aArguments, aProjectPath, aMutationAvailable );

        if( aTool == "inspect" )
            return handleInspect( aArguments, aProjectPath );

        if( aTool == "design" )
            return handleDesign( aArguments, aProjectPath, aMutationAvailable );

        if( aTool == "pcb" )
            return handlePcb( aArguments, aProjectPath, aMutationAvailable, aIpcSocketDirectory );

        return failure( "unknown_tool", "The requested tool is not advertised by KiChad" );
    }
    catch( const std::exception& error )
    {
        return failure( "tool_failed", error.what() );
    }
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleDesign(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() )
    {
        return failure( "invalid_arguments", "design.operation must be a string" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "describe" && operation != "compile" && operation != "preview"
        && operation != "save" )
    {
        return failure( "invalid_arguments",
                        "design.operation must be 'describe', 'compile', 'preview', or 'save'" );
    }

    if( operation == "describe" )
        return success( KICHAD::DESIGN_SCRIPT_COMPILER::Describe() );

    const bool hasSource = aArguments.contains( "source" ) && aArguments["source"].is_string();
    const bool hasPath = aArguments.contains( "path" ) && aArguments["path"].is_string();

    if( ( ( operation == "compile" || operation == "preview" ) && hasSource == hasPath )
        || ( operation == "save" && ( !hasSource || !hasPath ) ) )
    {
        return failure( "invalid_arguments",
                        operation != "save"
                                ? "design.compile and design.preview require exactly one of source or path"
                                : "design.save requires source and path" );
    }

    if( ( aArguments.contains( "source" ) && !aArguments["source"].is_string() )
        || ( aArguments.contains( "path" ) && !aArguments["path"].is_string() )
        || ( aArguments.contains( "includeIr" ) && !aArguments["includeIr"].is_boolean() )
        || ( aArguments.contains( "includeOperations" )
             && !aArguments["includeOperations"].is_boolean() ) )
    {
        return failure( "invalid_arguments",
                        "design source, path, includeIr, or includeOperations has the wrong type" );
    }

    auto readFile = []( const wxFileName& aFile, std::string& aSource,
                        std::string& aError ) -> bool
    {
        wxFile file( aFile.GetFullPath(), wxFile::read );

        if( !file.IsOpened() )
        {
            aError = "could not open the KiChad Design Script sidecar";
            return false;
        }

        const wxFileOffset length = file.Length();

        if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_SCRIPT_BYTES ) )
        {
            aError = "KiChad Design Script sidecars must contain 1 byte to 1 MiB";
            return false;
        }

        aSource.assign( static_cast<size_t>( length ), '\0' );

        if( file.Read( aSource.data(), static_cast<size_t>( length ) ) != length )
        {
            aError = "could not read the complete KiChad Design Script sidecar";
            aSource.clear();
            return false;
        }

        return true;
    };

    std::string source;
    wxFileName  sidecar;
    std::string pathError;

    if( hasPath )
    {
        const std::string relativePath = aArguments["path"].get<std::string>();

        if( !resolveProjectSidecar( aProjectPath, relativePath, sidecar, pathError ) )
            return failure( "invalid_path", pathError );

        if( ( operation == "compile" || operation == "preview" ) && !sidecar.FileExists() )
            return failure( "read_failed", "KiChad Design Script sidecar does not exist" );
    }

    if( hasSource )
        source = aArguments["source"].get<std::string>();
    else if( !readFile( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    if( source.empty() || source.size() > MAX_DESIGN_SCRIPT_BYTES
        || source.find( '\0' ) != std::string::npos )
    {
        return failure( "invalid_source",
                        "KiChad Design Script source must be UTF-8 text containing 1 byte to 1 MiB" );
    }

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( operation == "compile" )
    {
        JSON payload = { { "operation", "compile" },
                         { "valid", compiled.ok },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "plan", compiled.plan },
                         { "diagnostics", compiled.diagnostics } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        const bool includeIr = aArguments.value( "includeIr", true );

        if( includeIr && compiled.ir.dump().size() <= MAX_DESIGN_RESULT_BYTES )
        {
            payload["ir"] = std::move( compiled.ir );
            payload["irOmitted"] = false;
        }
        else
        {
            payload["irOmitted"] = true;
            payload["irOmissionReason"] = includeIr ? "size_limit" : "not_requested";
        }

        return success( payload );
    }

    if( operation == "preview" )
    {
        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        JSON boardPlan = { { "fullyLowered", planned.fullyLowered },
                           { "counts", std::move( planned.counts ) },
                           { "diagnostics", std::move( planned.diagnostics ) } };
        const bool includeOperations = aArguments.value( "includeOperations", true );

        if( includeOperations && planned.operations.dump().size() <= MAX_DESIGN_RESULT_BYTES )
        {
            boardPlan["operations"] = std::move( planned.operations );
            boardPlan["operationsOmitted"] = false;
        }
        else
        {
            boardPlan["operationsOmitted"] = true;
            boardPlan["operationsOmissionReason"] =
                    includeOperations ? "size_limit" : "not_requested";
        }

        JSON payload = { { "operation", "preview" },
                         { "valid", true },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "compilerPlan", std::move( compiled.plan ) },
                         { "boardPlan", std::move( boardPlan ) } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( !aMutationAvailable )
        return failure( "snapshot_required", "A pre-turn project snapshot is required to save KDS" );

    if( !compiled.ok )
    {
        std::string message = "KiChad Design Script did not pass compilation";

        if( !compiled.diagnostics.empty() )
            message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

        return failure( "compile_failed", message );
    }

    if( sidecar.FileExists() )
    {
        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string() )
        {
            return failure( "stale_source",
                            "expectedSha256 is required when replacing an existing sidecar" );
        }

        std::string existing;

        if( !readFile( sidecar, existing, pathError ) )
            return failure( "read_failed", pathError );

        std::string existingSha256;
        picosha2::hash256_hex_string( existing, existingSha256 );

        if( aArguments["expectedSha256"].get<std::string>() != existingSha256 )
            return failure( "stale_source", "sidecar changed since it was loaded" );
    }

    const wxString temporaryPath =
            sidecar.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( source.data(), source.size() ) != source.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        return failure( "write_failed", "could not durably write the sidecar temporary file" );
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, sidecar.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        return failure( "write_failed", "could not atomically install the sidecar" );
    }

    std::string installed;

    if( !readFile( sidecar, installed, pathError ) || installed != source )
        return failure( "write_failed", "sidecar verification failed after atomic installation" );

    JSON payload = { { "operation", "save" },
                     { "path", aArguments["path"] },
                     { "bytes", source.size() },
                     { "sourceSha256", compiled.sourceSha256 },
                     { "valid", true },
                     { "plan", compiled.plan },
                     { "transaction", "snapshot-backed atomic save" } };
    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handlePcb(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        const wxString& aIpcSocketDirectory ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() || !aArguments.contains( "path" )
        || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "pcb.operation and pcb.path must be strings" );
    }

    if( aArguments.dump().size() > MAX_PCB_ARGUMENT_BYTES )
        return failure( "invalid_arguments", "pcb arguments are limited to 1 MiB" );

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "status" && operation != "describe" && operation != "get"
        && operation != "mutate" )
    {
        return failure( "invalid_arguments",
                        "pcb.operation must be 'status', 'describe', 'get', or 'mutate'" );
    }

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName       resolved;
    std::string      pathError;

    if( !resolveProjectFile( aProjectPath, relativePath, resolved, pathError )
        || resolved.GetExt() != wxS( "kicad_pcb" ) )
    {
        if( pathError.empty() )
            pathError = "pcb.path must identify a project .kicad_pcb file";

        return failure( "invalid_path", pathError );
    }

    if( operation == "mutate" && !aMutationAvailable )
    {
        return failure( "snapshot_required",
                        "A complete pre-turn project snapshot is required before PCB mutation" );
    }

    JSON payload = { { "operation", operation },
                     { "path", relativePath },
                     { "mutationAvailable", aMutationAvailable },
                     { "transport", "KiCad 10 protobuf IPC" } };

    std::string itemType;
    kiapi::common::types::KiCadObjectType objectType =
            kiapi::common::types::KOT_UNKNOWN;

    if( operation != "status" )
    {
        if( !aArguments.contains( "itemType" ) || !aArguments["itemType"].is_string() )
            return failure( "invalid_arguments", "pcb.itemType must be a supported string" );

        itemType = aArguments["itemType"].get<std::string>();
        objectType = pcbObjectType( itemType );

        if( objectType == kiapi::common::types::KOT_UNKNOWN )
            return failure( "invalid_arguments", "pcb.itemType is not supported" );
    }

    if( operation == "describe" )
    {
        std::string messagePath;

        if( aArguments.contains( "messagePath" ) )
        {
            if( !aArguments["messagePath"].is_string()
                || aArguments["messagePath"].get_ref<const std::string&>().size() > 512 )
            {
                return failure( "invalid_arguments",
                                "pcb.messagePath must be a string of at most 512 bytes" );
            }

            messagePath = aArguments["messagePath"].get<std::string>();
        }

        std::string descriptorError;
        JSON descriptor = describePcbMessage( itemType, messagePath, descriptorError );

        if( !descriptorError.empty() )
            return failure( "invalid_arguments", descriptorError );

        payload["schema"] = std::move( descriptor );
        payload["editorRequired"] = false;
        return success( payload );
    }

    KICHAD_IPC_CLIENT client( "org.kichad.codex-" + std::to_string( wxGetProcessId() ),
                              aIpcSocketDirectory );
    KICHAD_IPC_TARGET target;
    std::string       ipcError;
    bool editorOpen = client.FindOpenPcb( aProjectPath, resolved.GetFullPath(), target, ipcError );
    payload["editorOpen"] = editorOpen;

    if( !editorOpen )
    {
        if( operation == "status" )
        {
            payload["detail"] = ipcError;
            return success( payload );
        }

        return failure( "pcb_editor_unavailable", ipcError );
    }

    payload["boardFilename"] = target.document.board_filename();
    payload["projectName"] = target.document.project().name();

    if( operation == "status" )
        return success( payload );

    std::vector<std::string> fieldMask;

    if( aArguments.contains( "fieldMask" ) )
    {
        if( !aArguments["fieldMask"].is_array() || aArguments["fieldMask"].size() > 32 )
            return failure( "invalid_arguments", "pcb.fieldMask must contain at most 32 paths" );

        for( const JSON& field : aArguments["fieldMask"] )
        {
            if( !field.is_string() || field.get_ref<const std::string&>().empty()
                || field.get_ref<const std::string&>().size() > 128 )
            {
                return failure( "invalid_arguments", "pcb.fieldMask contains an invalid path" );
            }

            fieldMask.emplace_back( field.get<std::string>() );
        }

        std::unique_ptr<google::protobuf::Message> prototype = newPcbItem( itemType );

        for( const std::string& field : fieldMask )
        {
            if( !google::protobuf::util::FieldMaskUtil::GetFieldDescriptors(
                        prototype->GetDescriptor(), field, nullptr ) )
            {
                return failure( "invalid_arguments",
                                "pcb.fieldMask contains a path that is invalid for " + itemType );
            }
        }
    }

    if( operation == "get" )
    {
        int limit = 50;

        if( aArguments.contains( "limit" ) )
        {
            if( !aArguments["limit"].is_number_integer() )
                return failure( "invalid_arguments", "pcb.limit must be an integer" );

            limit = aArguments["limit"].get<int>();

            if( limit < 1 || limit > 200 )
                return failure( "invalid_arguments", "pcb.limit must be between 1 and 200" );
        }

        kiapi::common::commands::GetItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );
        request.add_types( objectType );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::GetItemsResponse itemsResponse;

        if( !response.message().UnpackTo( &itemsResponse )
            || itemsResponse.status() != kiapi::common::types::IRS_OK )
        {
            return failure( "ipc_failed", "KiCad returned an invalid get-items response" );
        }

        JSON   items = JSON::array();
        size_t resultBytes = 0;

        for( const google::protobuf::Any& item : itemsResponse.items() )
        {
            if( items.size() >= static_cast<size_t>( limit ) )
                break;

            std::unique_ptr<google::protobuf::Message> message = newPcbItem( itemType );

            if( !message || !item.UnpackTo( message.get() ) )
                return failure( "ipc_failed", "KiCad returned an unexpected PCB item type" );

            if( !fieldMask.empty() )
            {
                google::protobuf::FieldMask mask;

                for( const std::string& field : fieldMask )
                    mask.add_paths( field );

                mask.add_paths( "id" );
                google::protobuf::util::FieldMaskUtil::TrimMessage( mask, message.get() );
            }

            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            std::string serialized;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString( *message, &serialized, options );

            if( !status.ok() )
                return failure( "ipc_failed", status.ToString() );

            if( resultBytes + serialized.size() > MAX_PCB_RESULT_BYTES )
                break;

            resultBytes += serialized.size();
            items.emplace_back( JSON::parse( serialized ) );
        }

        payload["itemType"] = itemType;
        payload["totalItems"] = itemsResponse.items_size();
        payload["items"] = std::move( items );
        payload["resultTruncated"] =
                payload["items"].size() < static_cast<size_t>( itemsResponse.items_size() );
        return success( payload );
    }

    if( !aArguments.contains( "action" ) || !aArguments["action"].is_string() )
        return failure( "invalid_arguments", "pcb.action must be create, update, or delete" );

    const std::string action = aArguments["action"].get<std::string>();

    if( action != "create" && action != "update" && action != "delete" )
        return failure( "invalid_arguments", "pcb.action must be create, update, or delete" );

    std::string commitMessage = "Codex " + action + " PCB " + itemType;

    if( aArguments.contains( "commitMessage" ) )
    {
        if( !aArguments["commitMessage"].is_string()
            || aArguments["commitMessage"].get_ref<const std::string&>().size() > 256 )
        {
            return failure( "invalid_arguments", "pcb.commitMessage must be at most 256 bytes" );
        }

        if( !aArguments["commitMessage"].get_ref<const std::string&>().empty() )
            commitMessage = aArguments["commitMessage"].get<std::string>();
    }

    KICHAD_IPC_COMMIT_GUARD commit( client, target );

    if( action == "delete" )
    {
        if( !aArguments.contains( "ids" ) || !aArguments["ids"].is_array()
            || aArguments["ids"].empty() || aArguments["ids"].size() > 500 )
        {
            return failure( "invalid_arguments", "pcb.ids must contain 1 to 500 UUIDs" );
        }

        kiapi::common::commands::DeleteItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );

        for( const JSON& id : aArguments["ids"] )
        {
            if( !id.is_string() || !KIID::SniffTest( wxString::FromUTF8( id.get<std::string>() ) ) )
                return failure( "invalid_arguments", "pcb.ids contains an invalid KiCad UUID" );

            request.add_item_ids()->set_value( id.get<std::string>() );
        }

        if( !commit.Begin( ipcError ) )
            return failure( "transaction_failed", ipcError );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::DeleteItemsResponse deleted;

        if( !response.message().UnpackTo( &deleted )
            || deleted.status() != kiapi::common::types::IRS_OK
            || deleted.deleted_items_size() != request.item_ids_size() )
        {
            return failure( "ipc_failed", "KiCad rejected one or more PCB deletions" );
        }

        for( const kiapi::common::commands::ItemDeletionResult& item : deleted.deleted_items() )
        {
            if( item.status() != kiapi::common::commands::IDS_OK )
                return failure( "ipc_failed", "KiCad rejected one or more PCB deletions" );
        }

        if( !commit.Commit( commitMessage, ipcError ) )
            return failure( "transaction_failed", ipcError );

        payload["action"] = action;
        payload["itemType"] = itemType;
        payload["affectedItems"] = aArguments["ids"].size();
        payload["transaction"] = "committed";
        return success( payload );
    }

    if( !aArguments.contains( "items" ) || !aArguments["items"].is_array()
        || aArguments["items"].empty() || aArguments["items"].size() > 200 )
    {
        return failure( "invalid_arguments", "pcb.items must contain 1 to 200 objects" );
    }

    if( action == "update" && fieldMask.empty() )
        return failure( "invalid_arguments", "pcb.fieldMask is required for update" );

    if( action == "update" )
    {
        for( const std::string& field : fieldMask )
        {
            if( field == "id" || field.starts_with( "id." ) )
                return failure( "invalid_arguments", "pcb.fieldMask cannot update an item UUID" );
        }
    }

    if( action == "create" )
    {
        kiapi::common::commands::CreateItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );

        for( const JSON& item : aArguments["items"] )
        {
            std::unique_ptr<google::protobuf::Message> message;

            if( !parsePcbItem( itemType, item, message, ipcError ) )
                return failure( "invalid_arguments", ipcError );

            request.add_items()->PackFrom( *message );
        }

        if( !commit.Begin( ipcError ) )
            return failure( "transaction_failed", ipcError );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::CreateItemsResponse created;

        if( !response.message().UnpackTo( &created )
            || created.status() != kiapi::common::types::IRS_OK
            || created.created_items_size() != request.items_size() )
        {
            return failure( "ipc_failed", "KiCad returned an invalid create-items response" );
        }

        JSON ids = JSON::array();

        for( const kiapi::common::commands::ItemCreationResult& item : created.created_items() )
        {
            if( item.status().code() != kiapi::common::commands::ISC_OK )
                return failure( "ipc_failed", item.status().error_message() );

            std::string id = pcbItemId( item.item(), itemType );

            if( !id.empty() )
                ids.emplace_back( std::move( id ) );
        }

        if( !commit.Commit( commitMessage, ipcError ) )
            return failure( "transaction_failed", ipcError );

        payload["action"] = action;
        payload["itemType"] = itemType;
        payload["affectedItems"] = created.created_items_size();
        payload["itemIds"] = std::move( ids );
        payload["transaction"] = "committed";
        return success( payload );
    }

    kiapi::common::commands::UpdateItems request;
    request.mutable_header()->mutable_document()->CopyFrom( target.document );

    for( const std::string& field : fieldMask )
        request.mutable_header()->mutable_field_mask()->add_paths( field );

    for( const JSON& item : aArguments["items"] )
    {
        std::unique_ptr<google::protobuf::Message> message;

        if( !parsePcbItem( itemType, item, message, ipcError ) )
            return failure( "invalid_arguments", ipcError );

        request.add_items()->PackFrom( *message );
    }

    if( !commit.Begin( ipcError ) )
        return failure( "transaction_failed", ipcError );

    kiapi::common::ApiResponse response;

    if( !client.Call( target, request, response, ipcError ) )
        return failure( "ipc_failed", ipcError );

    kiapi::common::commands::UpdateItemsResponse updated;

    if( !response.message().UnpackTo( &updated )
        || updated.status() != kiapi::common::types::IRS_OK
        || updated.updated_items_size() != request.items_size() )
    {
        return failure( "ipc_failed", "KiCad returned an invalid update-items response" );
    }

    for( const kiapi::common::commands::ItemUpdateResult& item : updated.updated_items() )
    {
        if( item.status().code() != kiapi::common::commands::ISC_OK )
            return failure( "ipc_failed", item.status().error_message() );
    }

    if( !commit.Commit( commitMessage, ipcError ) )
        return failure( "transaction_failed", ipcError );

    payload["action"] = action;
    payload["itemType"] = itemType;
    payload["affectedItems"] = updated.updated_items_size();
    payload["transaction"] = "committed";
    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleInspect(
        const JSON& aArguments, const wxString& aProjectPath ) const
{
    if( !aArguments.is_object() )
        return failure( "invalid_arguments", "inspect arguments must be an object" );

    if( !aArguments.contains( "operation" ) || !aArguments["operation"].is_string()
        || !aArguments.contains( "path" ) || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "inspect.operation and inspect.path must be strings" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();
    const std::string relativePath = aArguments["path"].get<std::string>();

    if( operation != "summary" && operation != "find" )
        return failure( "invalid_arguments", "inspect.operation must be 'summary' or 'find'" );

    wxString root = aProjectPath;

    if( !wxFileName::DirExists( root ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    wxFileName resolved;
    std::string pathError;

    if( !resolveProjectFile( root, relativePath, resolved, pathError ) )
        return failure( "invalid_path", pathError );

    wxFile file( resolved.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
        return failure( "read_failed", "Could not open the requested project file" );

    const wxFileOffset length = file.Length();

    if( length == wxInvalidOffset || length > MAX_INSPECTION_BYTES )
        return failure( "file_too_large", "Inspection is limited to 16 MiB per file" );

    std::string source( static_cast<size_t>( length ), '\0' );

    if( length > 0 && file.Read( source.data(), static_cast<size_t>( length ) ) != length )
        return failure( "read_failed", "Could not read the complete project file" );

    std::string parseError;
    std::unique_ptr<KICHAD::LOSSLESS_SEXPR_DOCUMENT> document =
            KICHAD::LOSSLESS_SEXPR_DOCUMENT::Parse( std::move( source ), &parseError );

    if( !document )
        return failure( "parse_failed", parseError );

    if( document->Roots().empty() )
        return failure( "parse_failed", "The requested file has no root expression" );

    const std::string rootHead = document->ListHead( document->Roots().front() );

    if( rootHead != expectedRootHead( resolved.GetExt() ) )
        return failure( "format_mismatch", "The file root does not match its KiCad extension" );

    JSON payload = { { "operation", operation },
                     { "path", relativePath },
                     { "bytes", static_cast<uint64_t>( length ) },
                     { "rootHead", rootHead } };

    if( operation == "summary" )
    {
        std::map<std::string, size_t> counts;

        for( size_t i = 0; i < document->Nodes().size(); ++i )
        {
            std::string head = document->ListHead( i );

            if( !head.empty() )
                ++counts[head];
        }

        std::vector<std::pair<std::string, size_t>> ordered( counts.begin(), counts.end() );
        std::sort( ordered.begin(), ordered.end(),
                   []( const auto& aLeft, const auto& aRight )
                   {
                       if( aLeft.second != aRight.second )
                           return aLeft.second > aRight.second;

                       return aLeft.first < aRight.first;
                   } );

        JSON listCounts = JSON::array();

        for( size_t i = 0; i < ordered.size() && i < MAX_DISTINCT_HEADS; ++i )
        {
            const auto& [head, count] = ordered[i];
            listCounts.push_back( { { "head", head }, { "count", count } } );
        }

        payload["roots"] = document->Roots().size();
        payload["nodes"] = document->Nodes().size();
        payload["distinctHeads"] = ordered.size();
        payload["listCounts"] = std::move( listCounts );
        payload["resultTruncated"] = ordered.size() > MAX_DISTINCT_HEADS;
        return success( payload );
    }

    if( !aArguments.contains( "head" ) || !aArguments["head"].is_string() )
        return failure( "invalid_arguments", "inspect.head must be a string for operation 'find'" );

    const std::string head = aArguments["head"].get<std::string>();

    if( head.empty() || head.size() > 128 )
        return failure( "invalid_arguments", "inspect.head must contain 1 to 128 bytes" );

    int limit = 20;

    if( aArguments.contains( "limit" ) )
    {
        if( !aArguments["limit"].is_number_integer() )
            return failure( "invalid_arguments", "inspect.limit must be an integer" );

        limit = aArguments["limit"].get<int>();

        if( limit < 1 || limit > 50 )
            return failure( "invalid_arguments", "inspect.limit must be between 1 and 50" );
    }

    const std::vector<size_t> matches = document->FindLists( head );
    JSON                      expressions = JSON::array();
    size_t                    resultBytes = 0;

    for( size_t i = 0; i < matches.size() && expressions.size() < static_cast<size_t>( limit ); ++i )
    {
        std::string raw = document->RawText( matches[i] );
        bool        truncated = raw.size() > MAX_EXPRESSION_BYTES;

        if( truncated )
        {
            size_t boundary = MAX_EXPRESSION_BYTES;

            while( boundary > 0 && boundary < raw.size()
                   && ( static_cast<unsigned char>( raw[boundary] ) & 0xC0 ) == 0x80 )
            {
                --boundary;
            }

            raw.resize( boundary );
        }

        if( resultBytes + raw.size() > MAX_RESULT_BYTES )
            break;

        resultBytes += raw.size();
        expressions.push_back( { { "index", i }, { "text", std::move( raw ) },
                                 { "truncated", truncated } } );
    }

    payload["head"] = head;
    payload["totalMatches"] = matches.size();
    payload["expressions"] = std::move( expressions );
    payload["resultTruncated"] = payload["expressions"].size() < matches.size();
    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleProject(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string()
        || aArguments["operation"].get<std::string>() != "context" )
    {
        return failure( "invalid_arguments", "project.operation must be 'context'" );
    }

    wxString path = aProjectPath;

    if( !wxFileName::DirExists( path ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    JSON files = JSON::array();
    wxDir directory( path );
    wxString name;
    bool found = directory.GetFirst( &name, wxS( "*.kicad_*" ), wxDIR_FILES );

    while( found )
    {
        wxFileName file( path, name );
        wxULongLong size = file.GetSize();
        JSON item = { { "name", std::string( name.ToUTF8() ) } };

        if( size != wxInvalidSize )
            item["bytes"] = size.GetValue();

        files.emplace_back( std::move( item ) );
        found = directory.GetNext( &name );
    }

    JSON payload = {
        { "operation", "context" },
        { "projectPath", std::string( path.ToUTF8() ) },
        { "kicadVersion", std::string( GetBuildVersion().ToUTF8() ) },
        { "files", std::move( files ) },
        { "mutationAvailable", aMutationAvailable }
    };

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::success( const JSON& aPayload ) const
{
    JSON envelope = { { "ok", true }, { "data", aPayload } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", true } };
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::failure( const std::string& aCode,
                                                        const std::string& aMessage ) const
{
    JSON envelope = { { "ok", false },
                      { "error", { { "code", aCode }, { "message", aMessage } } } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", false } };
}


wxString CODEX_TOOL_REGISTRY::projectPath() const
{
    wxString path = m_projectPathProvider ? m_projectPathProvider() : wxString();

    return path;
}
