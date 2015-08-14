/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool eval_type(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  intptr_t arg = (intptr_t)data;

  unused_parameter(ctx);

  switch (arg) {
    case 'b':
      return S_ISBLK(file->st.st_mode);
    case 'c':
      return S_ISCHR(file->st.st_mode);
    case 'd':
      return S_ISDIR(file->st.st_mode);
    case 'f':
      return S_ISREG(file->st.st_mode);
    case 'p':
      return S_ISFIFO(file->st.st_mode);
    case 'l':
      return S_ISLNK(file->st.st_mode);
    case 's':
      return S_ISSOCK(file->st.st_mode);
#ifdef S_ISDOOR
    case 'D':
      return S_ISDOOR(file->st.st_mode);
#endif
    default:
      return false;
  }
}

static void dispose_type(void *data)
{
  unused_parameter(data);
}

static w_query_expr *type_parser(w_query *query, json_t *term)
{
  const char *ignore, *typestr, *found;
  intptr_t arg;

  if (json_unpack(term, "[s,s]", &ignore, &typestr) != 0) {
    query->errmsg = strdup("must use [\"type\", \"typestr\"]");
    return NULL;
  }

  found = strpbrk(typestr, "bcdfplsD");
  if (!found || strlen(typestr) > 1) {
    ignore_result(asprintf(&query->errmsg, "invalid type string '%s'",
        typestr));
    return NULL;
  }

  arg = *found;

  return w_query_expr_new(eval_type, dispose_type, (void*)arg);
}
W_TERM_PARSER("type", type_parser)

/* vim:ts=2:sw=2:et:
 */
