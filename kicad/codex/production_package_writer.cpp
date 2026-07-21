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

#include "production_package_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>

#include <picosha2.h>
#include <wx/base64.h>


namespace
{

using JSON = nlohmann::json;

constexpr uintmax_t MAX_FIRMWARE_FILE_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr uintmax_t MAX_FIRMWARE_TOTAL_BYTES = 256ULL * 1024ULL * 1024ULL;


std::string extensionForFormat( const std::string& aFormat )
{
    static const std::map<std::string, std::string> EXTENSIONS = {
        { "binary", ".bin" }, { "ihex", ".hex" }, { "elf", ".elf" },
        { "uf2", ".uf2" }, { "srec", ".srec" }
    };
    auto extension = EXTENSIONS.find( aFormat );
    return extension == EXTENSIONS.end() ? std::string() : extension->second;
}


bool hashFile( const std::filesystem::path& aPath, std::string& aDigest )
{
    std::ifstream input( aPath, std::ios::binary );

    if( !input )
        return false;

    std::array<picosha2::byte_t, picosha2::k_digest_size> digest{};
    picosha2::hash256( input, digest.begin(), digest.end() );
    aDigest = picosha2::bytes_to_hex_string( digest );
    return !input.bad() && aDigest.size() == 64;
}


bool confinedRegularFile( const std::filesystem::path& aRoot,
                          const std::filesystem::path& aRelative,
                          std::filesystem::path& aResolved, std::string& aError )
{
    if( aRelative.empty() || aRelative.is_absolute()
        || aRelative.lexically_normal() != aRelative )
    {
        aError = "production firmware path is not a normalized project-relative path";
        return false;
    }

    for( const std::filesystem::path& part : aRelative )
    {
        if( part.empty() || part == "." || part == ".." )
        {
            aError = "production firmware path escapes the project";
            return false;
        }
    }

    std::error_code filesystemError;
    const std::filesystem::path canonicalRoot = std::filesystem::canonical( aRoot,
                                                                            filesystemError );

    if( filesystemError )
    {
        aError = "production input root could not be resolved";
        return false;
    }

    const std::filesystem::path candidate = canonicalRoot / aRelative;
    const std::filesystem::file_status lexicalStatus =
            std::filesystem::symlink_status( candidate, filesystemError );

    if( filesystemError || std::filesystem::is_symlink( lexicalStatus )
        || !std::filesystem::is_regular_file( lexicalStatus ) )
    {
        aError = "production firmware input must be a confined regular file, not a symlink";
        return false;
    }

    const std::filesystem::path canonical = std::filesystem::canonical( candidate,
                                                                        filesystemError );
    const std::filesystem::path relative = canonical.lexically_relative( canonicalRoot );

    if( filesystemError || relative.empty() || relative.is_absolute()
        || relative.begin() == relative.end() || *relative.begin() == ".."
        || canonical != candidate.lexically_normal() )
    {
        aError = "production firmware input resolves outside the project";
        return false;
    }

    aResolved = canonical;
    return true;
}


bool writeJson( const std::filesystem::path& aPath, const JSON& aDocument,
                std::string& aError )
{
    std::ofstream output( aPath, std::ios::binary | std::ios::trunc );

    if( !output )
    {
        aError = "could not create the production plan";
        return false;
    }

    output << aDocument.dump( 2 ) << '\n';
    output.flush();

    if( !output )
    {
        aError = "could not write the complete production plan";
        return false;
    }

    output.close();
    return static_cast<bool>( output );
}


int hexDigit( char aCharacter )
{
    if( aCharacter >= '0' && aCharacter <= '9' )
        return aCharacter - '0';

    if( aCharacter >= 'A' && aCharacter <= 'F' )
        return aCharacter - 'A' + 10;

    if( aCharacter >= 'a' && aCharacter <= 'f' )
        return aCharacter - 'a' + 10;

    return -1;
}


bool hexByte( std::string_view aText, size_t aOffset, uint8_t& aValue )
{
    if( aOffset > aText.size() || aText.size() - aOffset < 2 )
        return false;

    const int high = hexDigit( aText[aOffset] );
    const int low = hexDigit( aText[aOffset + 1] );

    if( high < 0 || low < 0 )
        return false;

    aValue = static_cast<uint8_t>( ( high << 4 ) | low );
    return true;
}


bool validateIntelHex( const std::filesystem::path& aPath )
{
    std::ifstream input( aPath, std::ios::binary );
    std::string line;
    bool dataRecord = false;
    bool endRecord = false;

    while( std::getline( input, line ) )
    {
        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.empty() || endRecord || line.front() != ':' || line.size() < 11
            || ( line.size() - 1 ) % 2 != 0 )
        {
            return false;
        }

        uint8_t byteCount = 0;
        uint8_t addressHigh = 0;
        uint8_t addressLow = 0;
        uint8_t recordType = 0;

        if( !hexByte( line, 1, byteCount ) || !hexByte( line, 3, addressHigh )
            || !hexByte( line, 5, addressLow ) || !hexByte( line, 7, recordType )
            || line.size() != 11 + static_cast<size_t>( byteCount ) * 2 )
        {
            return false;
        }

        unsigned int checksum = 0;

        for( size_t offset = 1; offset < line.size(); offset += 2 )
        {
            uint8_t value = 0;

            if( !hexByte( line, offset, value ) )
                return false;

            checksum += value;
        }

        if( ( checksum & 0xFFU ) != 0 )
            return false;

        const uint16_t address = static_cast<uint16_t>( addressHigh ) << 8 | addressLow;

        switch( recordType )
        {
        case 0x00:
            if( byteCount == 0 )
                return false;

            dataRecord = true;
            break;

        case 0x01:
            if( byteCount != 0 || address != 0 )
                return false;

            endRecord = true;
            break;

        case 0x02:
        case 0x04:
            if( byteCount != 2 || address != 0 )
                return false;
            break;

        case 0x03:
        case 0x05:
            if( byteCount != 4 || address != 0 )
                return false;
            break;

        default:
            return false;
        }
    }

    return input.eof() && !input.bad() && dataRecord && endRecord;
}


bool validateSRecord( const std::filesystem::path& aPath )
{
    std::ifstream input( aPath, std::ios::binary );
    std::string line;
    bool dataRecord = false;
    bool terminationRecord = false;

    while( std::getline( input, line ) )
    {
        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.size() < 10 || line[0] != 'S' || line[1] < '0' || line[1] > '9'
            || terminationRecord )
        {
            return false;
        }

        const int type = line[1] - '0';
        const std::map<int, size_t> addressBytes = {
            { 0, 2 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 5, 2 },
            { 6, 3 }, { 7, 4 }, { 8, 3 }, { 9, 2 }
        };
        const auto addressLength = addressBytes.find( type );
        uint8_t byteCount = 0;

        if( addressLength == addressBytes.end() || !hexByte( line, 2, byteCount )
            || byteCount < addressLength->second + 1
            || line.size() != 4 + static_cast<size_t>( byteCount ) * 2 )
        {
            return false;
        }

        unsigned int checksum = byteCount;

        for( size_t offset = 4; offset < line.size(); offset += 2 )
        {
            uint8_t value = 0;

            if( !hexByte( line, offset, value ) )
                return false;

            checksum += value;
        }

        if( ( checksum & 0xFFU ) != 0xFFU )
            return false;

        if( type >= 1 && type <= 3 )
        {
            if( byteCount == addressLength->second + 1 )
                return false;

            dataRecord = true;
        }
        else if( type >= 7 && type <= 9 )
        {
            if( byteCount != addressLength->second + 1 )
                return false;

            terminationRecord = true;
        }
    }

    return input.eof() && !input.bad() && dataRecord && terminationRecord;
}


uint32_t littleEndian32( const unsigned char* aBytes )
{
    return static_cast<uint32_t>( aBytes[0] )
           | static_cast<uint32_t>( aBytes[1] ) << 8
           | static_cast<uint32_t>( aBytes[2] ) << 16
           | static_cast<uint32_t>( aBytes[3] ) << 24;
}


bool validateUf2( const std::filesystem::path& aPath, uintmax_t aLength )
{
    if( aLength == 0 || aLength % 512 != 0
        || aLength / 512 > std::numeric_limits<uint32_t>::max() )
    {
        return false;
    }

    std::ifstream input( aPath, std::ios::binary );
    std::array<unsigned char, 512> block{};
    std::set<uint32_t> blockNumbers;
    const uint32_t expectedBlocks = static_cast<uint32_t>( aLength / block.size() );

    while( input.read( reinterpret_cast<char*>( block.data() ), block.size() ) )
    {
        if( littleEndian32( block.data() ) != 0x0A324655U
            || littleEndian32( block.data() + 4 ) != 0x9E5D5157U
            || littleEndian32( block.data() + 508 ) != 0x0AB16F30U )
        {
            return false;
        }

        const uint32_t payloadBytes = littleEndian32( block.data() + 16 );
        const uint32_t blockNumber = littleEndian32( block.data() + 20 );
        const uint32_t blockCount = littleEndian32( block.data() + 24 );

        if( payloadBytes == 0 || payloadBytes > 476 || payloadBytes % 4 != 0
            || blockCount != expectedBlocks || blockNumber >= blockCount
            || !blockNumbers.emplace( blockNumber ).second )
        {
            return false;
        }
    }

    return input.eof() && !input.bad() && blockNumbers.size() == expectedBlocks;
}


uint16_t elf16( const unsigned char* aBytes, bool aLittleEndian )
{
    return aLittleEndian
                   ? static_cast<uint16_t>( aBytes[0] | static_cast<uint16_t>( aBytes[1] ) << 8 )
                   : static_cast<uint16_t>( static_cast<uint16_t>( aBytes[0] ) << 8 | aBytes[1] );
}


uint64_t elfInteger( const unsigned char* aBytes, size_t aLength, bool aLittleEndian )
{
    uint64_t value = 0;

    for( size_t index = 0; index < aLength; ++index )
    {
        const size_t source = aLittleEndian ? aLength - index - 1 : index;
        value = value << 8 | aBytes[source];
    }

    return value;
}


bool validateElf( const std::filesystem::path& aPath, uintmax_t aLength )
{
    std::ifstream input( aPath, std::ios::binary );
    std::array<unsigned char, 64> header{};

    if( aLength < 52 || !input.read( reinterpret_cast<char*>( header.data() ), 52 ) )
        return false;

    if( header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F'
        || ( header[4] != 1 && header[4] != 2 ) || ( header[5] != 1 && header[5] != 2 )
        || header[6] != 1 )
    {
        return false;
    }

    const bool is64 = header[4] == 2;
    const bool littleEndian = header[5] == 1;
    const size_t headerBytes = is64 ? 64 : 52;

    if( is64 )
    {
        input.read( reinterpret_cast<char*>( header.data() + 52 ), 12 );

        if( input.gcount() != 12 )
            return false;
    }

    const uint16_t type = elf16( header.data() + 16, littleEndian );
    const uint32_t version = static_cast<uint32_t>( elfInteger( header.data() + 20, 4,
                                                                littleEndian ) );
    const uint64_t programOffset = elfInteger( header.data() + ( is64 ? 32 : 28 ),
                                                is64 ? 8 : 4, littleEndian );
    const uint16_t encodedHeaderBytes = elf16( header.data() + ( is64 ? 52 : 40 ),
                                               littleEndian );
    const uint16_t programEntryBytes = elf16( header.data() + ( is64 ? 54 : 42 ),
                                              littleEndian );
    const uint16_t programEntries = elf16( header.data() + ( is64 ? 56 : 44 ),
                                           littleEndian );
    const uint64_t requiredProgramBytes = is64 ? 56 : 32;

    return ( type == 2 || type == 3 ) && version == 1
           && encodedHeaderBytes == headerBytes && programOffset >= headerBytes
           && programEntryBytes >= requiredProgramBytes && programEntries > 0
           && programOffset <= aLength
           && static_cast<uint64_t>( programEntryBytes ) * programEntries
                      <= aLength - programOffset;
}


bool firmwareFormatMatches( const std::filesystem::path& aPath,
                            const std::string& aFormat )
{
    if( aFormat == "binary" )
        return true;

    std::ifstream input( aPath, std::ios::binary );

    if( !input )
        return false;

    input.seekg( 0, std::ios::end );
    const std::streamoff length = input.tellg();

    if( length <= 0 )
        return false;

    if( aFormat == "ihex" )
        return validateIntelHex( aPath );

    if( aFormat == "elf" )
        return validateElf( aPath, static_cast<uintmax_t>( length ) );

    if( aFormat == "uf2" )
        return validateUf2( aPath, static_cast<uintmax_t>( length ) );

    if( aFormat == "srec" )
        return validateSRecord( aPath );

    return false;
}

} // namespace


namespace KICHAD
{

PRODUCTION_PACKAGE_WRITER::JSON PRODUCTION_PACKAGE_WRITER::BuildPlan(
        const JSON& aProduction )
{
    if( !aProduction.is_object() )
        return nullptr;

    JSON plan = {
        { "schema", "kichad.production-plan.v2" },
        { "planVersion", 2 },
        { "assembly", aProduction.value( "assembly", JSON( nullptr ) ) },
        { "firmware", JSON::array() },
        { "programming", aProduction.value( "programming", JSON::array() ) },
        { "power", aProduction.value( "power", JSON::array() ) },
        { "tests", aProduction.value( "tests", JSON::array() ) }
    };

    if( !aProduction.contains( "firmware" ) || !aProduction["firmware"].is_array() )
        return nullptr;

    for( const JSON& firmware : aProduction["firmware"] )
    {
        JSON entry = firmware;
        const bool embedded = entry.contains( "dataBase64" );
        entry.erase( "dataBase64" );

        if( entry.contains( "deviceCode" ) && entry["deviceCode"].is_object()
            && entry["deviceCode"].contains( "files" )
            && entry["deviceCode"]["files"].is_array() )
        {
            for( JSON& file : entry["deviceCode"]["files"] )
            {
                file.erase( "dataBase64" );
                file["packagePath"] = "production/source/" + firmware.value( "id", "" )
                                      + "/" + file.value( "path", "" );
            }
        }

        entry["sourceKind"] = embedded ? "embedded" : "project_file";
        entry["packagePath"] = "production/firmware/" + firmware.value( "id", "" )
                               + extensionForFormat( firmware.value( "format", "" ) );
        plan["firmware"].push_back( std::move( entry ) );
    }

    return plan;
}


bool PRODUCTION_PACKAGE_WRITER::Write( const std::filesystem::path& aVerificationRoot,
                                       const std::filesystem::path& aStaging,
                                       const JSON& aProduction, const JSON& aPlan,
                                       JSON& aArtifacts,
                                       std::string& aError )
{
    aArtifacts = JSON::array();

    if( aPlan.is_null() )
        return true;

    if( !aProduction.is_object() || !aPlan.is_object()
        || aPlan.value( "schema", "" ) != "kichad.production-plan.v2"
        || aPlan.value( "planVersion", 0 ) != 2 || !aPlan.contains( "firmware" )
        || !aPlan["firmware"].is_array() )
    {
        aError = "compiled production plan is malformed";
        return false;
    }

    const std::filesystem::path productionDirectory = aStaging / "production";
    const std::filesystem::path firmwareDirectory = productionDirectory / "firmware";
    std::error_code filesystemError;
    std::filesystem::create_directories( firmwareDirectory, filesystemError );

    if( filesystemError )
    {
        aError = "could not create private production package directories";
        return false;
    }

    const auto cleanup = [&]()
    {
        std::error_code cleanupError;
        std::filesystem::remove_all( productionDirectory, cleanupError );
    };
    std::map<std::string, const JSON*> firmwareSources;

    if( !aProduction.contains( "firmware" ) || !aProduction["firmware"].is_array() )
    {
        cleanup();
        aError = "compiled KDS production firmware inventory is malformed";
        return false;
    }

    for( const JSON& firmware : aProduction["firmware"] )
    {
        if( firmware.is_object() && !firmware.value( "id", "" ).empty() )
            firmwareSources[firmware["id"].get<std::string>()] = &firmware;
    }

    uintmax_t totalBytes = 0;

    for( const JSON& firmware : aPlan["firmware"] )
    {
        if( !firmware.is_object() || !firmware.contains( "sourceKind" )
            || !firmware["sourceKind"].is_string() || !firmware.contains( "packagePath" )
            || !firmware["packagePath"].is_string() || !firmware.contains( "sha256" )
            || !firmware["sha256"].is_string() || !firmware.contains( "bytes" )
            || !firmware["bytes"].is_number_unsigned() )
        {
            cleanup();
            aError = "compiled firmware artifact entry is malformed";
            return false;
        }

        const std::filesystem::path relativeDestination(
                firmware["packagePath"].get<std::string>() );
        std::filesystem::path source;
        const std::string id = firmware.value( "id", "" );
        const auto sourceEntry = firmwareSources.find( id );

        if( sourceEntry == firmwareSources.end() )
        {
            cleanup();
            aError = "production plan firmware has no matching compiled KDS source";
            return false;
        }

        const bool embedded = firmware["sourceKind"].get<std::string>() == "embedded";
        const bool projectFile = firmware["sourceKind"].get<std::string>() == "project_file";

        if( !embedded && !projectFile )
        {
            cleanup();
            aError = "compiled firmware source kind is unsupported";
            return false;
        }

        const JSON& authored = *sourceEntry->second;
        std::filesystem::path temporaryEmbedded;

        if( embedded )
        {
            if( !authored.contains( "dataBase64" ) || !authored["dataBase64"].is_string() )
            {
                cleanup();
                aError = "embedded firmware source is missing from compiled KDS";
                return false;
            }

            const std::string encoded = authored["dataBase64"].get<std::string>();
            size_t errorPosition = 0;
            wxMemoryBuffer decoded = wxBase64Decode( encoded.data(), encoded.size(),
                                                      wxBase64DecodeMode_Strict,
                                                      &errorPosition );
            temporaryEmbedded = firmwareDirectory / ( "." + id + ".embedded" );
            std::ofstream output( temporaryEmbedded, std::ios::binary | std::ios::trunc );

            if( decoded.GetDataLen() == 0 || !output
                || decoded.GetDataLen() != firmware["bytes"].get<uintmax_t>() )
            {
                cleanup();
                aError = "embedded firmware could not be decoded exactly";
                return false;
            }

            output.write( static_cast<const char*>( decoded.GetData() ),
                          static_cast<std::streamsize>( decoded.GetDataLen() ) );
            output.close();

            if( !output )
            {
                cleanup();
                aError = "embedded firmware could not be written completely";
                return false;
            }

            source = temporaryEmbedded;
        }
        else
        {
            if( !authored.contains( "path" ) || !authored["path"].is_string()
                || !confinedRegularFile(
                        aVerificationRoot,
                        std::filesystem::path( authored["path"].get<std::string>() ),
                        source, aError ) )
            {
                cleanup();
                return false;
            }
        }

        const uintmax_t expectedBytes = firmware["bytes"].get<uintmax_t>();
        const uintmax_t bytes = std::filesystem::file_size( source, filesystemError );

        if( filesystemError || bytes == 0 || bytes > MAX_FIRMWARE_FILE_BYTES
            || bytes != expectedBytes || totalBytes > MAX_FIRMWARE_TOTAL_BYTES - bytes )
        {
            cleanup();
            aError = "firmware byte count differs from KDS or exceeds package limits";
            return false;
        }

        std::string sourceDigest;

        if( !hashFile( source, sourceDigest )
            || sourceDigest != firmware["sha256"].get<std::string>() )
        {
            cleanup();
            aError = "firmware SHA-256 differs from the exact KDS release identity";
            return false;
        }

        if( !firmwareFormatMatches( source, firmware.value( "format", "" ) ) )
        {
            cleanup();
            aError = "firmware bytes do not match the declared firmware format";
            return false;
        }

        if( relativeDestination.empty() || relativeDestination.is_absolute()
            || relativeDestination.parent_path() != std::filesystem::path( "production/firmware" ) )
        {
            cleanup();
            aError = "compiled firmware package path is unsafe";
            return false;
        }

        const std::filesystem::path destination = aStaging / relativeDestination;
        filesystemError.clear();
        std::filesystem::copy_file( source, destination,
                                    std::filesystem::copy_options::none, filesystemError );

        if( filesystemError )
        {
            cleanup();
            aError = "could not copy exact firmware into the production package";
            return false;
        }

        if( !temporaryEmbedded.empty() )
        {
            std::error_code removeError;
            std::filesystem::remove( temporaryEmbedded, removeError );

            if( removeError )
            {
                cleanup();
                aError = "could not remove private embedded-firmware staging input";
                return false;
            }
        }

        std::string installedDigest;

        if( !hashFile( destination, installedDigest ) || installedDigest != sourceDigest )
        {
            cleanup();
            aError = "installed firmware failed digest verification";
            return false;
        }

        totalBytes += bytes;
        aArtifacts.push_back( { { "kind", "firmware" },
                                { "id", firmware.value( "id", "" ) },
                                { "path", relativeDestination.generic_string() },
                                { "bytes", bytes },
                                { "sha256", installedDigest } } );

        if( firmware.contains( "deviceCode" ) )
        {
            if( !authored.contains( "deviceCode" ) || !authored["deviceCode"].is_object()
                || !authored["deviceCode"].contains( "files" )
                || !authored["deviceCode"]["files"].is_array()
                || !firmware["deviceCode"].is_object()
                || !firmware["deviceCode"].contains( "files" )
                || !firmware["deviceCode"]["files"].is_array() )
            {
                cleanup();
                aError = "compiled device-code bundle is malformed";
                return false;
            }

            std::map<std::string, const JSON*> authoredFiles;

            for( const JSON& file : authored["deviceCode"]["files"] )
            {
                if( file.is_object() && !file.value( "path", "" ).empty() )
                    authoredFiles[file["path"].get<std::string>()] = &file;
            }

            const std::filesystem::path sourceRoot =
                    std::filesystem::path( "production/source" ) / id;

            for( const JSON& file : firmware["deviceCode"]["files"] )
            {
                const std::string path = file.value( "path", "" );
                const auto authoredFile = authoredFiles.find( path );

                if( path.empty() || authoredFile == authoredFiles.end()
                    || !file.contains( "packagePath" ) || !file["packagePath"].is_string()
                    || !file.contains( "bytes" ) || !file["bytes"].is_number_unsigned()
                    || !file.contains( "sha256" ) || !file["sha256"].is_string()
                    || !authoredFile->second->contains( "dataBase64" )
                    || !authoredFile->second->at( "dataBase64" ).is_string() )
                {
                    cleanup();
                    aError = "compiled device-code file entry is malformed";
                    return false;
                }

                const std::filesystem::path sourceRelativeDestination(
                        file["packagePath"].get<std::string>() );
                const std::filesystem::path expectedDestination =
                        sourceRoot / std::filesystem::path( path );

                if( sourceRelativeDestination.empty() || sourceRelativeDestination.is_absolute()
                    || sourceRelativeDestination.lexically_normal() != sourceRelativeDestination
                    || sourceRelativeDestination != expectedDestination.lexically_normal() )
                {
                    cleanup();
                    aError = "compiled device-code package path is unsafe";
                    return false;
                }

                const std::string encoded =
                        authoredFile->second->at( "dataBase64" ).get<std::string>();
                size_t errorPosition = 0;
                wxMemoryBuffer decoded = wxBase64Decode( encoded.data(), encoded.size(),
                                                          wxBase64DecodeMode_Strict,
                                                          &errorPosition );

                if( decoded.GetDataLen() == 0
                    || decoded.GetDataLen() != file["bytes"].get<uintmax_t>() )
                {
                    cleanup();
                    aError = "device-code file could not be decoded exactly";
                    return false;
                }

                const std::filesystem::path sourceDestination =
                        aStaging / sourceRelativeDestination;
                filesystemError.clear();
                std::filesystem::create_directories( sourceDestination.parent_path(),
                                                      filesystemError );

                if( filesystemError )
                {
                    cleanup();
                    aError = "could not create device-code package directories";
                    return false;
                }

                std::ofstream output( sourceDestination, std::ios::binary | std::ios::trunc );
                output.write( static_cast<const char*>( decoded.GetData() ),
                              static_cast<std::streamsize>( decoded.GetDataLen() ) );
                output.close();

                std::string digest;

                if( !output || !hashFile( sourceDestination, digest )
                    || digest != file["sha256"].get<std::string>() )
                {
                    cleanup();
                    aError = "packaged device-code file failed exact digest validation";
                    return false;
                }

                aArtifacts.push_back( { { "kind", "device_source" },
                                        { "firmware", id },
                                        { "path", sourceRelativeDestination.generic_string() },
                                        { "bytes", decoded.GetDataLen() },
                                        { "sha256", digest } } );
            }
        }
    }

    const std::filesystem::path planPath = productionDirectory / "production-plan.json";

    if( !writeJson( planPath, aPlan, aError ) )
    {
        cleanup();
        return false;
    }

    const uintmax_t planBytes = std::filesystem::file_size( planPath, filesystemError );
    std::string planDigest;

    if( filesystemError || planBytes == 0 || planBytes > 4 * 1024 * 1024
        || !hashFile( planPath, planDigest ) )
    {
        cleanup();
        aError = "production plan failed bounded digest validation";
        return false;
    }

    aArtifacts.push_back( { { "kind", "production_plan" },
                            { "path", "production/production-plan.json" },
                            { "bytes", planBytes },
                            { "sha256", planDigest } } );
    return true;
}

} // namespace KICHAD
