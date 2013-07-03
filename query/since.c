/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct since_term {
  struct w_clockspec_query spec;
  enum {
    SINCE_OCLOCK,
    SINCE_CCLOCK,
    SINCE_MTIME,
    SINCE_CTIME
  } field;
};


static bool eval_since(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  struct since_term *term = data;
  w_clock_t clock;
  time_t tval = 0;

  unused_parameter(ctx);

  switch (term->field) {
    case SINCE_OCLOCK:
    case SINCE_CCLOCK:
      clock = (term->field == SINCE_OCLOCK) ? file->otime : file->ctime;
      if (term->spec.is_timestamp) {
        return w_timeval_compare(term->spec.tv, clock.tv) > 0;
      }
      return clock.ticks > term->spec.ticks;
    case SINCE_MTIME:
      tval = file->st.st_mtime;
      break;
    case SINCE_CTIME:
      tval = file->st.st_ctime;
      break;
  }

  return tval > term->spec.tv.tv_sec;
}

static void dispose_since(void *data)
{
  free(data);
}

static struct {
  int value;
  const char *label;
} allowed_fields[] = {
  { SINCE_OCLOCK, "oclock" },
  { SINCE_CCLOCK, "cclock" },
  { SINCE_MTIME,  "mtime" },
  { SINCE_CTIME,  "ctime" },
  { 0, NULL }
};

w_query_expr *w_expr_since_parser(w_query *query, json_t *term)
{
  json_t *jval;
  struct w_clockspec_query since;
  struct since_term *sterm;
  int selected_field = SINCE_OCLOCK;
  const char *fieldname = "oclock";

  if (!json_is_array(term)) {
    query->errmsg = strdup("\"since\" term must be an array");
    return NULL;
  }

  if (json_array_size(term) < 2 || json_array_size(term) > 3) {
    query->errmsg = strdup("\"since\" term has invalid number of parameters");
    return NULL;
  }

  jval = json_array_get(term, 1);
  if (!w_parse_clockspec(NULL, jval, &since, false)) {
    query->errmsg = strdup(
        "invalid clockspec for \"since\" term (cursors are not allowed)"
    );
    return NULL;
  }

  jval = json_array_get(term, 2);
  if (jval) {
    int i;
    bool valid = false;

    fieldname = json_string_value(jval);
    if (!fieldname) {
      query->errmsg = strdup("field name for \"since\" term must be a string");
      return NULL;
    }

    for (i = 0; allowed_fields[i].label; i++) {
      if (!strcmp(allowed_fields[i].label, fieldname)) {
        selected_field = allowed_fields[i].value;
        valid = true;
        break;
      }
    }

    if (!valid) {
      ignore_result(asprintf(&query->errmsg,
          "invalid field name \"%s\" for \"since\" term",
          fieldname));
      return NULL;
    }
  }

  switch (selected_field) {
    case SINCE_CTIME:
    case SINCE_MTIME:
      if (!since.is_timestamp) {
        ignore_result(asprintf(&query->errmsg,
            "field \"%s\" requires a timestamp value "
            "for comparison in \"since\" term",
            fieldname));
        return NULL;
      }
      break;
    case SINCE_OCLOCK:
    case SINCE_CCLOCK:
      /* we'll work with clocks or timestamps */
      break;
  }

  sterm = calloc(1, sizeof(*sterm));
  if (!sterm) {
    query->errmsg = strdup("out of memory");
    return NULL;
  }

  sterm->spec = since;
  sterm->field = selected_field;

  return w_query_expr_new(eval_since, dispose_since, sterm);
}


/* vim:ts=2:sw=2:et:
 */

