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

#include "design_script_board_asset_compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include <picosha2.h>
#include <wx/base64.h>
#include <wx/mstream.h>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using RESULT = KICHAD::DESIGN_SCRIPT_BOARD_ASSET_COMPILER::RESULT;

constexpr size_t MAX_IDENTIFIER_BYTES = 128;
constexpr size_t MAX_DESCRIPTION_BYTES = 4096;
constexpr size_t MAX_BARCODE_TEXT_BYTES = 4096;
constexpr size_t MAX_IMAGE_BYTES = 8 * 1024 * 1024;
constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool scalarText( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( aNode >= aDocument.Nodes().size()
        || aDocument.Nodes()[aNode].kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool singleValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], aValue );
}


bool validIdentifier( const std::string& aValue )
{
    return !aValue.empty() && aValue.size() <= MAX_IDENTIFIER_BYTES
           && std::all_of( aValue.begin(), aValue.end(),
                           []( unsigned char aCharacter )
                           {
                               return std::isalnum( aCharacter ) || aCharacter == '_'
                                      || aCharacter == '-' || aCharacter == '+'
                                      || aCharacter == '.' || aCharacter == '/'
                                      || aCharacter == '#';
                           } );
}


bool parseDistance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), value );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( value ) )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( aText.data() + aText.size()
                                                      - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm" )
        scale = 1'000'000.0L;
    else if( unit == "mil" )
        scale = 25'400.0L;
    else if( unit == "um" )
        scale = 1'000.0L;
    else if( unit == "nm" )
        scale = 1.0L;
    else if( unit == "in" )
        scale = 25'400'000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( !std::isfinite( rounded ) || rounded < -MAX_COORDINATE_NM
        || rounded > MAX_COORDINATE_NM )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool parsePoint( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 3
        || !scalarText( aDocument, node.children[1], xText )
        || !scalarText( aDocument, node.children[2], yText )
        || !parseDistance( xText, x ) || !parseDistance( yText, y ) )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool parseBoolean( const DOCUMENT& aDocument, size_t aNode, bool& aValue )
{
    std::string value;

    if( !singleValue( aDocument, aNode, value ) || ( value != "true" && value != "false" ) )
        return false;

    aValue = value == "true";
    return true;
}


bool parseDecimal( const std::string& aText, double& aValue )
{
    const std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), aValue );
    return converted.ec == std::errc() && converted.ptr == aText.data() + aText.size()
           && std::isfinite( aValue );
}


bool parseAngle( const std::string& aText, double& aDegrees )
{
    const std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), aDegrees );

    return converted.ec == std::errc() && converted.ptr != aText.data()
           && std::string_view( converted.ptr,
                                static_cast<size_t>( aText.data() + aText.size()
                                                     - converted.ptr ) ) == "deg"
           && std::isfinite( aDegrees ) && std::abs( aDegrees ) <= 360'000.0;
}


bool boardLayer( const std::string& aLayer )
{
    static const std::set<std::string> FIXED_LAYERS = {
        "F.Cu", "B.Cu", "B.Adhes", "F.Adhes", "B.Paste", "F.Paste",
        "B.SilkS", "F.SilkS", "B.Mask", "F.Mask", "Dwgs.User", "Cmts.User",
        "Eco1.User", "Eco2.User", "Edge.Cuts", "Margin", "B.CrtYd", "F.CrtYd",
        "B.Fab", "F.Fab"
    };

    if( FIXED_LAYERS.contains( aLayer ) )
        return true;

    const auto numberedLayer = [&]( std::string_view aPrefix, std::string_view aSuffix,
                                    int aMinimum, int aMaximum )
    {
        if( !aLayer.starts_with( aPrefix ) || !aLayer.ends_with( aSuffix ) )
            return false;

        const char* begin = aLayer.data() + aPrefix.size();
        const char* end = aLayer.data() + aLayer.size() - aSuffix.size();
        int value = 0;
        const std::from_chars_result converted = std::from_chars( begin, end, value );
        return converted.ec == std::errc() && converted.ptr == end
               && value >= aMinimum && value <= aMaximum;
    };

    return numberedLayer( "In", ".Cu", 1, 30 )
           || numberedLayer( "User.", "", 1, 45 );
}


std::string imageMediaType( const std::vector<char>& aBytes )
{
    static const unsigned char PNG[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

    if( aBytes.size() >= sizeof( PNG )
        && std::equal( std::begin( PNG ), std::end( PNG ),
                       reinterpret_cast<const unsigned char*>( aBytes.data() ) ) )
    {
        return "image/png";
    }

    if( aBytes.size() >= 3
        && static_cast<unsigned char>( aBytes[0] ) == 0xff
        && static_cast<unsigned char>( aBytes[1] ) == 0xd8
        && static_cast<unsigned char>( aBytes[2] ) == 0xff )
    {
        return "image/jpeg";
    }

    if( aBytes.size() >= 6
        && ( std::equal( aBytes.begin(), aBytes.begin() + 6, "GIF87a" )
             || std::equal( aBytes.begin(), aBytes.begin() + 6, "GIF89a" ) ) )
    {
        return "image/gif";
    }

    if( aBytes.size() >= 2 && aBytes[0] == 'B' && aBytes[1] == 'M' )
        return "image/bmp";

    if( aBytes.size() >= 12 && std::equal( aBytes.begin(), aBytes.begin() + 4, "RIFF" )
        && std::equal( aBytes.begin() + 8, aBytes.begin() + 12, "WEBP" ) )
    {
        return "image/webp";
    }

    return {};
}


bool collectFields( const DOCUMENT& aDocument, const DOCUMENT::NODE& aNode,
                    const std::set<std::string>& aAllowed,
                    std::map<std::string, size_t>& aFields, RESULT& aResult,
                    const std::string& aKind )
{
    bool valid = true;

    for( size_t index = 2; index < aNode.children.size(); ++index )
    {
        const size_t child = aNode.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !aAllowed.contains( head ) )
        {
            diagnostic( aResult, "unknown_board_" + aKind + "_field",
                        "board " + aKind + " does not support field '" + head + "'" );
            valid = false;
        }
        else if( !aFields.emplace( head, child ).second )
        {
            diagnostic( aResult, "duplicate_board_" + aKind + "_field",
                        "board " + aKind + " field '" + head + "' occurs more than once" );
            valid = false;
        }
    }

    return valid;
}


JSON compileImage( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "invalid_board_image_id",
                    "board image requires one bounded stable ID" );
        return JSON::object();
    }

    static const std::set<std::string> ALLOWED = {
        "at", "origin_offset", "layer", "scale", "locked", "media_type",
        "sha256", "description", "data_base64"
    };
    std::map<std::string, size_t> fields;
    collectFields( aDocument, node, ALLOWED, fields, aResult, "image" );
    JSON image = { { "kind", "reference_image" }, { "logicalId", id }, { "typed", true } };

    for( const char* required : { "at", "origin_offset", "layer", "scale", "locked",
                                  "media_type", "sha256", "description", "data_base64" } )
    {
        if( !fields.contains( required ) )
            diagnostic( aResult, "missing_board_image_field",
                        "board image " + id + " is missing " + required );
    }

    JSON point;

    if( fields.contains( "at" ) && parsePoint( aDocument, fields["at"], point ) )
        image["position"] = std::move( point );
    else if( fields.contains( "at" ) )
        diagnostic( aResult, "invalid_board_image_at", "board image at requires two distances" );

    if( fields.contains( "origin_offset" )
        && parsePoint( aDocument, fields["origin_offset"], point ) )
    {
        image["originOffset"] = std::move( point );
    }
    else if( fields.contains( "origin_offset" ) )
    {
        diagnostic( aResult, "invalid_board_image_origin_offset",
                    "board image origin_offset requires two distances" );
    }

    std::string value;

    if( fields.contains( "layer" ) )
    {
        if( !singleValue( aDocument, fields["layer"], value ) || !boardLayer( value ) )
            diagnostic( aResult, "invalid_board_image_layer", "board image layer is not a KiCad board layer" );
        else
            image["layer"] = value;
    }

    if( fields.contains( "scale" ) )
    {
        double scale = 0.0;

        if( !singleValue( aDocument, fields["scale"], value ) || !parseDecimal( value, scale )
            || scale < 0.001 || scale > 1000.0 )
        {
            diagnostic( aResult, "invalid_board_image_scale",
                        "board image scale must be a finite factor from 0.001 through 1000" );
        }
        else
        {
            image["scale"] = scale;
        }
    }

    if( fields.contains( "locked" ) )
    {
        bool locked = false;

        if( !parseBoolean( aDocument, fields["locked"], locked ) )
            diagnostic( aResult, "invalid_board_image_locked", "board image locked must be true or false" );
        else
            image["locked"] = locked;
    }

    static const std::set<std::string> MEDIA_TYPES = {
        "image/png", "image/jpeg", "image/gif", "image/bmp", "image/webp"
    };

    if( fields.contains( "media_type" ) )
    {
        if( !singleValue( aDocument, fields["media_type"], value )
            || !MEDIA_TYPES.contains( value ) )
        {
            diagnostic( aResult, "invalid_board_image_media_type",
                        "board image media_type must be image/png|jpeg|gif|bmp|webp" );
        }
        else
        {
            image["mediaType"] = value;
        }
    }

    if( fields.contains( "sha256" ) )
    {
        if( !singleValue( aDocument, fields["sha256"], value ) || value.size() != 64
            || !std::all_of( value.begin(), value.end(),
                             []( unsigned char aCharacter )
                             {
                                 return std::isdigit( aCharacter )
                                        || ( aCharacter >= 'a' && aCharacter <= 'f' );
                             } ) )
        {
            diagnostic( aResult, "invalid_board_image_sha256",
                        "board image sha256 requires one lowercase 64-digit digest" );
        }
        else
        {
            image["sha256"] = value;
        }
    }

    if( fields.contains( "description" ) )
    {
        if( !singleValue( aDocument, fields["description"], value ) || value.empty()
            || value.size() > MAX_DESCRIPTION_BYTES || value.find( '\0' ) != std::string::npos )
        {
            diagnostic( aResult, "invalid_board_image_description",
                        "board image description requires 1 through 4096 UTF-8 bytes" );
        }
        else
        {
            image["description"] = value;
        }
    }

    std::vector<char> decoded;

    if( fields.contains( "data_base64" ) )
    {
        size_t errorPosition = 0;

        if( !singleValue( aDocument, fields["data_base64"], value ) || value.empty()
            || value.size() > ( MAX_IMAGE_BYTES * 4 / 3 + 4 ) )
        {
            diagnostic( aResult, "invalid_board_image_data",
                        "board image data_base64 must encode 1 byte through 8 MiB" );
        }
        else
        {
            const wxMemoryBuffer buffer = wxBase64Decode( value.data(), value.size(),
                                                           wxBase64DecodeMode_Strict,
                                                           &errorPosition );

            if( buffer.GetDataLen() == 0 || buffer.GetDataLen() > MAX_IMAGE_BYTES )
            {
                diagnostic( aResult, "invalid_board_image_data",
                            "board image data_base64 is malformed or exceeds 8 MiB decoded" );
            }
            else
            {
                const char* bytes = static_cast<const char*>( buffer.GetData() );
                decoded.assign( bytes, bytes + buffer.GetDataLen() );
                image["dataBase64"] = value;
                image["byteCount"] = decoded.size();
            }
        }
    }

    if( !decoded.empty() && image.contains( "mediaType" ) && image.contains( "sha256" ) )
    {
        const std::string detected = imageMediaType( decoded );
        std::string digest;
        picosha2::hash256_hex_string( decoded, digest );

        if( detected.empty() || detected != image["mediaType"].get<std::string>() )
            diagnostic( aResult, "board_image_media_mismatch",
                        "board image media_type does not match its decoded file signature" );

        if( digest != image["sha256"].get<std::string>() )
            diagnostic( aResult, "board_image_digest_mismatch",
                        "board image sha256 does not match its decoded data_base64" );
    }

    return image;
}


JSON compileBarcode( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "invalid_board_barcode_id",
                    "board barcode requires one bounded stable ID" );
        return JSON::object();
    }

    static const std::set<std::string> ALLOWED = {
        "text", "kind", "error_correction", "at", "rotation", "layer", "size",
        "show_text", "text_height", "knockout", "knockout_margin", "locked"
    };
    std::map<std::string, size_t> fields;
    collectFields( aDocument, node, ALLOWED, fields, aResult, "barcode" );
    JSON barcode = { { "kind", "barcode" }, { "logicalId", id }, { "typed", true } };

    for( const char* required : { "text", "kind", "error_correction", "at", "rotation",
                                  "layer", "size", "show_text", "text_height", "knockout",
                                  "knockout_margin", "locked" } )
    {
        if( !fields.contains( required ) )
            diagnostic( aResult, "missing_board_barcode_field",
                        "board barcode " + id + " is missing " + required );
    }

    std::string value;

    if( fields.contains( "text" ) )
    {
        if( !singleValue( aDocument, fields["text"], value ) || value.empty()
            || value.size() > MAX_BARCODE_TEXT_BYTES || value.find( '\0' ) != std::string::npos )
        {
            diagnostic( aResult, "invalid_board_barcode_text",
                        "board barcode text requires 1 through 4096 UTF-8 bytes" );
        }
        else
        {
            barcode["text"] = value;
        }
    }

    static const std::set<std::string> KINDS = {
        "code_39", "code_128", "data_matrix", "qr_code", "micro_qr_code"
    };

    if( fields.contains( "kind" ) )
    {
        if( !singleValue( aDocument, fields["kind"], value ) || !KINDS.contains( value ) )
            diagnostic( aResult, "invalid_board_barcode_kind",
                        "board barcode kind must be code_39|code_128|data_matrix|qr_code|micro_qr_code" );
        else
            barcode["barcodeKind"] = value;
    }

    if( fields.contains( "error_correction" ) )
    {
        if( !singleValue( aDocument, fields["error_correction"], value )
            || ( value != "L" && value != "M" && value != "Q" && value != "H" ) )
        {
            diagnostic( aResult, "invalid_board_barcode_error_correction",
                        "board barcode error_correction must be L|M|Q|H" );
        }
        else
        {
            barcode["errorCorrection"] = value;
        }
    }

    JSON point;

    if( fields.contains( "at" ) && parsePoint( aDocument, fields["at"], point ) )
        barcode["position"] = std::move( point );
    else if( fields.contains( "at" ) )
        diagnostic( aResult, "invalid_board_barcode_at", "board barcode at requires two distances" );

    if( fields.contains( "rotation" ) )
    {
        double angle = 0.0;

        if( !singleValue( aDocument, fields["rotation"], value ) || !parseAngle( value, angle ) )
            diagnostic( aResult, "invalid_board_barcode_rotation", "board barcode rotation requires an angle in degrees" );
        else
            barcode["rotationDegrees"] = angle;
    }

    if( fields.contains( "layer" ) )
    {
        if( !singleValue( aDocument, fields["layer"], value ) || !boardLayer( value ) )
            diagnostic( aResult, "invalid_board_barcode_layer", "board barcode layer is not a KiCad board layer" );
        else
            barcode["layer"] = value;
    }

    if( fields.contains( "size" ) )
    {
        const DOCUMENT::NODE& size = aDocument.Nodes()[fields["size"]];
        std::string widthText;
        std::string heightText;
        int64_t width = 0;
        int64_t height = 0;

        if( size.kind != DOCUMENT::NODE_KIND::LIST || size.children.size() != 3
            || !scalarText( aDocument, size.children[1], widthText )
            || !scalarText( aDocument, size.children[2], heightText )
            || !parseDistance( widthText, width ) || !parseDistance( heightText, height )
            || width < 100'000 || height < 100'000 )
        {
            diagnostic( aResult, "invalid_board_barcode_size",
                        "board barcode size requires width and height from 0.1 mm through 2 m" );
        }
        else
        {
            barcode["widthNm"] = width;
            barcode["heightNm"] = height;
        }
    }

    for( const auto& [field, output] :
         { std::pair<const char*, const char*>{ "show_text", "showText" },
           { "knockout", "knockout" }, { "locked", "locked" } } )
    {
        if( !fields.contains( field ) )
            continue;

        bool boolean = false;

        if( !parseBoolean( aDocument, fields[field], boolean ) )
            diagnostic( aResult, "invalid_board_barcode_" + std::string( field ),
                        "board barcode " + std::string( field ) + " must be true or false" );
        else
            barcode[output] = boolean;
    }

    if( fields.contains( "text_height" ) )
    {
        int64_t height = 0;

        if( !singleValue( aDocument, fields["text_height"], value )
            || !parseDistance( value, height ) || height < 100'000 )
        {
            diagnostic( aResult, "invalid_board_barcode_text_height",
                        "board barcode text_height must be from 0.1 mm through 2 m" );
        }
        else
        {
            barcode["textHeightNm"] = height;
        }
    }

    if( fields.contains( "knockout_margin" )
        && parsePoint( aDocument, fields["knockout_margin"], point )
        && point["xNm"].get<int64_t>() >= 0 && point["yNm"].get<int64_t>() >= 0 )
    {
        barcode["knockoutMargin"] = std::move( point );
    }
    else if( fields.contains( "knockout_margin" ) )
    {
        diagnostic( aResult, "invalid_board_barcode_knockout_margin",
                    "board barcode knockout_margin requires two nonnegative distances" );
    }

    return barcode;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_BOARD_ASSET_COMPILER::IsAssetHead( const std::string& aHead )
{
    return aHead == "image" || aHead == "barcode";
}


DESIGN_SCRIPT_BOARD_ASSET_COMPILER::RESULT DESIGN_SCRIPT_BOARD_ASSET_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const std::string head = aDocument.ListHead( aNode );

    if( head == "image" )
        result.statement = compileImage( aDocument, aNode, result );
    else if( head == "barcode" )
        result.statement = compileBarcode( aDocument, aNode, result );
    else
        diagnostic( result, "invalid_board_asset", "board asset must be image or barcode" );

    result.ok = result.diagnostics.empty() && result.statement.is_object()
                && !result.statement.empty();
    return result;
}

} // namespace KICHAD
