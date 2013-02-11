/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static json_t *make_name(struct watchman_rule_match *match)
{
  return json_string_nocheck(match->relname->buf);
}

static json_t *make_exists(struct watchman_rule_match *match)
{
  return json_boolean(match->file->exists);
}

static json_t *make_new(struct watchman_rule_match *match)
{
  if (match->is_new) {
    return json_true();
  }
  return NULL;
}

#define MAKE_CLOCK_FIELD(name, member) \
  static json_t *make_##name(struct watchman_rule_match *match) { \
    char buf[128]; \
    if (clock_id_string(match->file->member.ticks, buf, sizeof(buf))) { \
      return json_string_nocheck(buf); \
    } \
    return NULL; \
  }
MAKE_CLOCK_FIELD(cclock, ctime)
MAKE_CLOCK_FIELD(oclock, otime)

// Note: our JSON library supports 64-bit integers, but this may
// pose a compatibility issue for others.  We'll see if anyone
// runs into an issue and deal with it then...
#define MAKE_INT_FIELD(name, member) \
  static json_t *make_##name(struct watchman_rule_match *match) { \
    return json_integer(match->file->st.member); \
  }

MAKE_INT_FIELD(size, st_size)
MAKE_INT_FIELD(mode, st_mode)
MAKE_INT_FIELD(uid, st_uid)
MAKE_INT_FIELD(gid, st_gid)
MAKE_INT_FIELD(atime, st_atime)
MAKE_INT_FIELD(mtime, st_mtime)
MAKE_INT_FIELD(ctime, st_ctime)
MAKE_INT_FIELD(ino, st_ino)
MAKE_INT_FIELD(dev, st_dev)
MAKE_INT_FIELD(nlink, st_nlink)

static struct w_query_field_renderer {
  const char *name;
  json_t *(*make)(struct watchman_rule_match *match);
} field_defs[] = {
  { "name", make_name },
  { "exists", make_exists },
  { "size", make_size },
  { "mode", make_mode },
  { "uid", make_uid },
  { "gid", make_gid },
  { "atime", make_atime },
  { "mtime", make_mtime },
  { "ctime", make_ctime },
  { "ino", make_ino },
  { "dev", make_dev },
  { "nlink", make_nlink },
  { "new", make_new },
  { "oclock", make_oclock },
  { "cclock", make_cclock },
  { NULL, NULL }
};

json_t *w_query_results_to_json(
    struct w_query_field_list *field_list,
    uint32_t num_results,
    struct watchman_rule_match *results)
{
  json_t *file_list = json_array();
  uint32_t i, f;

  for (i = 0; i < num_results; i++) {
    json_t *value, *ele;

    if (field_list->num_fields == 1) {
      value = field_list->fields[0]->make(&results[i]);
    } else {
      value = json_object();

      for (f = 0; f < field_list->num_fields; f++) {
        ele = field_list->fields[f]->make(&results[i]);
        set_prop(value, field_list->fields[f]->name, ele);
      }
    }
    json_array_append_new(file_list, value);
  }
  return file_list;
}


bool parse_field_list(json_t *field_list,
    struct w_query_field_list *selected,
    char **errmsg)
{
  uint32_t i, f;

  memset(selected, 0, sizeof(*selected));

  if (field_list == NULL) {
    // Use the default list
    field_list = json_pack("[sssss]", "name", "exists", "new", "size", "mode");
  } else {
    // Add a ref so that we don't need complicated logic to deal with
    // whether we defaulted or not; just unconditionally delref on return
    json_incref(field_list);
  }

  if (!json_is_array(field_list)) {
    *errmsg = strdup("field list must be an array of strings");
    json_decref(field_list);
    return false;
  }

  for (i = 0; i < json_array_size(field_list); i++) {
    json_t *jname = json_array_get(field_list, i);
    const char *name;
    bool found = false;

    if (!json_is_string(jname)) {
      *errmsg = strdup("field list must be an array of strings");
      json_decref(field_list);
      return false;
    }

    name = json_string_value(jname);

    for (f = 0; field_defs[f].name; f++) {
      if (!strcmp(name, field_defs[f].name)) {
        found = true;
        selected->fields[selected->num_fields++] = &field_defs[f];
        break;
      }
    }

    if (!found) {
      asprintf(errmsg, "unknown field name '%s'", name);
      json_decref(field_list);
      return false;
    }
  }

  json_decref(field_list);
  return true;
}


/* vim:ts=2:sw=2:et:
 */

