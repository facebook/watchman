/*
 * Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <assert.h>
#include <errno.h>
#include <fmt/compile.h>
#include <stdio.h>
#include <string.h>
#include "jansson_private.h"

#if JSON_HAVE_LOCALECONV
#include <locale.h>

/*
  - This code assumes that the decimal separator is exactly one
    character.

  - If setlocale() is called by another thread between the call to
    localeconv() and the call to sprintf() or strtod(), the result may
    be wrong. setlocale() is not thread-safe and should not be used
    this way. Multi-threaded programs should use uselocale() instead.
*/

static void to_locale(std::string& strbuffer) {
  const char* point = localeconv()->decimal_point;

  if (*point == '.') {
    /* No conversion needed */
    return;
  }

  auto pos = strbuffer.find('.');
  if (pos != std::string::npos) {
    strbuffer.replace(pos, 1, point);
  }
}
#endif

int jsonp_strtod(std::string& strbuffer, double* out) {
  double value;
  char* end;

#if JSON_HAVE_LOCALECONV
  to_locale(strbuffer);
#endif

  errno = 0;
  value = strtod(strbuffer.c_str(), &end);
  assert(end == strbuffer.c_str() + strbuffer.size());

  if (errno == ERANGE && value != 0) {
    /* Overflow */
    return -1;
  }

  *out = value;
  return 0;
}

int jsonp_dtostr(char* buffer, size_t size, double value) {
  auto result = fmt::format_to_n(buffer, size, FMT_COMPILE("{:.17g}"), value);
  if (result.size >= size) {
    return -1;
  }
  // If `value` is integral, buffer may not contain a . or e, so add one. This
  // avoids parsing a double as an integer, even though the JSON spec does not
  // differentiate between types of numbers.
  if (nullptr == memchr(buffer, '.', result.size) && nullptr == memchr(buffer, 'e', result.size)) {
    if (result.size + 2 >= size) {
      return -1;
    }
    buffer[result.size] = '.';
    buffer[result.size + 1] = '0';
    return result.size + 2;
  }
  return result.size;
}
