/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Register all the query terms */

void w_query_init_all(void)
{
#define REG_PARSER(name) \
  w_query_register_expression_parser( \
    #name, w_expr_##name##_parser)
  REG_PARSER(true);
  REG_PARSER(false);
  REG_PARSER(allof);
  REG_PARSER(anyof);
  REG_PARSER(not);
  REG_PARSER(type);
  REG_PARSER(suffix);
  REG_PARSER(match);
  REG_PARSER(imatch);
#ifdef HAVE_PCRE_H
  REG_PARSER(pcre);
  REG_PARSER(ipcre);
#endif
  REG_PARSER(name);
  REG_PARSER(iname);
  REG_PARSER(since);
}

/* vim:ts=2:sw=2:et:
 */

