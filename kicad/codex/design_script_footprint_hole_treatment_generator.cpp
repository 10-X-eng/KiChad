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

#include "design_script_footprint_hole_treatment_generator.h"

#include <cstdint>
#include <string>


namespace
{

using JSON = nlohmann::json;


std::string millimetres( int64_t aNanometers )
{
    const uint64_t magnitude = static_cast<uint64_t>( aNanometers );
    const uint64_t fraction = magnitude % 1'000'000;
    std::string result = std::to_string( magnitude / 1'000'000 );

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction );
        result += "." + std::string( 6 - digits.size(), '0' ) + digits;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


std::string degrees( int64_t aTenths )
{
    std::string result = std::to_string( aTenths / 10 );

    if( aTenths % 10 != 0 )
        result += "." + std::to_string( aTenths % 10 );

    return result;
}


bool renderMachining( const JSON& aMachining, const char* aNativeSide, std::string& aSource )
{
    if( !aMachining.is_object() || !aMachining.contains( "mode" )
        || !aMachining["mode"].is_string()
        || !aMachining.contains( "side" ) || !aMachining["side"].is_string()
        || aMachining["side"] != aNativeSide
        || ( aMachining["mode"] != "counterbore" && aMachining["mode"] != "countersink" )
        || !aMachining.contains( "diameterNm" )
        || !aMachining["diameterNm"].is_number_integer()
        || aMachining["diameterNm"].get<int64_t>() <= 0 )
    {
        return false;
    }

    const std::string mode = aMachining["mode"].get<std::string>();
    aSource += "\t\t(" + std::string( aNativeSide ) + "_post_machining " + mode
               + " (size " + millimetres( aMachining["diameterNm"].get<int64_t>() ) + ")";

    if( aMachining.contains( "depthNm" ) && aMachining["depthNm"].is_number_integer() )
    {
        if( aMachining["depthNm"].get<int64_t>() <= 0 )
            return false;

        aSource += " (depth " + millimetres( aMachining["depthNm"].get<int64_t>() ) + ")";
    }
    else if( !aMachining.contains( "depthNm" ) || !aMachining["depthNm"].is_null()
             || mode == "counterbore" )
    {
        return false;
    }

    if( aMachining.contains( "angleTenths" )
        && aMachining["angleTenths"].is_number_integer() )
    {
        if( mode != "countersink" || aMachining["angleTenths"].get<int64_t>() <= 0 )
            return false;

        aSource += " (angle " + degrees( aMachining["angleTenths"].get<int64_t>() ) + ")";
    }
    else if( !aMachining.contains( "angleTenths" ) || !aMachining["angleTenths"].is_null()
             || mode == "countersink" )
    {
        return false;
    }

    aSource += ")\n";
    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_HOLE_TREATMENT_GENERATOR::Render(
        const JSON& aTreatment, std::string& aSource )
{
    if( !aTreatment.is_object() || !aTreatment.contains( "tenting" )
        || !aTreatment.contains( "frontPostMachining" )
        || !aTreatment.contains( "backPostMachining" ) )
    {
        return false;
    }

    bool emitted = false;

    if( aTreatment["tenting"].is_object() )
    {
        const std::string front = aTreatment["tenting"].value( "front", "" );
        const std::string back = aTreatment["tenting"].value( "back", "" );
        const auto native = []( const std::string& aValue ) -> const char*
        {
            if( aValue == "inherit" )
                return "none";

            return aValue == "open" ? "yes" : aValue == "tented" ? "no" : nullptr;
        };
        const char* frontNative = native( front );
        const char* backNative = native( back );

        if( !frontNative || !backNative )
            return false;

        aSource += "\t\t(tenting (front " + std::string( frontNative ) + ") (back "
                   + backNative + "))\n";
        emitted = true;
    }
    else if( !aTreatment["tenting"].is_null() )
    {
        return false;
    }

    if( aTreatment["frontPostMachining"].is_object() )
    {
        if( !renderMachining( aTreatment["frontPostMachining"], "front", aSource ) )
            return false;

        emitted = true;
    }
    else if( !aTreatment["frontPostMachining"].is_null() )
    {
        return false;
    }

    if( aTreatment["backPostMachining"].is_object() )
    {
        if( !renderMachining( aTreatment["backPostMachining"], "back", aSource ) )
            return false;

        emitted = true;
    }
    else if( !aTreatment["backPostMachining"].is_null() )
    {
        return false;
    }

    return emitted;
}

} // namespace KICHAD
