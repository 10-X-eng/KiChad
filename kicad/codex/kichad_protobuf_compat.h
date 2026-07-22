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

#ifndef KICHAD_PROTOBUF_COMPAT_H
#define KICHAD_PROTOBUF_COMPAT_H

#include <string>
#include <utility>

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

namespace KICHAD
{

/**
 * protobuf 22+ replaced google::protobuf::util::Status with absl::Status.
 * Derive the status type from the JSON utility return value so the code
 * compiles against both protobuf generations without naming either type.
 */
using PROTOBUF_STATUS = decltype( google::protobuf::util::MessageToJsonString(
        std::declval<const google::protobuf::Message&>(), std::declval<std::string*>() ) );

namespace DETAIL
{
template <typename T>
auto setAlwaysPrint( T& aOptions, bool aValue, int )
        -> decltype( (void) ( aOptions.always_print_fields_with_no_presence = aValue ) )
{
    aOptions.always_print_fields_with_no_presence = aValue;
}

template <typename T>
auto setAlwaysPrint( T& aOptions, bool aValue, long )
        -> decltype( (void) ( aOptions.always_print_primitive_fields = aValue ) )
{
    aOptions.always_print_primitive_fields = aValue;
}
} // namespace DETAIL

/**
 * protobuf 25+ renamed JsonPrintOptions::always_print_primitive_fields to
 * always_print_fields_with_no_presence.  Set whichever member exists.
 */
inline void SetAlwaysPrintDefaultValuedFields( google::protobuf::util::JsonPrintOptions& aOptions,
                                               bool aValue )
{
    DETAIL::setAlwaysPrint( aOptions, aValue, 0 );
}

} // namespace KICHAD

#endif // KICHAD_PROTOBUF_COMPAT_H
