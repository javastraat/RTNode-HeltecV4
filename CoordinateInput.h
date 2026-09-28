// CoordinateInput.h — parse a latitude/longitude typed into the config portal.
//
// Kept free of Arduino types so tests/native can compile it on the host.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef COORDINATE_INPUT_H
#define COORDINATE_INPUT_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Parses signed decimal degrees such as "-80.1918". A ',' decimal separator
// is accepted too, because phone keypads in comma-decimal locales type one.
// Surrounding whitespace is allowed; any other trailing text is not.
// Returns false, leaving `out` untouched, when the text is empty, is not a
// number, or lies outside [min_deg, max_deg].
inline bool parse_coordinate_text(const char* text, double min_deg, double max_deg, double& out) {
    char buf[32];
    size_t len = strlen(text);
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, text, len + 1);
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == ',') buf[i] = '.';
    }

    char* end = nullptr;
    double value = strtod(buf, &end);
    if (end == buf) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return false;
    if (isnan(value) || value < min_deg || value > max_deg) return false;

    out = value;
    return true;
}

#endif
