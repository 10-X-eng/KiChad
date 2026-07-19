/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <vector>

#include <api/api_handler_common.h>
#include <build_version.h>
#include <eda_shape.h>
#include <eda_text.h>
#include <gestfich.h>
#include <geometry/shape_compound.h>
#include <google/protobuf/empty.pb.h>
#include <paths.h>
#include <pgm_base.h>
#include <api/api_plugin.h>
#include <api/api_utils.h>
#include <project/net_settings.h>
#include <project/project_file.h>
#include <settings/settings_manager.h>
#include <wx/string.h>

using namespace kiapi::common::commands;
using namespace kiapi::common::types;
using google::protobuf::Empty;


namespace
{

constexpr size_t MAX_NETCLASS_COUNT = 256;
constexpr size_t MAX_NETCLASS_ASSIGNMENTS = 1024;
constexpr size_t MAX_EXPANDED_NETCLASS_ASSIGNMENTS = 4096;
constexpr size_t MAX_NETCLASS_TEXT_BYTES = 256;


struct DECODED_NETCLASS_SETTINGS
{
    std::shared_ptr<NETCLASS>                             defaultClass;
    std::map<wxString, std::shared_ptr<NETCLASS>>         classes;
    std::vector<std::pair<wxString, wxString>>            assignments;
};


bool validColor( const kiapi::common::types::Color& aColor )
{
    return std::isfinite( aColor.r() ) && aColor.r() >= 0.0 && aColor.r() <= 1.0
           && std::isfinite( aColor.g() ) && aColor.g() >= 0.0 && aColor.g() <= 1.0
           && std::isfinite( aColor.b() ) && aColor.b() >= 0.0 && aColor.b() <= 1.0
           && std::isfinite( aColor.a() ) && aColor.a() >= 0.0 && aColor.a() <= 1.0;
}


bool validateNetclassDistance( bool aPresent, const kiapi::common::types::Distance& aDistance,
                               bool aRequired, int64_t aMinimum, int64_t aMaximum,
                               int64_t aQuantum, const std::string& aName, std::string& aError )
{
    if( !aPresent )
    {
        if( aRequired )
            aError = aName + " is required in the Default netclass";

        return !aRequired;
    }

    const int64_t value = aDistance.value_nm();

    if( value < aMinimum || value > aMaximum || value % aQuantum != 0 )
    {
        aError = aName + " is outside its exact native range or resolution";
        return false;
    }

    return true;
}


bool validateNetclassPadstack( const kiapi::board::types::PadStack& aStack, bool aRequired,
                               int64_t aMaximum, const std::string& aName,
                               std::optional<int64_t>& aDiameter,
                               std::optional<int64_t>& aDrill, std::string& aError )
{
    if( aStack.copper_layers_size() > 1 )
    {
        aError = aName + " supports exactly one circular all-layer diameter";
        return false;
    }

    if( aStack.copper_layers_size() == 1 )
    {
        const kiapi::board::types::PadStackLayer& layer = aStack.copper_layers( 0 );

        if( layer.layer() != kiapi::board::types::BL_F_Cu
            || layer.shape() != kiapi::board::types::PSS_CIRCLE || !layer.has_size()
            || layer.size().x_nm() != layer.size().y_nm() || layer.size().x_nm() <= 0
            || layer.size().x_nm() > aMaximum )
        {
            aError = aName + " diameter must be a bounded circular F.Cu all-layer entry";
            return false;
        }

        aDiameter = layer.size().x_nm();
    }

    if( aStack.has_drill() )
    {
        const kiapi::board::types::DrillProperties& drill = aStack.drill();

        if( !drill.has_diameter() || drill.diameter().x_nm() != drill.diameter().y_nm()
            || drill.diameter().x_nm() <= 0 || drill.diameter().x_nm() > aMaximum
            || ( drill.shape() != kiapi::board::types::DS_UNKNOWN
                 && drill.shape() != kiapi::board::types::DS_CIRCLE ) )
        {
            aError = aName + " drill must be a bounded circular diameter";
            return false;
        }

        aDrill = drill.diameter().x_nm();
    }

    if( aRequired && ( !aDiameter || !aDrill ) )
    {
        aError = aName + " diameter and drill are required in the Default netclass";
        return false;
    }

    return true;
}


bool boundedBusRanges( const std::string& aPattern )
{
    size_t range = 0;

    while( ( range = aPattern.find( "..", range ) ) != std::string::npos )
    {
        const size_t open = aPattern.rfind( '[', range );
        const size_t close = aPattern.find( ']', range + 2 );

        if( open != std::string::npos && close != std::string::npos )
        {
            int64_t first = 0;
            int64_t last = 0;
            const char* firstBegin = aPattern.data() + open + 1;
            const char* firstEnd = aPattern.data() + range;
            const char* lastBegin = aPattern.data() + range + 2;
            const char* lastEnd = aPattern.data() + close;
            std::from_chars_result firstResult =
                    std::from_chars( firstBegin, firstEnd, first );
            std::from_chars_result lastResult = std::from_chars( lastBegin, lastEnd, last );

            if( firstResult.ec == std::errc() && firstResult.ptr == firstEnd
                && lastResult.ec == std::errc() && lastResult.ptr == lastEnd
                && std::fabs( static_cast<long double>( first )
                              - static_cast<long double>( last ) ) > 255.0L )
            {
                return false;
            }
        }

        range += 2;
    }

    return true;
}


bool decodeNetClassSettings( const kiapi::common::project::NetClassSettings& aSettings,
                             DECODED_NETCLASS_SETTINGS& aDecoded, std::string& aError )
{
    using kiapi::common::project::NetClass;
    using kiapi::common::project::NCT_EXPLICIT;
    using kiapi::common::types::SLS_DASH;
    using kiapi::common::types::SLS_DASHDOT;
    using kiapi::common::types::SLS_DASHDOTDOT;
    using kiapi::common::types::SLS_DOT;
    using kiapi::common::types::SLS_SOLID;

    if( aSettings.net_classes_size() < 1
        || aSettings.net_classes_size() > static_cast<int>( MAX_NETCLASS_COUNT ) )
    {
        aError = "netclass settings require 1 through 256 explicit classes";
        return false;
    }

    std::set<std::string> foldedNames;
    std::set<std::string> exactNames;
    std::set<int> priorities;
    int defaultCount = 0;

    for( const NetClass& netClass : aSettings.net_classes() )
    {
        wxString name = wxString::FromUTF8( netClass.name() );
        wxString trimmed = name;
        trimmed.Trim( true );
        trimmed.Trim( false );
        const bool isDefault = name == wxS( "Default" );

        if( name.empty() || name != trimmed || netClass.name().size() > MAX_NETCLASS_TEXT_BYTES )
        {
            aError = "netclass names must be non-empty, trimmed, and at most 256 bytes";
            return false;
        }

        if( !foldedNames.emplace( name.Lower().ToStdString() ).second )
        {
            aError = "netclass names must be unique without regard to case";
            return false;
        }

        exactNames.emplace( netClass.name() );

        if( netClass.type() != NCT_EXPLICIT || netClass.constituents_size() != 0 )
        {
            aError = "only explicit root netclasses may be updated";
            return false;
        }

        if( !netClass.has_priority() )
        {
            aError = "every netclass requires an explicit priority";
            return false;
        }

        if( isDefault )
        {
            ++defaultCount;

            if( netClass.priority() != std::numeric_limits<int32_t>::max() )
            {
                aError = "Default netclass priority must use the canonical maximum value";
                return false;
            }

            if( netClass.board().has_color() || netClass.schematic().has_color() )
            {
                aError = "Default netclass colors are controlled by the editor theme";
                return false;
            }
        }
        else if( netClass.priority() < 0 || !priorities.emplace( netClass.priority() ).second )
        {
            aError = "non-Default netclass priorities must be unique non-negative integers";
            return false;
        }

        const auto& board = netClass.board();
        const auto& schematic = netClass.schematic();

        if( !validateNetclassDistance( board.has_clearance(), board.clearance(), isDefault, 0,
                                       500000000, 1, netClass.name() + ".clearance", aError )
            || !validateNetclassDistance( board.has_track_width(), board.track_width(), isDefault,
                                          1, 25000000, 1,
                                          netClass.name() + ".track_width", aError )
            || !validateNetclassDistance( board.has_diff_pair_track_width(),
                                          board.diff_pair_track_width(), isDefault, 1, 25000000, 1,
                                          netClass.name() + ".diff_pair_track_width", aError )
            || !validateNetclassDistance( board.has_diff_pair_gap(), board.diff_pair_gap(),
                                          isDefault, 0, 100000000, 1,
                                          netClass.name() + ".diff_pair_gap", aError )
            || !validateNetclassDistance( board.has_diff_pair_via_gap(),
                                          board.diff_pair_via_gap(), isDefault, 0, 100000000, 1,
                                          netClass.name() + ".diff_pair_via_gap", aError )
            || !validateNetclassDistance( schematic.has_wire_width(), schematic.wire_width(),
                                          isDefault, 100, 100000000, 100,
                                          netClass.name() + ".wire_width", aError )
            || !validateNetclassDistance( schematic.has_bus_width(), schematic.bus_width(),
                                          isDefault, 100, 100000000, 100,
                                          netClass.name() + ".bus_width", aError ) )
        {
            return false;
        }

        std::optional<int64_t> viaDiameter;
        std::optional<int64_t> viaDrill;
        std::optional<int64_t> microviaDiameter;
        std::optional<int64_t> microviaDrill;

        if( ( board.has_via_stack()
              && !validateNetclassPadstack( board.via_stack(), isDefault, 25000000,
                                            netClass.name() + ".via", viaDiameter, viaDrill,
                                            aError ) )
            || ( !board.has_via_stack() && isDefault ) )
        {
            if( aError.empty() )
                aError = netClass.name() + ".via diameter and drill are required";

            return false;
        }

        if( ( board.has_microvia_stack()
              && !validateNetclassPadstack( board.microvia_stack(), isDefault, 10000000,
                                            netClass.name() + ".microvia", microviaDiameter,
                                            microviaDrill, aError ) )
            || ( !board.has_microvia_stack() && isDefault ) )
        {
            if( aError.empty() )
                aError = netClass.name() + ".microvia diameter and drill are required";

            return false;
        }

        if( viaDiameter && viaDrill && *viaDiameter < *viaDrill )
        {
            aError = netClass.name() + ".via diameter cannot be smaller than its drill";
            return false;
        }

        if( microviaDiameter && microviaDrill && *microviaDiameter < *microviaDrill )
        {
            aError = netClass.name() + ".microvia diameter cannot be smaller than its drill";
            return false;
        }

        if( board.has_color() && !validColor( board.color() ) )
        {
            aError = netClass.name() + ".pcb_color has an invalid channel";
            return false;
        }

        if( schematic.has_color() && !validColor( schematic.color() ) )
        {
            aError = netClass.name() + ".schematic_color has an invalid channel";
            return false;
        }

        if( board.has_tuning_profile()
            && board.tuning_profile().size() > MAX_NETCLASS_TEXT_BYTES )
        {
            aError = netClass.name() + ".tuning_profile exceeds 256 bytes";
            return false;
        }

        if( isDefault && !schematic.has_line_style() )
        {
            aError = "Default.line_style is required";
            return false;
        }

        if( schematic.has_line_style()
            && schematic.line_style() != SLS_SOLID && schematic.line_style() != SLS_DASH
            && schematic.line_style() != SLS_DOT && schematic.line_style() != SLS_DASHDOT
            && schematic.line_style() != SLS_DASHDOTDOT )
        {
            aError = netClass.name() + ".line_style is invalid";
            return false;
        }

        google::protobuf::Any any;
        any.PackFrom( netClass );
        std::shared_ptr<NETCLASS> decoded = std::make_shared<NETCLASS>( name, false );

        if( !decoded->Deserialize( any ) )
        {
            aError = "could not decode validated netclass " + netClass.name();
            return false;
        }

        if( isDefault )
            aDecoded.defaultClass = std::move( decoded );
        else
            aDecoded.classes.emplace( name, std::move( decoded ) );
    }

    if( defaultCount != 1 )
    {
        aError = "netclass settings require exactly one class named Default";
        return false;
    }

    if( priorities.size() != aDecoded.classes.size()
        || ( !priorities.empty()
             && ( *priorities.begin() != 0
                  || *priorities.rbegin() != static_cast<int>( priorities.size() ) - 1 ) ) )
    {
        aError = "non-Default netclass priorities must form the canonical sequence from zero";
        return false;
    }

    if( aSettings.assignments_size() > static_cast<int>( MAX_NETCLASS_ASSIGNMENTS ) )
    {
        aError = "netclass settings support at most 1024 pattern assignments";
        return false;
    }

    std::set<std::pair<std::string, std::string>> seenAssignments;
    size_t expandedAssignments = 0;

    for( const kiapi::common::project::NetClassAssignment& assignment :
         aSettings.assignments() )
    {
        wxString pattern = wxString::FromUTF8( assignment.pattern() );
        wxString trimmed = pattern;
        trimmed.Trim( true );
        trimmed.Trim( false );

        if( pattern.empty() || trimmed.empty()
            || assignment.pattern().size() > MAX_NETCLASS_TEXT_BYTES
            || assignment.net_class().size() > MAX_NETCLASS_TEXT_BYTES )
        {
            aError = "netclass assignments require bounded non-empty patterns and class names";
            return false;
        }

        if( assignment.net_class() == "Default" || !exactNames.contains( assignment.net_class() ) )
        {
            aError = "netclass assignment references an unknown or redundant Default class";
            return false;
        }

        if( !seenAssignments.emplace( assignment.pattern(), assignment.net_class() ).second )
        {
            aError = "duplicate netclass pattern assignment";
            return false;
        }

        if( !boundedBusRanges( assignment.pattern() ) )
        {
            aError = "netclass bus patterns may expand at most 256 members per range";
            return false;
        }

        NET_SETTINGS::ForEachBusMember(
                pattern,
                [&]( const wxString& )
                {
                    ++expandedAssignments;
                } );

        if( expandedAssignments > MAX_EXPANDED_NETCLASS_ASSIGNMENTS )
        {
            aError = "netclass patterns expand to more than 4096 assignments";
            return false;
        }

        aDecoded.assignments.emplace_back( pattern,
                                           wxString::FromUTF8( assignment.net_class() ) );
    }

    return true;
}


kiapi::common::commands::NetClassSettingsResponse encodeNetClassSettings( NET_SETTINGS& aSettings )
{
    kiapi::common::commands::NetClassSettingsResponse response;
    kiapi::common::project::NetClassSettings* settings = response.mutable_settings();
    google::protobuf::Any any;
    aSettings.GetDefaultNetclass()->Serialize( any );
    any.UnpackTo( settings->add_net_classes() );
    std::vector<std::shared_ptr<NETCLASS>> classes;

    for( const auto& netClass : aSettings.GetNetclasses() | std::views::values )
        classes.emplace_back( netClass );

    std::sort( classes.begin(), classes.end(),
               []( const std::shared_ptr<NETCLASS>& aLeft,
                   const std::shared_ptr<NETCLASS>& aRight )
               {
                   if( aLeft->GetPriority() != aRight->GetPriority() )
                       return aLeft->GetPriority() < aRight->GetPriority();

                   return aLeft->GetName() < aRight->GetName();
               } );

    for( const std::shared_ptr<NETCLASS>& netClass : classes )
    {
        netClass->Serialize( any );
        any.UnpackTo( settings->add_net_classes() );
    }

    for( const auto& [matcher, netClass] : aSettings.GetNetclassPatternAssignments() )
    {
        kiapi::common::project::NetClassAssignment* assignment = settings->add_assignments();
        assignment->set_pattern( matcher->GetPattern().ToUTF8() );
        assignment->set_net_class( netClass.ToUTF8() );
    }

    return response;
}

} // namespace


API_HANDLER_COMMON::API_HANDLER_COMMON( std::function<void()> aOnNetClassSettingsChanged ) :
        API_HANDLER(),
        m_onNetClassSettingsChanged( std::move( aOnNetClassSettingsChanged ) )
{
    registerHandler<commands::GetVersion, GetVersionResponse>( &API_HANDLER_COMMON::handleGetVersion );
    registerHandler<GetKiCadBinaryPath, PathResponse>(
            &API_HANDLER_COMMON::handleGetKiCadBinaryPath );
    registerHandler<GetNetClasses, NetClassesResponse>( &API_HANDLER_COMMON::handleGetNetClasses );
    registerHandler<SetNetClasses, Empty>( &API_HANDLER_COMMON::handleSetNetClasses );
    registerHandler<GetNetClassSettings, NetClassSettingsResponse>(
            &API_HANDLER_COMMON::handleGetNetClassSettings );
    registerHandler<UpdateNetClassSettings, NetClassSettingsResponse>(
            &API_HANDLER_COMMON::handleUpdateNetClassSettings );
    registerHandler<Ping, Empty>( &API_HANDLER_COMMON::handlePing );
    registerHandler<GetTextExtents, types::Box2>( &API_HANDLER_COMMON::handleGetTextExtents );
    registerHandler<GetTextAsShapes, GetTextAsShapesResponse>(
            &API_HANDLER_COMMON::handleGetTextAsShapes );
    registerHandler<ExpandTextVariables, ExpandTextVariablesResponse>(
            &API_HANDLER_COMMON::handleExpandTextVariables );
    registerHandler<GetPluginSettingsPath, StringResponse>(
            &API_HANDLER_COMMON::handleGetPluginSettingsPath );
    registerHandler<GetTextVariables, project::TextVariables>(
            &API_HANDLER_COMMON::handleGetTextVariables );
    registerHandler<SetTextVariables, Empty>(
            &API_HANDLER_COMMON::handleSetTextVariables );

}


HANDLER_RESULT<GetVersionResponse> API_HANDLER_COMMON::handleGetVersion(
        const HANDLER_CONTEXT<commands::GetVersion>& )
{
    GetVersionResponse reply;

    reply.mutable_version()->set_full_version( GetBuildVersion().ToStdString() );

    std::tuple<int, int, int> version = GetMajorMinorPatchTuple();
    reply.mutable_version()->set_major( std::get<0>( version ) );
    reply.mutable_version()->set_minor( std::get<1>( version ) );
    reply.mutable_version()->set_patch( std::get<2>( version ) );

    return reply;
}


HANDLER_RESULT<PathResponse> API_HANDLER_COMMON::handleGetKiCadBinaryPath(
        const HANDLER_CONTEXT<GetKiCadBinaryPath>& aCtx )
{
    wxFileName fn( wxEmptyString, wxString::FromUTF8( aCtx.Request.binary_name() ) );
#ifdef _WIN32
    fn.SetExt( wxT( "exe" ) );
#endif

    wxString path = FindKicadFile( fn.GetFullName() );
    PathResponse reply;
    reply.set_path( path.ToUTF8() );
    return reply;
}


HANDLER_RESULT<NetClassesResponse> API_HANDLER_COMMON::handleGetNetClasses(
        const HANDLER_CONTEXT<GetNetClasses>& aCtx )
{
    NetClassesResponse reply;

    std::shared_ptr<NET_SETTINGS>& netSettings =
            Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;

    google::protobuf::Any any;

    netSettings->GetDefaultNetclass()->Serialize( any );
    any.UnpackTo( reply.add_net_classes() );

    for( const auto& netClass : netSettings->GetNetclasses() | std::views::values )
    {
        netClass->Serialize( any );
        any.UnpackTo( reply.add_net_classes() );
    }

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleSetNetClasses(
        const HANDLER_CONTEXT<SetNetClasses>& aCtx )
{
    std::shared_ptr<NET_SETTINGS>& netSettings =
            Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;
    // Open editors keep raw NETCLASS pointers on their NETINFO_ITEMs.  Retain the prior shared
    // objects until the synchronous editor callback has repointed every live net.
    const std::shared_ptr<NETCLASS> previousDefault = netSettings->GetDefaultNetclass();
    const auto previousNetClasses = netSettings->GetNetclasses();

    if( aCtx.Request.merge_mode() == MapMergeMode::MMM_REPLACE )
        netSettings->ClearNetclasses();

    auto netClasses = netSettings->GetNetclasses();
    google::protobuf::Any any;

    for( const auto& ncProto : aCtx.Request.net_classes() )
    {
        any.PackFrom( ncProto );
        wxString name = wxString::FromUTF8( ncProto.name() );

        if( name == wxT( "Default" ) )
        {
            netSettings->GetDefaultNetclass()->Deserialize( any );
        }
        else
        {
            if( !netClasses.contains( name ) )
                netClasses.insert( { name, std::make_shared<NETCLASS>( name, false ) } );

            netClasses[name]->Deserialize( any );
        }
    }

    netSettings->SetNetclasses( netClasses );

    if( m_onNetClassSettingsChanged )
        m_onNetClassSettingsChanged();

    return Empty();
}


HANDLER_RESULT<NetClassSettingsResponse> API_HANDLER_COMMON::handleGetNetClassSettings(
        const HANDLER_CONTEXT<GetNetClassSettings>& )
{
    PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_NOT_READY );
        error.set_error_message( "no valid project is loaded, cannot get netclass settings" );
        return tl::unexpected( error );
    }

    NET_SETTINGS& settings = *project.GetProjectFile().m_NetSettings;
    return encodeNetClassSettings( settings );
}


HANDLER_RESULT<NetClassSettingsResponse> API_HANDLER_COMMON::handleUpdateNetClassSettings(
        const HANDLER_CONTEXT<UpdateNetClassSettings>& aCtx )
{
    PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_NOT_READY );
        error.set_error_message( "no valid project is loaded, cannot update netclass settings" );
        return tl::unexpected( error );
    }

    DECODED_NETCLASS_SETTINGS decoded;
    std::string               decodeError;

    if( !aCtx.Request.has_settings()
        || !decodeNetClassSettings( aCtx.Request.settings(), decoded, decodeError ) )
    {
        ApiResponseStatus error;
        error.set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.set_error_message( decodeError.empty() ? "netclass settings are required"
                                                     : decodeError );
        return tl::unexpected( error );
    }

    NET_SETTINGS& settings = *project.GetProjectFile().m_NetSettings;
    // BOARD net records cache raw pointers into these shared objects.  Keep the complete previous
    // table alive until all editor caches have been synchronously refreshed by the callback.
    const std::shared_ptr<NETCLASS> previousDefault = settings.GetDefaultNetclass();
    const auto previousNetClasses = settings.GetNetclasses();
    settings.SetDefaultNetclass( std::move( decoded.defaultClass ) );
    settings.SetNetclasses( decoded.classes );
    settings.ClearNetclassPatternAssignments();

    for( const auto& [pattern, netClass] : decoded.assignments )
        settings.SetNetclassPatternAssignment( pattern, netClass );

    if( m_onNetClassSettingsChanged )
        m_onNetClassSettingsChanged();

    Pgm().GetSettingsManager().SaveProject();
    return encodeNetClassSettings( settings );
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handlePing( const HANDLER_CONTEXT<Ping>& aCtx )
{
    return Empty();
}


HANDLER_RESULT<types::Box2> API_HANDLER_COMMON::handleGetTextExtents(
        const HANDLER_CONTEXT<GetTextExtents>& aCtx )
{
    EDA_TEXT text( pcbIUScale );
    google::protobuf::Any any;
    any.PackFrom( aCtx.Request.text() );

    if( !text.Deserialize( any ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Could not decode text in GetTextExtents message" );
        return tl::unexpected( e );
    }

    types::Box2 response;

    BOX2I bbox = text.GetTextBox( nullptr );
    EDA_ANGLE angle = text.GetTextAngle();

    if( !angle.IsZero() )
        bbox = bbox.GetBoundingBoxRotated( text.GetTextPos(), text.GetTextAngle() );

    response.mutable_position()->set_x_nm( bbox.GetPosition().x );
    response.mutable_position()->set_y_nm( bbox.GetPosition().y );
    response.mutable_size()->set_x_nm( bbox.GetSize().x );
    response.mutable_size()->set_y_nm( bbox.GetSize().y );

    return response;
}


HANDLER_RESULT<GetTextAsShapesResponse> API_HANDLER_COMMON::handleGetTextAsShapes(
        const HANDLER_CONTEXT<GetTextAsShapes>& aCtx )
{
    GetTextAsShapesResponse reply;

    for( const TextOrTextBox& textMsg : aCtx.Request.text() )
    {
        Text dummyText;
        const Text* textPtr = &textMsg.text();

        if( textMsg.has_textbox() )
        {
            dummyText.set_text( textMsg.textbox().text() );
            dummyText.mutable_attributes()->CopyFrom( textMsg.textbox().attributes() );
            textPtr = &dummyText;
        }

        EDA_TEXT text( pcbIUScale );
        google::protobuf::Any any;
        any.PackFrom( *textPtr );

        if( !text.Deserialize( any ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Could not decode text in GetTextAsShapes message" );
            return tl::unexpected( e );
        }

        std::shared_ptr<SHAPE_COMPOUND> shapes = text.GetEffectiveTextShape( false );

        TextWithShapes* entry = reply.add_text_with_shapes();
        entry->mutable_text()->CopyFrom( textMsg );

        for( SHAPE* subshape : shapes->Shapes() )
        {
            EDA_SHAPE proxy( *subshape );
            proxy.Serialize( any );
            GraphicShape* shapeMsg = entry->mutable_shapes()->add_shapes();
            any.UnpackTo( shapeMsg );
        }

        if( textMsg.has_textbox() )
        {
            GraphicShape* border = entry->mutable_shapes()->add_shapes();
            int width = textMsg.textbox().attributes().stroke_width().value_nm();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            VECTOR2I tl = UnpackVector2( textMsg.textbox().top_left() );
            VECTOR2I br = UnpackVector2( textMsg.textbox().bottom_right() );

            // top
            PackVector2( *border->mutable_segment()->mutable_start(), tl );
            PackVector2( *border->mutable_segment()->mutable_end(), VECTOR2I( br.x, tl.y ) );

            // right
            border = entry->mutable_shapes()->add_shapes();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            PackVector2( *border->mutable_segment()->mutable_start(), VECTOR2I( br.x, tl.y ) );
            PackVector2( *border->mutable_segment()->mutable_end(), br );

            // bottom
            border = entry->mutable_shapes()->add_shapes();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            PackVector2( *border->mutable_segment()->mutable_start(), br );
            PackVector2( *border->mutable_segment()->mutable_end(), VECTOR2I( tl.x, br.y ) );

            // left
            border = entry->mutable_shapes()->add_shapes();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            PackVector2( *border->mutable_segment()->mutable_start(), VECTOR2I( tl.x, br.y ) );
            PackVector2( *border->mutable_segment()->mutable_end(), tl );
        }
    }

    return reply;
}


HANDLER_RESULT<ExpandTextVariablesResponse> API_HANDLER_COMMON::handleExpandTextVariables(
        const HANDLER_CONTEXT<ExpandTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    ExpandTextVariablesResponse reply;
    PROJECT& project = Pgm().GetSettingsManager().Prj();

    for( const std::string& textMsg : aCtx.Request.text() )
    {
        wxString result = ExpandTextVars( wxString::FromUTF8( textMsg ), &project );
        reply.add_text( result.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<StringResponse> API_HANDLER_COMMON::handleGetPluginSettingsPath(
        const HANDLER_CONTEXT<GetPluginSettingsPath>& aCtx )
{
    wxString identifier = wxString::FromUTF8( aCtx.Request.identifier() );

    if( identifier.IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "plugin identifier is missing" );
        return tl::unexpected( e );
    }

    if( API_PLUGIN::IsValidIdentifier( identifier ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "plugin identifier is invalid" );
        return tl::unexpected( e );
    }

    wxFileName path( PATHS::GetUserSettingsPath(), wxEmptyString );
    path.AppendDir( "plugins" );

    // Create the base plugins path if needed, but leave the specific plugin to create its own path
    PATHS::EnsurePathExists( path.GetPath() );

    path.AppendDir( identifier );

    StringResponse reply;
    reply.set_response( path.GetPath() );
    return reply;
}


HANDLER_RESULT<project::TextVariables> API_HANDLER_COMMON::handleGetTextVariables(
        const HANDLER_CONTEXT<GetTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    const PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "no valid project is loaded, cannot get text variables" );
        return tl::unexpected( e );
    }

    const std::map<wxString, wxString>& vars = project.GetTextVars();

    project::TextVariables reply;
    auto map = reply.mutable_variables();

    for( const auto& [key, value] : vars )
        ( *map )[ std::string( key.ToUTF8() ) ] = value.ToUTF8();

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleSetTextVariables(
    const HANDLER_CONTEXT<SetTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "no valid project is loaded, cannot set text variables" );
        return tl::unexpected( e );
    }

    const project::TextVariables& newVars = aCtx.Request.variables();
    std::map<wxString, wxString>& vars = project.GetTextVars();

    if( aCtx.Request.merge_mode() == MapMergeMode::MMM_REPLACE )
        vars.clear();

    for( const auto& [key, value] : newVars.variables() )
        vars[wxString::FromUTF8( key )] = wxString::FromUTF8( value );

    Pgm().GetSettingsManager().SaveProject();

    return Empty();
}
