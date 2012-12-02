/*
 * Copyright (c) 2010-2012 Petri Lehtinen <petri@digip.org>
 * Copyright 2012 Facebook, Inc. (modified to integrate with watchman)
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 *
 *
 * This file specifies a part of the site-specific configuration for
 * Jansson, namely those things that affect the public API in
 * jansson.h.
 *
 * The configure script copies this file to jansson_config.h and
 * replaces @var@ substitutions by values that fit your system. If you
 * cannot run the configure script, you can do the value substitution
 * by hand.
 */

#ifndef JANSSON_CONFIG_H
#define JANSSON_CONFIG_H

#include "config.h"
#define JSON_INLINE inline
#if HAVE_STRTOLL
# define JSON_INTEGER_IS_LONG_LONG 1
#else
# define JSON_INTEGER_IS_LONG_LONG 0
#endif
#if HAVE_LOCALECONV
# define JSON_HAVE_LOCALECONV 1
#else
# define JSON_HAVE_LOCALECONV 0
#endif

#endif
