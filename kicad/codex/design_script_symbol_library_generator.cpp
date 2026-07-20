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

#include "design_script_symbol_library_generator.h"

#include "design_script_symbol_field_generator.h"
#include "design_script_symbol_graphics_generator.h"
#include "design_script_symbol_text_generator.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT;
using FIELD_GENERATOR = KICHAD::DESIGN_SCRIPT_SYMBOL_FIELD_GENERATOR;
using GRAPHICS_GENERATOR = KICHAD::DESIGN_SCRIPT_SYMBOL_GRAPHICS_GENERATOR;
using TEXT_GENERATOR = KICHAD::DESIGN_SCRIPT_SYMBOL_TEXT_GENERATOR;

constexpr size_t MAX_NATIVE_LIBRARY_BYTES = 16 * 1024 * 1024;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


std::string quoted( const std::string& aText )
{
    std::string result = "\"";
    result.reserve( aText.size() + 2 );

    for( unsigned char character : aText )
    {
        switch( character )
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result.push_back( static_cast<char>( character ) ); break;
        }
    }

    result.push_back( '"' );
    return result;
}


std::string millimetres( int64_t aNanometers )
{
    if( aNanometers == 0 )
        return "0";

    const bool negative = aNanometers < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aNanometers + 1 ) ) + 1
                                       : static_cast<uint64_t>( aNanometers );
    const uint64_t whole = magnitude / 1'000'000;
    const uint64_t fraction = magnitude % 1'000'000;
    std::string result = negative ? "-" : "";
    result += std::to_string( whole );

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction );
        result += "." + std::string( 6 - digits.size(), '0' ) + digits;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


std::string yesNo( bool aValue )
{
    return aValue ? "yes" : "no";
}


std::string property( const std::string& aName, const std::string& aValue, bool aHidden,
                      int64_t aX = 0, int64_t aY = 0, int aAngle = 0 )
{
    std::string result = "\t\t(property " + quoted( aName ) + " " + quoted( aValue ) + "\n"
                         "\t\t\t(at " + millimetres( aX ) + " " + millimetres( aY ) + " "
                         + std::to_string( aAngle ) + ")\n"
                         "\t\t\t(show_name no)\n"
                         "\t\t\t(do_not_autoplace no)\n";

    if( aHidden )
        result += "\t\t\t(hide yes)\n";

    result += "\t\t\t(effects\n"
              "\t\t\t\t(font\n"
              "\t\t\t\t\t(size 1.27 1.27)\n"
              "\t\t\t\t)\n"
              "\t\t\t)\n"
              "\t\t)\n";
    return result;
}


bool renderField( const JSON& aSymbol, const std::string& aRole,
                  const std::string& aNativeName, const std::string& aValue,
                  bool aHidden, int64_t aX, int64_t aY, std::string& aSource )
{
    if( !aSymbol.contains( "fieldLayouts" ) || !aSymbol["fieldLayouts"].is_object() )
        return false;

    if( aSymbol["fieldLayouts"].contains( aRole ) )
        return FIELD_GENERATOR::Render( aNativeName, aValue,
                                        aSymbol["fieldLayouts"][aRole], aSource );

    aSource += property( aNativeName, aValue, aHidden, aX, aY );
    return true;
}


bool renderCustomField( const JSON& aField, std::string& aSource )
{
    return aField.is_object() && aField.contains( "name" ) && aField["name"].is_string()
           && aField.contains( "value" ) && aField["value"].is_string()
           && aField.contains( "layout" )
           && FIELD_GENERATOR::Render( aField["name"].get<std::string>(),
                                       aField["value"].get<std::string>(),
                                       aField["layout"], aSource );
}


bool validPoint( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" ) && aPoint["xNm"].is_number_integer()
           && aPoint.contains( "yNm" ) && aPoint["yNm"].is_number_integer();
}


bool renderPin( const JSON& aItem, std::string& aSource )
{
    if( !aItem.is_object() || aItem.value( "kind", "" ) != "pin"
        || !aItem.contains( "number" ) || !aItem["number"].is_string()
        || !aItem.contains( "name" ) || !aItem["name"].is_string()
        || !aItem.contains( "electrical" ) || !aItem["electrical"].is_string()
        || !aItem.contains( "shape" ) || !aItem["shape"].is_string()
        || !aItem.contains( "at" ) || !validPoint( aItem["at"] )
        || !aItem.contains( "orientation" ) || !aItem["orientation"].is_string()
        || !aItem.contains( "lengthNm" ) || !aItem["lengthNm"].is_number_integer()
        || !aItem.contains( "hidden" ) || !aItem["hidden"].is_boolean()
        || !aItem.contains( "nameSizeNm" ) || !aItem["nameSizeNm"].is_number_integer()
        || !aItem.contains( "numberSizeNm" ) || !aItem["numberSizeNm"].is_number_integer()
        || !aItem.contains( "alternates" ) || !aItem["alternates"].is_array() )
    {
        return false;
    }

    static const std::map<std::string, int> angles = {
        { "right", 0 }, { "down", 90 }, { "left", 180 }, { "up", 270 }
    };
    const std::string orientation = aItem["orientation"].get<std::string>();

    if( !angles.contains( orientation ) )
        return false;

    aSource += "\t\t\t(pin " + aItem["electrical"].get<std::string>() + " "
               + aItem["shape"].get<std::string>() + "\n"
               "\t\t\t\t(at " + millimetres( aItem["at"]["xNm"].get<int64_t>() ) + " "
               + millimetres( aItem["at"]["yNm"].get<int64_t>() ) + " "
               + std::to_string( angles.at( orientation ) ) + ")\n"
               "\t\t\t\t(length " + millimetres( aItem["lengthNm"].get<int64_t>() ) + ")\n";

    if( aItem["hidden"].get<bool>() )
        aSource += "\t\t\t\t(hide yes)\n";

    aSource += "\t\t\t\t(name " + quoted( aItem["name"].get<std::string>() ) + "\n"
               "\t\t\t\t\t(effects\n"
               "\t\t\t\t\t\t(font\n"
               "\t\t\t\t\t\t\t(size "
               + millimetres( aItem["nameSizeNm"].get<int64_t>() ) + " "
               + millimetres( aItem["nameSizeNm"].get<int64_t>() ) + ")\n"
               "\t\t\t\t\t\t)\n"
               "\t\t\t\t\t)\n"
               "\t\t\t\t)\n"
               "\t\t\t\t(number " + quoted( aItem["number"].get<std::string>() ) + "\n"
               "\t\t\t\t\t(effects\n"
               "\t\t\t\t\t\t(font\n"
               "\t\t\t\t\t\t\t(size "
               + millimetres( aItem["numberSizeNm"].get<int64_t>() ) + " "
               + millimetres( aItem["numberSizeNm"].get<int64_t>() ) + ")\n"
               "\t\t\t\t\t\t)\n"
               "\t\t\t\t\t)\n"
               "\t\t\t\t)\n";

    for( const JSON& alternate : aItem["alternates"] )
    {
        if( !alternate.is_object() || !alternate.contains( "name" )
            || !alternate["name"].is_string() || !alternate.contains( "electrical" )
            || !alternate["electrical"].is_string() || !alternate.contains( "shape" )
            || !alternate["shape"].is_string() )
        {
            return false;
        }

        aSource += "\t\t\t\t(alternate " + quoted( alternate["name"].get<std::string>() )
                   + " " + alternate["electrical"].get<std::string>() + " "
                   + alternate["shape"].get<std::string>() + ")\n";
    }

    aSource += "\t\t\t)\n";
    return true;
}


bool renderSymbol( const JSON& aSymbol, std::string& aSource, RESULT& aResult )
{
    static const std::vector<std::pair<const char*, JSON::value_t>> required = {
        { "name", JSON::value_t::string }, { "reference", JSON::value_t::string },
        { "value", JSON::value_t::string }, { "footprint", JSON::value_t::string },
        { "datasheet", JSON::value_t::string }, { "description", JSON::value_t::string },
        { "keywords", JSON::value_t::string }, { "excludeFromSim", JSON::value_t::boolean },
        { "inBom", JSON::value_t::boolean }, { "onBoard", JSON::value_t::boolean },
        { "inPosFiles", JSON::value_t::boolean }, { "hidePinNames", JSON::value_t::boolean },
        { "hidePinNumbers", JSON::value_t::boolean },
        { "pinNamesOffsetNm", JSON::value_t::number_integer },
        { "properties", JSON::value_t::array }, { "units", JSON::value_t::array },
        { "power", JSON::value_t::string }, { "extends", JSON::value_t::string },
        { "declaredFields", JSON::value_t::array },
        { "duplicatePinNumbersAreJumpers", JSON::value_t::boolean },
        { "jumperGroups", JSON::value_t::array }, { "fieldLayouts", JSON::value_t::object }
    };

    if( !aSymbol.is_object() )
        return false;

    for( const auto& [field, type] : required )
    {
        if( !aSymbol.contains( field ) || aSymbol[field].type() != type )
            return false;
    }

    const std::string name = aSymbol["name"].get<std::string>();
    aSource += "\t(symbol " + quoted( name ) + "\n";
    const std::string parent = aSymbol["extends"].get<std::string>();

    if( !parent.empty() )
    {
        std::set<std::string> declared;

        for( const JSON& field : aSymbol["declaredFields"] )
        {
            if( !field.is_string() )
                return false;

            declared.insert( field.get<std::string>() );
        }

        aSource += "\t\t(extends " + quoted( parent ) + ")\n";

        if( declared.contains( "reference" )
            && !renderField( aSymbol, "reference", "Reference",
                             aSymbol["reference"].get<std::string>(), false,
                             0, -2'540'000, aSource ) )
            return false;

        if( declared.contains( "value" )
            && !renderField( aSymbol, "value", "Value", aSymbol["value"].get<std::string>(),
                             false, 0, 2'540'000, aSource ) )
            return false;

        if( declared.contains( "footprint" )
            && !renderField( aSymbol, "footprint", "Footprint",
                             aSymbol["footprint"].get<std::string>(), true, 0, 0, aSource ) )
            return false;

        if( declared.contains( "datasheet" )
            && !renderField( aSymbol, "datasheet", "Datasheet",
                             aSymbol["datasheet"].get<std::string>(), true, 0, 0, aSource ) )
            return false;

        if( declared.contains( "description" )
            && !renderField( aSymbol, "description", "Description",
                             aSymbol["description"].get<std::string>(), true, 0, 0, aSource ) )
            return false;

        if( declared.contains( "keywords" )
            && !renderField( aSymbol, "keywords", "ki_keywords",
                             aSymbol["keywords"].get<std::string>(), true, 0, 0, aSource ) )
            return false;

        for( const JSON& custom : aSymbol["properties"] )
        {
            if( !renderCustomField( custom, aSource ) )
                return false;
        }

        aSource += "\t\t(embedded_fonts no)\n\t)\n";
        return true;
    }

    const std::string power = aSymbol["power"].get<std::string>();

    if( power == "global" || power == "local" )
        aSource += "\t\t(power " + power + ")\n";
    else if( power != "normal" )
        return false;

    if( aSymbol["hidePinNumbers"].get<bool>() )
        aSource += "\t\t(pin_numbers\n\t\t\t(hide yes)\n\t\t)\n";

    aSource += "\t\t(pin_names\n"
               "\t\t\t(offset " + millimetres( aSymbol["pinNamesOffsetNm"].get<int64_t>() )
               + ")\n";

    if( aSymbol["hidePinNames"].get<bool>() )
        aSource += "\t\t\t(hide yes)\n";

    aSource += "\t\t)\n"
               "\t\t(exclude_from_sim " + yesNo( aSymbol["excludeFromSim"].get<bool>() ) + ")\n"
               "\t\t(in_bom " + yesNo( aSymbol["inBom"].get<bool>() ) + ")\n"
               "\t\t(on_board " + yesNo( aSymbol["onBoard"].get<bool>() ) + ")\n"
               "\t\t(in_pos_files " + yesNo( aSymbol["inPosFiles"].get<bool>() ) + ")\n"
               "\t\t(duplicate_pin_numbers_are_jumpers "
               + yesNo( aSymbol["duplicatePinNumbersAreJumpers"].get<bool>() ) + ")\n";

    if( !aSymbol["jumperGroups"].empty() )
    {
        aSource += "\t\t(jumper_pin_groups\n";

        for( const JSON& group : aSymbol["jumperGroups"] )
        {
            if( !group.is_array() || group.size() < 2 )
                return false;

            aSource += "\t\t\t(";

            for( const JSON& pinNumber : group )
            {
                if( !pinNumber.is_string() )
                    return false;

                aSource += quoted( pinNumber.get<std::string>() ) + " ";
            }

            aSource += ")\n";
        }

        aSource += "\t\t)\n";
    }

    if( !renderField( aSymbol, "reference", "Reference",
                      aSymbol["reference"].get<std::string>(), false,
                      0, -2'540'000, aSource )
        || !renderField( aSymbol, "value", "Value", aSymbol["value"].get<std::string>(),
                         false, 0, 2'540'000, aSource )
        || !renderField( aSymbol, "footprint", "Footprint",
                         aSymbol["footprint"].get<std::string>(), true, 0, 0, aSource )
        || !renderField( aSymbol, "datasheet", "Datasheet",
                         aSymbol["datasheet"].get<std::string>(), true, 0, 0, aSource )
        || !renderField( aSymbol, "description", "Description",
                         aSymbol["description"].get<std::string>(), true, 0, 0, aSource ) )
    {
        return false;
    }

    if( !aSymbol["keywords"].get_ref<const std::string&>().empty()
        && !renderField( aSymbol, "keywords", "ki_keywords",
                         aSymbol["keywords"].get<std::string>(), true, 0, 0, aSource ) )
        return false;

    for( const JSON& custom : aSymbol["properties"] )
    {
        if( !renderCustomField( custom, aSource ) )
            return false;
    }

    std::vector<const JSON*> units;

    for( const JSON& unit : aSymbol["units"] )
        units.push_back( &unit );

    std::stable_sort( units.begin(), units.end(), []( const JSON* aLeft, const JSON* aRight )
    {
        return std::pair( aLeft->value( "number", 0 ), aLeft->value( "bodyStyle", 0 ) )
               < std::pair( aRight->value( "number", 0 ), aRight->value( "bodyStyle", 0 ) );
    } );

    for( const JSON* unit : units )
    {
        if( !unit->is_object() || !unit->contains( "number" )
            || !( *unit )["number"].is_number_integer() || !unit->contains( "bodyStyle" )
            || !( *unit )["bodyStyle"].is_number_integer() || !unit->contains( "items" )
            || !( *unit )["items"].is_array() )
        {
            return false;
        }

        aSource += "\t\t(symbol "
                   + quoted( name + "_" + std::to_string( ( *unit )["number"].get<int64_t>() )
                             + "_" + std::to_string( ( *unit )["bodyStyle"].get<int64_t>() ) )
                   + "\n";

        for( const JSON& item : ( *unit )["items"] )
        {
            const std::string kind = item.value( "kind", "" );
            const bool rendered = kind == "pin" ? renderPin( item, aSource )
                                  : GRAPHICS_GENERATOR::Render( item, aSource )
                                    || TEXT_GENERATOR::Render( item, aSource );

            if( !rendered )
                return false;

            if( kind == "pin" )
                ++aResult.counts["pins"].get_ref<int64_t&>();
            else
                ++aResult.counts["graphics"].get_ref<int64_t&>();
        }

        aSource += "\t\t)\n";
    }

    aSource += "\t\t(embedded_fonts no)\n\t)\n";
    return true;
}


bool validateInheritance( const std::string& aNickname,
                          const std::vector<const JSON*>& aSymbols, RESULT& aResult )
{
    std::map<std::string, std::string> parents;

    for( const JSON* symbol : aSymbols )
    {
        const std::string name = symbol->value( "name", "" );
        const std::string parent = symbol->value( "extends", "" );

        if( name.empty() || !parents.emplace( name, parent ).second )
        {
            diagnostic( aResult, "duplicate_authored_symbol_name",
                        "managed library " + aNickname + " has a duplicate symbol name" );
            return false;
        }
    }

    bool valid = true;

    for( const auto& [name, parent] : parents )
    {
        if( !parent.empty() && !parents.contains( parent ) )
        {
            diagnostic( aResult, "unresolved_authored_symbol_parent",
                        "authored symbol " + aNickname + ":" + name
                                + " extends missing same-library symbol " + parent );
            valid = false;
        }
    }

    std::map<std::string, int> state;
    std::function<bool( const std::string& )> visit = [&]( const std::string& aName )
    {
        if( state[aName] == 1 )
            return false;

        if( state[aName] == 2 )
            return true;

        state[aName] = 1;
        const std::string& parent = parents.at( aName );

        if( !parent.empty() && parents.contains( parent ) && !visit( parent ) )
            return false;

        state[aName] = 2;
        return true;
    };

    for( const auto& entry : parents )
    {
        const std::string& name = entry.first;

        if( !visit( name ) )
        {
            diagnostic( aResult, "recursive_authored_symbol_inheritance",
                        "managed library " + aNickname
                                + " contains a recursive derived-symbol chain at " + name );
            valid = false;
            break;
        }
    }

    return valid;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::RESULT
DESIGN_SCRIPT_SYMBOL_LIBRARY_GENERATOR::Generate( const JSON& aCompilerIr )
{
    RESULT result;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "libraries" )
        || !aCompilerIr["libraries"].is_array() || !aCompilerIr.contains( "authoredSymbols" )
        || !aCompilerIr["authoredSymbols"].is_array() )
    {
        diagnostic( result, "invalid_authored_symbol_ir",
                    "compiled KDS symbol-library IR is malformed" );
        return result;
    }

    std::map<std::string, std::vector<const JSON*>> symbolsByLibrary;

    for( const JSON& symbol : aCompilerIr["authoredSymbols"] )
    {
        if( !symbol.is_object() || !symbol.contains( "library" )
            || !symbol["library"].is_string() )
        {
            diagnostic( result, "invalid_authored_symbol_ir",
                        "compiled KDS contains a malformed authored symbol" );
            continue;
        }

        symbolsByLibrary[symbol["library"].get<std::string>()].push_back( &symbol );
    }

    std::set<std::string> generatedLibraries;

    for( const JSON& library : aCompilerIr["libraries"] )
    {
        if( !library.is_object() || library.value( "kind", "" ) != "symbol"
            || library.value( "table", "" ) != "project"
            || !library.value( "managed", false ) )
        {
            continue;
        }

        const std::string nickname = library.value( "id", "" );

        if( nickname.empty() || !generatedLibraries.emplace( nickname ).second
            || !symbolsByLibrary.contains( nickname ) || symbolsByLibrary[nickname].empty() )
        {
            diagnostic( result, "invalid_managed_symbol_library",
                        "each managed project symbol library requires one unique declaration "
                        "and at least one authored symbol" );
            continue;
        }

        std::vector<const JSON*>& symbols = symbolsByLibrary[nickname];
        std::sort( symbols.begin(), symbols.end(), []( const JSON* aLeft, const JSON* aRight )
        {
            return aLeft->value( "name", "" ) < aRight->value( "name", "" );
        } );

        if( !validateInheritance( nickname, symbols, result ) )
            continue;

        std::string source = "(kicad_symbol_lib\n"
                             "\t(version 20251024)\n"
                             "\t(generator \"kichad_kds\")\n"
                             "\t(generator_version \"10.0\")\n";

        for( const JSON* symbol : symbols )
        {
            if( !renderSymbol( *symbol, source, result ) )
            {
                diagnostic( result, "invalid_authored_symbol_ir",
                            "authored symbol backend rejected "
                                    + symbol->value( "id", "unknown" ) );
                break;
            }

            ++result.counts["symbols"].get_ref<int64_t&>();
        }

        source += ")\n";

        if( source.size() > MAX_NATIVE_LIBRARY_BYTES )
        {
            diagnostic( result, "managed_symbol_library_too_large",
                        "generated symbol library " + nickname + " exceeds 16 MiB" );
            continue;
        }

        std::string parseError;
        std::unique_ptr<DOCUMENT> parsed = DOCUMENT::Parse( source, &parseError );

        if( !parsed || parsed->Roots().size() != 1
            || parsed->ListHead( parsed->Roots().front() ) != "kicad_symbol_lib" )
        {
            diagnostic( result, "invalid_generated_symbol_library",
                        "generated symbol library " + nickname + " is not structural Kicad data: "
                                + parseError );
            continue;
        }

        result.sources[nickname] = std::move( source );
        ++result.counts["libraries"].get_ref<int64_t&>();
    }

    for( const auto& [nickname, symbols] : symbolsByLibrary )
    {
        if( !generatedLibraries.contains( nickname ) )
        {
            diagnostic( result, "unresolved_managed_symbol_library",
                        "authored symbols target undeclared managed project library " + nickname );
        }
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
