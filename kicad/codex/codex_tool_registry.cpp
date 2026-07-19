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
#include "design_script_pcb_reconciler.h"
#include "kicad_ipc_client.h"
#include "lossless_sexpr_document.h"

#include <build_version.h>
#include <kiid.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <api/board/board_commands.pb.h>
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
constexpr size_t       MAX_DESIGN_STATE_BYTES = 4 * 1024 * 1024;
constexpr size_t       MAX_PCB_FOOTPRINTS = 10000;
constexpr size_t       MAX_PCB_ZONES = 10000;
constexpr auto         MAX_ZONE_REFILL_WAIT = std::chrono::seconds( 30 );


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


bool readJsonFile( const wxFileName& aPath, nlohmann::json& aDocument, std::string& aError )
{
    if( !aPath.FileExists() )
    {
        aDocument = nullptr;
        return true;
    }

    wxFile file( aPath.GetFullPath(), wxFile::read );

    if( !file.IsOpened() )
    {
        aError = "could not open KiChad managed-state data";
        return false;
    }

    const wxFileOffset length = file.Length();

    if( length <= 0 || length > static_cast<wxFileOffset>( MAX_DESIGN_STATE_BYTES ) )
    {
        aError = "KiChad managed-state data must contain 1 byte to 4 MiB";
        return false;
    }

    std::string text( static_cast<size_t>( length ), '\0' );

    if( file.Read( text.data(), text.size() ) != length )
    {
        aError = "could not read complete KiChad managed-state data";
        return false;
    }

    aDocument = nlohmann::json::parse( text, nullptr, false );

    if( aDocument.is_discarded() )
    {
        aError = "KiChad managed-state data is not valid JSON";
        return false;
    }

    return true;
}


bool writeJsonAtomically( const wxFileName& aPath, const nlohmann::json& aDocument,
                          std::string& aError )
{
    const std::string serialized = aDocument.dump( 2 ) + "\n";

    if( serialized.size() > MAX_DESIGN_STATE_BYTES )
    {
        aError = "KiChad managed-state data exceeds 4 MiB";
        return false;
    }

    const wxString temporaryPath =
            aPath.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( serialized.data(), serialized.size() ) != serialized.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        aError = "could not durably write KiChad managed-state data";
        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, aPath.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        aError = "could not atomically install KiChad managed-state data";
        return false;
    }

    nlohmann::json installed;

    if( !readJsonFile( aPath, installed, aError ) || installed != aDocument )
    {
        aError = "KiChad managed-state verification failed after installation";
        return false;
    }

    return true;
}


bool mergeRecoveryJournal( const nlohmann::json& aJournal,
                           const KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT& aContext,
                           nlohmann::json& aPreviousState, std::string& aError )
{
    if( aJournal.is_null() )
        return true;

    if( !aJournal.is_object() || aJournal.value( "format", "" ) != "kichad-kds-apply-journal"
        || aJournal.value( "version", 0 ) != 1
        || aJournal.value( "sourcePath", "" ) != aContext.sourcePath
        || aJournal.value( "boardPath", "" ) != aContext.boardPath
        || aJournal.value( "projectName", "" ) != aContext.projectName
        || !aJournal.contains( "previousState" ) || !aJournal.contains( "preparedState" )
        || !aJournal["preparedState"].is_object()
        || !aJournal["preparedState"].contains( "managedPcbItems" )
        || !aJournal["preparedState"]["managedPcbItems"].is_array() )
    {
        aError = "KiChad apply journal is malformed or belongs to a different design";
        return false;
    }

    nlohmann::json merged = aJournal["preparedState"];
    std::map<std::string, nlohmann::json> items;

    for( const nlohmann::json& item : merged["managedPcbItems"] )
    {
        if( !item.is_object() || !item.contains( "itemId" ) || !item["itemId"].is_string()
            || !items.emplace( item["itemId"].get<std::string>(), item ).second )
        {
            aError = "KiChad apply journal contains invalid prepared ownership";
            return false;
        }
    }

    const nlohmann::json& previous = aJournal["previousState"];

    if( !previous.is_null() )
    {
        if( !previous.is_object() || !previous.contains( "managedPcbItems" )
            || !previous["managedPcbItems"].is_array() )
        {
            aError = "KiChad apply journal contains invalid previous ownership";
            return false;
        }

        for( const nlohmann::json& item : previous["managedPcbItems"] )
        {
            if( !item.is_object() || !item.contains( "itemId" ) || !item["itemId"].is_string() )
            {
                aError = "KiChad apply journal contains invalid previous ownership";
                return false;
            }

            const std::string itemId = item["itemId"].get<std::string>();
            auto              existing = items.find( itemId );

            if( existing != items.end() && existing->second != item )
            {
                aError = "KiChad apply journal contains conflicting ownership";
                return false;
            }

            items.emplace( itemId, item );
        }
    }

    merged["sourcePath"] = aContext.sourcePath;
    merged["boardPath"] = aContext.boardPath;
    merged["projectName"] = aContext.projectName;
    merged["sourceSha256"] = aContext.sourceSha256;
    merged["managedPcbItems"] = nlohmann::json::array();

    for( const auto& [itemId, item] : items )
        merged["managedPcbItems"].push_back( item );

    aPreviousState = std::move( merged );
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
    if( aItemType == "zone" || aItemType == "rule_area" )
        return KOT_PCB_ZONE;
    if( aItemType == "shape" )
        return KOT_PCB_SHAPE;
    if( aItemType == "text" )
        return KOT_PCB_TEXT;
    if( aItemType == "dimension" )
        return KOT_PCB_DIMENSION;

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
    if( aItemType == "zone" || aItemType == "rule_area" )
        return std::make_unique<Zone>();
    if( aItemType == "shape" )
        return std::make_unique<BoardGraphicShape>();
    if( aItemType == "text" )
        return std::make_unique<BoardText>();
    if( aItemType == "dimension" )
        return std::make_unique<Dimension>();

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

    bool Drop( std::string& aError )
    {
        if( !m_active )
            return true;

        if( !m_client.EndCommit( m_target, m_id, false, "", aError ) )
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


std::string pcbAnyType( const google::protobuf::Any& aItem )
{
    using namespace kiapi::board::types;

    if( aItem.Is<BoardGraphicShape>() )
        return "shape";
    if( aItem.Is<Track>() )
        return "trace";
    if( aItem.Is<Arc>() )
        return "arc";
    if( aItem.Is<Via>() )
        return "via";
    if( aItem.Is<BoardText>() )
        return "text";
    if( aItem.Is<Dimension>() )
        return "dimension";
    if( aItem.Is<Zone>() )
    {
        Zone zone;

        if( aItem.UnpackTo( &zone ) && zone.type() == ZT_RULE_AREA )
            return "rule_area";

        return "zone";
    }

    return "other:" + aItem.type_url();
}


std::string pcbAnyId( const google::protobuf::Any& aItem )
{
    std::string itemType = pcbAnyType( aItem );

    if( itemType.starts_with( "other:" ) )
    {
        size_t separator = aItem.type_url().rfind( '/' );
        std::string typeName = separator == std::string::npos
                                       ? aItem.type_url()
                                       : aItem.type_url().substr( separator + 1 );
        const google::protobuf::Descriptor* descriptor =
                google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName( typeName );

        if( !descriptor )
            return {};

        const google::protobuf::Message* prototype =
                google::protobuf::MessageFactory::generated_factory()->GetPrototype( descriptor );

        if( !prototype )
            return {};

        std::unique_ptr<google::protobuf::Message> message( prototype->New() );

        if( !aItem.UnpackTo( message.get() ) )
            return {};

        const google::protobuf::FieldDescriptor* idField = descriptor->FindFieldByName( "id" );

        if( !idField || idField->cpp_type()
                               != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE )
        {
            return {};
        }

        const google::protobuf::Message& idMessage =
                message->GetReflection()->GetMessage( *message, idField );
        const google::protobuf::FieldDescriptor* valueField =
                idMessage.GetDescriptor()->FindFieldByName( "value" );

        if( !valueField || valueField->cpp_type()
                                  != google::protobuf::FieldDescriptor::CPPTYPE_STRING )
        {
            return {};
        }

        return idMessage.GetReflection()->GetString( idMessage, valueField );
    }

    return pcbItemId( aItem, itemType );
}


bool queryPcbInventory( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const std::set<std::string>& aIds, nlohmann::json& aInventory,
                        std::string& aError )
{
    aInventory = nlohmann::json::array();

    if( aIds.empty() )
        return true;

    kiapi::common::commands::GetItemsById request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

    for( const std::string& id : aIds )
        request.add_items()->set_value( id );

    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::GetItemsResponse items;

    if( !response.message().UnpackTo( &items )
        || items.status() != kiapi::common::types::IRS_OK )
    {
        aError = "KiCad returned an invalid managed-item inventory";
        return false;
    }

    std::set<std::string> returnedIds;

    for( const google::protobuf::Any& item : items.items() )
    {
        std::string itemId = pcbAnyId( item );

        if( itemId.empty() || !aIds.contains( itemId ) || !returnedIds.emplace( itemId ).second )
        {
            aError = "KiCad returned an unexpected managed-item identity";
            return false;
        }

        aInventory.push_back( { { "itemId", itemId }, { "itemType", pcbAnyType( item ) } } );
    }

    return true;
}


bool queryPcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                      kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    kiapi::board::commands::GetBoardStackup request;
    request.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardStackupResponse stackupResponse;

    if( !response.message().UnpackTo( &stackupResponse )
        || !stackupResponse.has_stackup() )
    {
        aError = "KiCad returned an invalid board stackup";
        return false;
    }

    aStackup.CopyFrom( stackupResponse.stackup() );
    return true;
}


bool updatePcbStackup( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                       const kiapi::board::BoardStackup& aStackup, std::string& aError )
{
    kiapi::board::commands::UpdateBoardStackup request;
    request.mutable_board()->CopyFrom( aTarget.document );
    request.mutable_stackup()->CopyFrom( aStackup );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::board::commands::BoardStackupResponse stackupResponse;

    if( !response.message().UnpackTo( &stackupResponse )
        || !stackupResponse.has_stackup() )
    {
        aError = "KiCad returned an invalid updated board stackup";
        return false;
    }

    return true;
}


bool queryPcbFootprintInventory( const KICHAD_IPC_CLIENT& aClient,
                                 const KICHAD_IPC_TARGET& aTarget,
                                 const std::set<std::string>& aReferences,
                                 nlohmann::json& aInventory, std::string& aError )
{
    if( aReferences.empty() )
        return true;

    kiapi::common::commands::GetItems request;
    request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
    request.add_types( kiapi::common::types::KOT_PCB_FOOTPRINT );

    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError ) )
        return false;

    kiapi::common::commands::GetItemsResponse items;

    if( !response.message().UnpackTo( &items )
        || items.status() != kiapi::common::types::IRS_OK
        || items.items_size() > static_cast<int>( MAX_PCB_FOOTPRINTS ) )
    {
        aError = "KiCad returned an invalid or excessive footprint inventory";
        return false;
    }

    std::set<std::string> returnedIds;

    for( const google::protobuf::Any& packed : items.items() )
    {
        kiapi::board::types::FootprintInstance footprint;

        if( !packed.UnpackTo( &footprint ) )
        {
            aError = "KiCad returned a non-footprint in the footprint inventory";
            return false;
        }

        const std::string reference = footprint.reference_field().text().text().text();

        if( !aReferences.contains( reference ) )
            continue;

        const std::string itemId = footprint.id().value();

        if( !KIID::SniffTest( wxString::FromUTF8( itemId ) )
            || !returnedIds.emplace( itemId ).second )
        {
            aError = "KiCad returned an invalid or duplicate footprint identity";
            return false;
        }

        aInventory.push_back(
                { { "itemId", itemId },
                  { "itemType", "footprint" },
                  { "reference", reference },
                  { "schematicLinked", !footprint.attributes().not_in_schematic()
                                                && footprint.symbol_path().path_size() > 0 } } );
    }

    return true;
}


bool refillPcbZones( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                     const std::set<std::string>& aExpectedZoneIds, std::string& aError )
{
    kiapi::board::commands::RefillZones refill;
    refill.mutable_board()->CopyFrom( aTarget.document );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, refill, response, aError ) )
        return false;

    // RefillZones is accepted synchronously but KiCad performs the fill on the editor thread and
    // reports AS_BUSY in the interim. Poll the authoritative zone objects until every desired KDS
    // zone is present and filled. This also closes the race before the queued fill action starts.
    std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
    const auto deadline = std::chrono::steady_clock::now() + MAX_ZONE_REFILL_WAIT;
    std::string lastError;

    do
    {
        kiapi::common::commands::GetItems query;
        query.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
        query.add_types( kiapi::common::types::KOT_PCB_ZONE );
        kiapi::common::ApiResponse queryResponse;

        if( aClient.Call( aTarget, query, queryResponse, lastError ) )
        {
            kiapi::common::commands::GetItemsResponse items;

            if( !queryResponse.message().UnpackTo( &items )
                || items.status() != kiapi::common::types::IRS_OK
                || items.items_size() > static_cast<int>( MAX_PCB_ZONES ) )
            {
                aError = "KiCad returned an invalid or excessive zone inventory after refill";
                return false;
            }

            std::set<std::string> filled;

            for( const google::protobuf::Any& packed : items.items() )
            {
                kiapi::board::types::Zone zone;

                if( !packed.UnpackTo( &zone ) )
                {
                    aError = "KiCad returned a non-zone in the zone refill inventory";
                    return false;
                }

                if( aExpectedZoneIds.contains( zone.id().value() ) && zone.filled() )
                    filled.emplace( zone.id().value() );
            }

            if( filled.size() == aExpectedZoneIds.size() )
                return true;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
    } while( std::chrono::steady_clock::now() < deadline );

    aError = "KiCad did not finish filling every managed KDS zone within 30 seconds";

    if( !lastError.empty() )
        aError += ": " + lastError;

    return false;
}


bool validateCreateUpdateResponse( const google::protobuf::RepeatedPtrField<
                                           kiapi::common::commands::ItemCreationResult>& aResults,
                                   const std::vector<const nlohmann::json*>& aActions,
                                   std::string& aError )
{
    if( aResults.size() != static_cast<int>( aActions.size() ) )
    {
        aError = "KiCad returned an incomplete create-items response";
        return false;
    }

    for( int i = 0; i < aResults.size(); ++i )
    {
        if( aResults[i].status().code() != kiapi::common::commands::ISC_OK
            || pcbAnyId( aResults[i].item() ) != aActions[i]->at( "itemId" ).get<std::string>() )
        {
            aError = aResults[i].status().error_message();

            if( aError.empty() )
                aError = "KiCad rejected or changed a deterministic PCB item UUID";

            return false;
        }
    }

    return true;
}


bool validateCreateUpdateResponse( const google::protobuf::RepeatedPtrField<
                                           kiapi::common::commands::ItemUpdateResult>& aResults,
                                   const std::vector<const nlohmann::json*>& aActions,
                                   std::string& aError )
{
    if( aResults.size() != static_cast<int>( aActions.size() ) )
    {
        aError = "KiCad returned an incomplete update-items response";
        return false;
    }

    for( int i = 0; i < aResults.size(); ++i )
    {
        if( aResults[i].status().code() != kiapi::common::commands::ISC_OK
            || pcbAnyId( aResults[i].item() ) != aActions[i]->at( "itemId" ).get<std::string>() )
        {
            aError = aResults[i].status().error_message();

            if( aError.empty() )
                aError = "KiCad rejected or changed a deterministic PCB item UUID";

            return false;
        }
    }

    return true;
}


bool executePcbActions( const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
                        const nlohmann::json& aActions, std::string& aError )
{
    std::vector<const nlohmann::json*> creates;
    std::map<std::pair<std::string, std::string>,
             std::vector<const nlohmann::json*>> updates;
    std::vector<const nlohmann::json*> deletes;

    for( const nlohmann::json& action : aActions )
    {
        const std::string kind = action.at( "action" ).get<std::string>();

        if( kind == "create" )
            creates.emplace_back( &action );
        else if( kind == "update" )
            updates[{ action.at( "itemType" ).get<std::string>(),
                      action.at( "fieldMask" ).dump() }].emplace_back( &action );
        else
            deletes.emplace_back( &action );
    }

    for( size_t begin = 0; begin < creates.size(); begin += 200 )
    {
        const size_t end = std::min( begin + 200, creates.size() );
        kiapi::common::commands::CreateItems request;
        request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );
        std::vector<const nlohmann::json*> batch( creates.begin() + begin, creates.begin() + end );

        for( const nlohmann::json* action : batch )
        {
            std::unique_ptr<google::protobuf::Message> item;

            if( !parsePcbItem( action->at( "itemType" ).get<std::string>(),
                               action->at( "item" ), item, aError ) )
            {
                return false;
            }

            request.add_items()->PackFrom( *item );
        }

        kiapi::common::ApiResponse response;

        if( !aClient.Call( aTarget, request, response, aError ) )
            return false;

        kiapi::common::commands::CreateItemsResponse created;

        if( !response.message().UnpackTo( &created )
            || created.status() != kiapi::common::types::IRS_OK
            || !validateCreateUpdateResponse( created.created_items(), batch, aError ) )
        {
            if( aError.empty() )
                aError = "KiCad returned an invalid create-items response";

            return false;
        }
    }

    for( const auto& [key, actions] : updates )
    {
        const std::string& itemType = key.first;

        for( size_t begin = 0; begin < actions.size(); begin += 200 )
        {
            const size_t end = std::min( begin + 200, actions.size() );
            std::vector<const nlohmann::json*> batch( actions.begin() + begin,
                                                       actions.begin() + end );
            kiapi::common::commands::UpdateItems request;
            request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

            for( const nlohmann::json& field : batch.front()->at( "fieldMask" ) )
                request.mutable_header()->mutable_field_mask()->add_paths( field.get<std::string>() );

            for( const nlohmann::json* action : batch )
            {
                std::unique_ptr<google::protobuf::Message> item;

                if( !parsePcbItem( itemType, action->at( "item" ), item, aError ) )
                    return false;

                request.add_items()->PackFrom( *item );
            }

            kiapi::common::ApiResponse response;

            if( !aClient.Call( aTarget, request, response, aError ) )
                return false;

            kiapi::common::commands::UpdateItemsResponse updated;

            if( !response.message().UnpackTo( &updated )
                || updated.status() != kiapi::common::types::IRS_OK
                || !validateCreateUpdateResponse( updated.updated_items(), batch, aError ) )
            {
                if( aError.empty() )
                    aError = "KiCad returned an invalid update-items response";

                return false;
            }
        }
    }

    for( size_t begin = 0; begin < deletes.size(); begin += 500 )
    {
        const size_t end = std::min( begin + 500, deletes.size() );
        kiapi::common::commands::DeleteItems request;
        request.mutable_header()->mutable_document()->CopyFrom( aTarget.document );

        for( size_t i = begin; i < end; ++i )
            request.add_item_ids()->set_value( deletes[i]->at( "itemId" ).get<std::string>() );

        kiapi::common::ApiResponse response;

        if( !aClient.Call( aTarget, request, response, aError ) )
            return false;

        kiapi::common::commands::DeleteItemsResponse deleted;

        if( !response.message().UnpackTo( &deleted )
            || deleted.status() != kiapi::common::types::IRS_OK
            || deleted.deleted_items_size() != static_cast<int>( end - begin ) )
        {
            aError = "KiCad returned an invalid delete-items response";
            return false;
        }

        for( int i = 0; i < deleted.deleted_items_size(); ++i )
        {
            if( deleted.deleted_items( i ).status() != kiapi::common::commands::IDS_OK
                || deleted.deleted_items( i ).id().value()
                           != deletes[begin + static_cast<size_t>( i )]
                                      ->at( "itemId" ).get<std::string>() )
            {
                aError = "KiCad rejected or changed a managed PCB deletion";
                return false;
            }
        }
    }

    return true;
}

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
              { "enum",
                JSON::array( { "describe", "read", "compile", "preview", "save", "apply" } ) } };
    designSchema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative reusable .kicad_kds sidecar path." } };
    designSchema["properties"]["source"] =
            { { "type", "string" }, { "maxLength", MAX_DESIGN_SCRIPT_BYTES },
              { "description", "Inline KiChad Design Script s-expression source." } };
    designSchema["properties"]["boardPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative .kicad_pcb target required by apply." } };
    designSchema["properties"]["expectedSha256"] =
            { { "type", "string" }, { "minLength", 64 }, { "maxLength", 64 },
              { "description",
                "Required for stale-write protection and to apply the exact compiled revision." } };
    specs.push_back( { { "type", "function" },
                       { "name", "design" },
                       { "description",
                         "Describe, read, compile, preview, atomically save, or transactionally "
                         "apply a reusable KiChad Design "
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
                                { "footprint", "trace", "via", "arc", "zone", "rule_area",
                                  "shape", "text", "dimension" } ) },
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
            return handleDesign( aArguments, aProjectPath, aMutationAvailable,
                                 aIpcSocketDirectory );

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
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        const wxString& aIpcSocketDirectory ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() )
    {
        return failure( "invalid_arguments", "design.operation must be a string" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "describe" && operation != "read" && operation != "compile"
        && operation != "preview" && operation != "save" && operation != "apply" )
    {
        return failure( "invalid_arguments",
                        "design.operation must be 'describe', 'read', 'compile', 'preview', "
                        "'save', or 'apply'" );
    }

    if( operation == "describe" )
        return success( KICHAD::DESIGN_SCRIPT_COMPILER::Describe() );

    const bool hasSource = aArguments.contains( "source" ) && aArguments["source"].is_string();
    const bool hasPath = aArguments.contains( "path" ) && aArguments["path"].is_string();

    if( ( ( operation == "compile" || operation == "preview" ) && hasSource == hasPath )
        || ( operation == "save" && ( !hasSource || !hasPath ) )
        || ( ( operation == "read" || operation == "apply" ) && ( hasSource || !hasPath ) ) )
    {
        std::string message;

        if( operation == "read" || operation == "apply" )
            message = "design." + operation + " requires path and does not accept inline source";
        else if( operation == "save" )
            message = "design.save requires source and path";
        else
            message = "design.compile and design.preview require exactly one of source or path";

        return failure( "invalid_arguments", message );
    }

    if( ( aArguments.contains( "source" ) && !aArguments["source"].is_string() )
        || ( aArguments.contains( "path" ) && !aArguments["path"].is_string() )
        || ( aArguments.contains( "boardPath" ) && !aArguments["boardPath"].is_string() ) )
    {
        return failure( "invalid_arguments",
                        "design source, path, or boardPath has the wrong type" );
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

        if( ( operation == "read" || operation == "compile" || operation == "preview"
              || operation == "apply" )
            && !sidecar.FileExists() )
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

    if( operation == "read" )
    {
        const bool invalidUtf8 = std::any_of(
                compiled.diagnostics.begin(), compiled.diagnostics.end(),
                []( const JSON& aDiagnostic )
                {
                    return aDiagnostic.value( "code", "" ) == "invalid_encoding";
                } );

        if( invalidUtf8 )
            return failure( "invalid_source", "KiChad Design Script source must be valid UTF-8" );

        return success( { { "operation", "read" },
                          { "path", aArguments["path"] },
                          { "source", source },
                          { "bytes", source.size() },
                          { "sourceSha256", compiled.sourceSha256 },
                          { "valid", compiled.ok },
                          { "diagnostics", compiled.diagnostics } } );
    }

    if( operation == "compile" )
    {
        JSON payload = { { "operation", "compile" },
                         { "valid", compiled.ok },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "plan", compiled.plan },
                         { "diagnostics", compiled.diagnostics } };

        if( hasPath )
            payload["path"] = aArguments["path"];

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
        JSON items = JSON::array();

        for( const JSON& plannedOperation : planned.operations )
        {
            if( items.size() == 500 )
                break;

            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                items.push_back( { { "action", "manage" },
                                   { "logicalId", plannedOperation["logicalId"] },
                                   { "itemType", plannedOperation["itemType"] },
                                   { "targetId", plannedOperation["itemId"] } } );
            }
            else if( action == "place_by_reference" )
            {
                items.push_back( { { "action", "place" },
                                   { "component", plannedOperation["component"] } } );
            }
            else if( action == "update_stackup" )
            {
                items.push_back( { { "action", "configure_stackup" },
                                   { "physicalLayers",
                                     plannedOperation["stackup"]["layers"].size() } } );
            }
            else
            {
                items.push_back( { { "action", "unsupported" },
                                   { "statementKind", plannedOperation["statementKind"] },
                                   { "reason", plannedOperation["reason"] } } );
            }
        }

        JSON boardPlan = { { "fullyLowered", planned.fullyLowered },
                           { "counts", std::move( planned.counts ) },
                           { "diagnostics", std::move( planned.diagnostics ) },
                           { "items", std::move( items ) },
                           { "itemsTruncated", planned.operations.size() > 500 } };

        JSON payload = { { "operation", "preview" },
                         { "valid", true },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "compilerPlan", std::move( compiled.plan ) },
                         { "boardPlan", std::move( boardPlan ) } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( operation == "apply" )
    {
        if( !aMutationAvailable )
        {
            return failure( "snapshot_required",
                            "A complete pre-turn project snapshot is required to apply KDS" );
        }

        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message );
        }

        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string()
            || aArguments["expectedSha256"].get<std::string>() != compiled.sourceSha256 )
        {
            return failure( "stale_source",
                            "design.apply requires the exact compiled source SHA-256" );
        }

        if( !aArguments.contains( "boardPath" ) || !aArguments["boardPath"].is_string() )
            return failure( "invalid_arguments", "design.apply requires boardPath" );

        const std::string boardRelativePath = aArguments["boardPath"].get<std::string>();
        wxFileName       board;

        if( !resolveProjectFile( aProjectPath, boardRelativePath, board, pathError )
            || board.GetExt() != wxS( "kicad_pcb" ) )
        {
            if( pathError.empty() )
                pathError = "boardPath must identify a project .kicad_pcb file";

            return failure( "invalid_path", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );

        if( !planned.fullyLowered )
        {
            std::string message = "one or more board statements do not have an apply backend";

            for( const JSON& action : planned.operations )
            {
                if( action.value( "action", "" ) == "unsupported" )
                {
                    message += ": " + action.value( "reason", "unsupported statement" );
                    break;
                }
            }

            return failure( "backend_incomplete", message );
        }

        std::unique_ptr<kiapi::board::BoardStackup> desiredStackup;

        for( const JSON& action : planned.operations )
        {
            if( action.value( "action", "" ) != "update_stackup" )
                continue;

            if( desiredStackup )
                return failure( "invalid_plan", "KDS planned more than one board stackup" );

            desiredStackup = std::make_unique<kiapi::board::BoardStackup>();
            google::protobuf::util::JsonParseOptions options;
            options.ignore_unknown_fields = false;
            google::protobuf::util::Status status =
                    google::protobuf::util::JsonStringToMessage(
                            action.at( "stackup" ).dump(), desiredStackup.get(), options );

            if( !status.ok() )
                return failure( "invalid_plan", "KDS produced an invalid native stackup" );
        }

        const std::string sourceRelativePath = aArguments["path"].get<std::string>();
        const std::string projectName = compiled.ir["project"]["name"].get<std::string>();
        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT reconcileContext = {
            sourceRelativePath, boardRelativePath, projectName, compiled.sourceSha256
        };
        wxFileName statePath = sidecar;
        statePath.SetExt( wxS( "kicad_kds_state" ) );
        wxFileName journalPath = sidecar;
        journalPath.SetExt( wxS( "kicad_kds_journal" ) );
        JSON previousState;
        JSON journal;

        if( !readJsonFile( statePath, previousState, pathError )
            || !readJsonFile( journalPath, journal, pathError )
            || !mergeRecoveryJournal( journal, reconcileContext, previousState, pathError ) )
        {
            return failure( "invalid_managed_state", pathError );
        }

        JSON managedOperations = JSON::array();
        JSON reconcileOperations = JSON::array();
        std::set<std::string> placementReferences;

        for( const JSON& plannedOperation : planned.operations )
        {
            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                managedOperations.push_back( plannedOperation );
                reconcileOperations.push_back( plannedOperation );
            }
            else if( action == "place_by_reference" )
            {
                placementReferences.emplace( plannedOperation["component"].get<std::string>() );
                reconcileOperations.push_back( plannedOperation );
            }
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT preflight =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        managedOperations, previousState, JSON::array(), reconcileContext );

        if( !preflight.ok )
        {
            return failure( "reconcile_failed",
                            preflight.diagnostics.empty()
                                    ? "managed PCB state failed validation"
                                    : preflight.diagnostics.front().value(
                                              "message", "managed PCB state failed validation" ) );
        }

        std::set<std::string> relevantIds;

        for( const JSON& plannedOperation : managedOperations )
            relevantIds.emplace( plannedOperation["itemId"].get<std::string>() );

        if( !previousState.is_null() )
        {
            for( const JSON& item : previousState["managedPcbItems"] )
                relevantIds.emplace( item["itemId"].get<std::string>() );
        }

        KICHAD_IPC_CLIENT client( "org.kichad.codex.design", aIpcSocketDirectory );
        KICHAD_IPC_TARGET target;

        if( !client.FindOpenPcb( aProjectPath, board.GetFullPath(), target, pathError ) )
            return failure( "pcb_not_open", pathError );

        kiapi::board::BoardStackup previousStackup;

        if( desiredStackup && !queryPcbStackup( client, target, previousStackup, pathError ) )
            return failure( "stackup_inventory_failed", pathError );

        JSON liveInventory;

        if( !queryPcbInventory( client, target, relevantIds, liveInventory, pathError ) )
            return failure( "inventory_failed", pathError );

        if( !queryPcbFootprintInventory( client, target, placementReferences,
                                         liveInventory, pathError ) )
        {
            return failure( "inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT reconciled =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        reconcileOperations, previousState, liveInventory, reconcileContext );

        if( !reconciled.ok )
        {
            return failure( "reconcile_failed",
                            reconciled.diagnostics.empty()
                                    ? "managed PCB reconciliation failed"
                                    : reconciled.diagnostics.front().value(
                                              "message", "managed PCB reconciliation failed" ) );
        }

        bool zoneMutation = false;
        std::set<std::string> expectedZoneIds;

        for( const JSON& managedOperation : managedOperations )
        {
            if( managedOperation.value( "itemType", "" ) == "zone" )
                expectedZoneIds.emplace( managedOperation["itemId"].get<std::string>() );
        }

        for( const JSON& action : reconciled.actions )
        {
            if( action.value( "itemType", "" ) == "zone" )
                zoneMutation = true;
        }

        JSON applyJournal = { { "format", "kichad-kds-apply-journal" },
                              { "version", 1 },
                              { "sourcePath", sourceRelativePath },
                              { "boardPath", boardRelativePath },
                              { "projectName", projectName },
                              { "sourceSha256", compiled.sourceSha256 },
                              { "previousState", previousState },
                              { "preparedState", reconciled.nextState } };

        if( desiredStackup )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousStackup, &serializedPrevious, options );

            if( !status.ok() )
                return failure( "journal_failed", "could not serialize the prior board stackup" );

            applyJournal["previousStackup"] = JSON::parse( serializedPrevious );
        }

        if( !writeJsonAtomically( journalPath, applyJournal, pathError ) )
            return failure( "journal_failed", pathError );

        bool stackupApplied = false;

        if( desiredStackup )
        {
            if( !updatePcbStackup( client, target, *desiredStackup, pathError ) )
            {
                std::string rollbackError;

                if( !updatePcbStackup( client, target, previousStackup, rollbackError ) )
                    pathError += "; stackup rollback also failed: " + rollbackError;

                return failure( "stackup_apply_failed",
                                pathError + "; the apply journal was retained for safe recovery" );
            }

            stackupApplied = true;
        }

        auto rollbackStackup = [&]( std::string& aMessage )
        {
            if( !stackupApplied )
                return;

            std::string rollbackError;

            if( !updatePcbStackup( client, target, previousStackup, rollbackError ) )
                aMessage += "; stackup rollback also failed: " + rollbackError;
        };

        if( reconciled.actions.empty() )
        {
            if( !writeJsonAtomically( statePath, reconciled.nextState, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                rollbackStackup( message );
                return failure( "state_write_failed", message );
            }

            const bool journalRemoved = wxRemoveFile( journalPath.GetFullPath() );
            JSON payload = { { "operation", "apply" },
                             { "path", sourceRelativePath },
                             { "boardPath", boardRelativePath },
                             { "sourceSha256", compiled.sourceSha256 },
                             { "counts", reconciled.counts },
                             { "transaction",
                               stackupApplied ? "stackup applied" : "no board changes" },
                             { "stackupApplied", stackupApplied },
                             { "journalRetained", !journalRemoved } };
            return success( payload );
        }

        KICHAD_IPC_COMMIT_GUARD commit( client, target );

        if( !commit.Begin( pathError ) )
        {
            std::string message = pathError
                                  + "; the apply journal was retained for safe recovery";
            rollbackStackup( message );
            return failure( "transaction_failed", message );
        }

        if( !executePcbActions( client, target, reconciled.actions, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackStackup( message );

            return failure( "apply_failed", message );
        }

        if( !commit.Commit( "Apply KiChad Design Script " + sourceRelativePath, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackStackup( message );

            return failure( "transaction_failed", message );
        }

        if( zoneMutation && !refillPcbZones( client, target, expectedZoneIds, pathError ) )
        {
            return failure( "zone_refill_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        if( !writeJsonAtomically( statePath, reconciled.nextState, pathError ) )
        {
            return failure( "state_write_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        const bool journalRemoved = wxRemoveFile( journalPath.GetFullPath() );
        JSON payload = { { "operation", "apply" },
                         { "path", sourceRelativePath },
                         { "boardPath", boardRelativePath },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "counts", reconciled.counts },
                         { "managedItems",
                           reconciled.nextState["managedPcbItems"].size() },
                         { "transaction", "committed" },
                         { "stackupApplied", stackupApplied },
                         { "zonesRefilled", zoneMutation ? expectedZoneIds.size() : 0 },
                         { "statePath", statePath.GetFullName().ToStdString() },
                         { "journalRetained", !journalRemoved } };
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
        size_t totalItems = 0;
        bool   resultCapacityReached = false;

        for( const google::protobuf::Any& item : itemsResponse.items() )
        {
            if( ( itemType == "zone" || itemType == "rule_area" )
                && pcbAnyType( item ) != itemType )
            {
                continue;
            }

            ++totalItems;

            if( resultCapacityReached || items.size() >= static_cast<size_t>( limit ) )
            {
                resultCapacityReached = true;
                continue;
            }

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
            {
                resultCapacityReached = true;
                continue;
            }

            resultBytes += serialized.size();
            items.emplace_back( JSON::parse( serialized ) );
        }

        payload["itemType"] = itemType;
        payload["totalItems"] = totalItems;
        payload["items"] = std::move( items );
        payload["resultTruncated"] = payload["items"].size() < totalItems;
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
