/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/*
 * This defines a binary serialization of the JSON data objects in this
 * library.  It is designed for use with watchman and is not intended to serve
 * as a general binary JSON interchange format.  In particular, all integers
 * are signed integers and are stored in host byte order to minimize
 * transformation overhead.
 */

/* Return the smallest size int that can store the value */
#define INT_SIZE(x) (((x) == ((int8_t)x))  ? 1 :    \
                     ((x) == ((int16_t)x)) ? 2 :    \
                     ((x) == ((int32_t)x)) ? 4 : 8)

#define BSER_ARRAY     0x00
#define BSER_OBJECT    0x01
#define BSER_BYTESTRING 0x02
#define BSER_INT8      0x03
#define BSER_INT16     0x04
#define BSER_INT32     0x05
#define BSER_INT64     0x06
#define BSER_REAL      0x07
#define BSER_TRUE      0x08
#define BSER_FALSE     0x09
#define BSER_NULL      0x0a
#define BSER_TEMPLATE  0x0b
#define BSER_SKIP      0x0c
#define BSER_UTF8STRING 0x0d

// BSER capabilities. Must be powers of 2.
#define BSER_CAP_DISABLE_UNICODE 0x1
#define BSER_CAP_DISABLE_UNICODE_FOR_ERRORS 0x2

static const char bser_true = BSER_TRUE;
static const char bser_false = BSER_FALSE;
static const char bser_null = BSER_NULL;
static const char bser_bytestring_hdr = BSER_BYTESTRING;
static const char bser_array_hdr = BSER_ARRAY;
static const char bser_object_hdr = BSER_OBJECT;
static const char bser_template_hdr = BSER_TEMPLATE;
static const char bser_skip = BSER_SKIP;
static const char bser_utf8string_hdr = BSER_UTF8STRING;

static bool is_bser_version_supported(const bser_ctx_t *ctx) {
  return ctx->bser_version == 1 || ctx->bser_version == 2;
}

static int bser_real(const bser_ctx_t *ctx, double val, void *data)
{
  char sz = BSER_REAL;
  if (!is_bser_version_supported(ctx)) {
    return -1;
  }

  if (ctx->dump(&sz, sizeof(sz), data)) {
    return -1;
  }
  return ctx->dump((char*)&val, sizeof(val), data);
}

bool bunser_generic_string(const char *buf, json_int_t avail,
    json_int_t *needed, const char **start, json_int_t *len)
{
  json_int_t ineed;

  if (!bunser_int(buf + 1, avail - 1, &ineed, len)) {
    *needed = ineed;
    return false;
  }

  buf += ineed + 1;
  avail -= ineed + 1;
  *needed = ineed + 1 + *len;

  if (*len > avail) {
    return false;
  }

  *start = buf;
  return true;
}

// Attempt to unserialize an integer value.
// Returns bool if successful, and populates *val with the value.
// Otherwise populates *needed with the size required to successfully
// decode the integer value
bool bunser_int(const char *buf, json_int_t avail,
    json_int_t *needed, json_int_t *val)
{
  int8_t i8;
  int16_t i16;
  int32_t i32;
  int64_t i64;

  switch (buf[0]) {
    case BSER_INT8:
      *needed = 2;
      break;
    case BSER_INT16:
      *needed = 3;
      break;
    case BSER_INT32:
      *needed = 5;
      break;
    case BSER_INT64:
      *needed = 9;
      break;
    default:
      *needed = -1;
      return false;
  }
  if (avail < *needed) {
    return false;
  }

  switch (buf[0]) {
    case BSER_INT8:
      memcpy(&i8, buf + 1, sizeof(i8));
      *val = i8;
      return true;
    case BSER_INT16:
      memcpy(&i16, buf + 1, sizeof(i16));
      *val = i16;
      return true;
    case BSER_INT32:
      memcpy(&i32, buf + 1, sizeof(i32));
      *val = i32;
      return true;
    case BSER_INT64:
      memcpy(&i64, buf + 1, sizeof(i64));
      *val = i64;
      return true;
    default:
      return false;
  }
}

static int bser_int(const bser_ctx_t *ctx, json_int_t val, void *data)
{
  int8_t i8;
  int16_t i16;
  int32_t i32;
  int64_t i64;
  char sz;
  int size = INT_SIZE(val);
  char *iptr;

  if (!is_bser_version_supported(ctx)) {
    return -1;
  }

  switch (size) {
    case 1:
      sz = BSER_INT8;
      i8 = (int8_t)val;
      iptr = (char*)&i8;
      break;
    case 2:
      sz = BSER_INT16;
      i16 = (int16_t)val;
      iptr = (char*)&i16;
      break;
    case 4:
      sz = BSER_INT32;
      i32 = (int32_t)val;
      iptr = (char*)&i32;
      break;
    case 8:
      sz = BSER_INT64;
      i64 = (int64_t)val;
      iptr = (char*)&i64;
      break;
    default:
      return -1;
  }

  if (ctx->dump(&sz, sizeof(sz), data)) {
    return -1;
  }

  return ctx->dump(iptr, size, data);
}

static int bser_generic_string(const bser_ctx_t *ctx, const char *str,
    void *data, const char hdr)
{
  size_t len = strlen(str);

  if (!is_bser_version_supported(ctx)) {
    return -1;
  }

  if (ctx->dump(&hdr, sizeof(hdr), data)) {
    return -1;
  }

  if (bser_int(ctx, len, data)) {
    return -1;
  }

  if (ctx->dump(str, len, data)) {
    return -1;
  }

  return 0;
}

static int bser_bytestring(const bser_ctx_t *ctx, const char *str, void *data)
{
  return bser_generic_string(ctx, str, data, bser_bytestring_hdr);
}

static int bser_utf8string(const bser_ctx_t *ctx, const char *str, void *data)
{
  if ((ctx->bser_capabilities & BSER_CAP_DISABLE_UNICODE) ||
      ctx->bser_version == 1) {
    return bser_bytestring(ctx, str, data);
  }
    return bser_generic_string(ctx, str, data, bser_utf8string_hdr);
}

static int bser_mixedstring(const bser_ctx_t *ctx, const char *str, void *data)
{
  int res, length;
  char *cpy;
  if (!(BSER_CAP_DISABLE_UNICODE_FOR_ERRORS & ctx->bser_capabilities) &&
      !(BSER_CAP_DISABLE_UNICODE & ctx->bser_capabilities)) {
    length = strlen(str);
    cpy = malloc(length + 1);
    memcpy(cpy, str, length + 1);
    utf8_fix_string(cpy, length);
    res = bser_utf8string(ctx, str, data);
    free(cpy);
  } else {
    res = bser_bytestring(ctx, str, data);
  }
  return res;
}

static int bser_array(const bser_ctx_t *ctx, const json_t *array, void *data);

static int bser_template(const bser_ctx_t *ctx, const json_t *array,
    const json_t *templ, void *data)
{
  size_t n = json_array_size(array);
  size_t i, pn;

  if (!is_bser_version_supported(ctx)) {
    return -1;
  }

  if (ctx->dump(&bser_template_hdr, sizeof(bser_template_hdr), data)) {
    return -1;
  }

  // The template goes next
  if (bser_array(ctx, templ, data)) {
    return -1;
  }

  // Now the array of arrays of object values.
  // How many objects
  if (bser_int(ctx, n, data)) {
    return -1;
  }

  pn = json_array_size(templ);

  // For each object
  for (i = 0; i < n; i++) {
    json_t *obj = json_array_get(array, i);
    size_t pi;

    // For each factored key
    for (pi = 0; pi < pn; pi++) {
      const char *key = json_string_value(json_array_get(templ, pi));
      json_t *val;

      // Look up the object property
      val = json_object_get(obj, key);
      if (!val) {
        // property not set on this one; emit a skip
        if (ctx->dump(&bser_skip, sizeof(bser_skip), data)) {
          return -1;
        }
        continue;
      }

      // Emit value
      if (w_bser_dump(ctx, val, data)) {
        return -1;
      }
    }
  }

  return 0;
}

static int bser_array(const bser_ctx_t *ctx, const json_t *array, void *data)
{
  size_t n = json_array_size(array);
  size_t i;
  json_t *templ;

  if (!is_bser_version_supported(ctx)) {
    return -1;
  }

  templ = json_array_get_template(array);
  if (templ) {
    return bser_template(ctx, array, templ, data);
  }

  if (ctx->dump(&bser_array_hdr, sizeof(bser_array_hdr), data)) {
    return -1;
  }

  if (bser_int(ctx, n, data)) {
    return -1;
  }

  for (i = 0; i < n; i++) {
    json_t *val = json_array_get(array, i);

    if (w_bser_dump(ctx, val, data)) {
      return -1;
    }
  }

  return 0;
}

static int bser_object(const bser_ctx_t *ctx, json_t *obj, void *data)
{
  size_t n;
  json_t *val;
  const char *key;
  void *iter;

  if (!is_bser_version_supported(ctx)) {
    return -1;
  }

  if (ctx->dump(&bser_object_hdr, sizeof(bser_object_hdr), data)) {
    return -1;
  }

  n = json_object_size(obj);
  if (bser_int(ctx, n, data)) {
    return -1;
  }

  iter = json_object_iter(obj);
  while (iter) {
    key = json_object_iter_key(iter);
    val = json_object_iter_value(iter);

    if (bser_bytestring(ctx, key, data)) {
      return -1;
    }
    if (w_bser_dump(ctx, val, data)) {
      return -1;
    }

    iter = json_object_iter_next(obj, iter);
  }

  return 0;
}

int w_bser_dump(const bser_ctx_t* ctx, json_t *json, void *data)
{
  int type = json_typeof(json);
  w_string_t* wstr;
  if (!is_bser_version_supported(ctx)) {
    return -1;
  }

  switch (type) {
    case JSON_NULL:
      return ctx->dump(&bser_null, sizeof(bser_null), data);
    case JSON_TRUE:
      return ctx->dump(&bser_true, sizeof(bser_true), data);
    case JSON_FALSE:
      return ctx->dump(&bser_false, sizeof(bser_false), data);
    case JSON_REAL:
      return bser_real(ctx, json_real_value(json), data);
    case JSON_INTEGER:
      return bser_int(ctx, json_integer_value(json), data);
    case JSON_STRING:
      wstr = json_to_w_string(json);
      switch (wstr->type) {
        case W_STRING_BYTE:
          return bser_bytestring(ctx, json_string_value(json), data);
        case W_STRING_UNICODE:
          return bser_utf8string(ctx, json_string_value(json), data);
        case W_STRING_MIXED:
          return bser_mixedstring(ctx, json_string_value(json), data);

      }
    case JSON_ARRAY:
      return bser_array(ctx, json, data);
    case JSON_OBJECT:
      return bser_object(ctx, json, data);
    default:
      return -1;
  }
}

static int measure(const char *buffer, size_t size, void *ptr)
{
  json_int_t *tot = ptr;
  *tot += size;
  unused_parameter(buffer);
  return 0;
}

int w_bser_write_pdu(const uint32_t bser_version,
    const uint32_t bser_capabilities, json_dump_callback_t dump, json_t *json,
    void *data)
{
  json_int_t m_size = 0;
  bser_ctx_t ctx = { .bser_version = bser_version,
    .bser_capabilities = bser_capabilities, .dump = measure };

  if (!is_bser_version_supported(&ctx)) {
    return -1;
  }

  if (w_bser_dump(&ctx, json, &m_size)) {
    return -1;
  }

  // To actually write the contents
  ctx.dump = dump;

  if (bser_version == 2) {
    if (dump(BSER_V2_MAGIC, 2, data)) {
      return -1;
    }
  } else {
    if (dump(BSER_MAGIC, 2, data)) {
      return -1;
    }
  }

  if (bser_version == 2) {
    if (dump((const char*) &bser_capabilities, 4, data)) {
      return -1;
    }
  }

  if (bser_int(&ctx, m_size, data)) {
    return -1;
  }

  if (w_bser_dump(&ctx, json, data)) {
    return -1;
  }

  return 0;
}

static json_t *bunser_array(const char *buf, const char *end,
    json_int_t *used, json_error_t *jerr)
{
  json_int_t needed;
  json_int_t total = 0;
  json_int_t i, nelems;
  json_t *arrval;

  buf++;
  total++;

  if (!bunser_int(buf, end - buf, &needed, &nelems)) {
    if (needed == -1) {
      snprintf(jerr->text, sizeof(jerr->text),
          "invalid integer encoding 0x%02x for array length. buf=%p\n",
          (int)buf[0], buf);
      return NULL;
    }
    *used = needed + total;
    snprintf(jerr->text, sizeof(jerr->text),
        "invalid array length encoding 0x%02x (needed %d but have %d)",
        (int)buf[0], (int)needed, (int)(end - buf));
    return NULL;
  }

  total += needed;
  buf += needed;

  arrval = json_array();
  for (i = 0; i < nelems; i++) {
    json_t *item;

    needed = 0;
    item = bunser(buf, end, &needed, jerr);

    total += needed;
    buf += needed;

    if (!item) {
      json_decref(arrval);
      *used = total;
      return NULL;
    }

    if (json_array_append_new(arrval, item)) {
      json_decref(arrval);
      *used = total;
      snprintf(jerr->text, sizeof(jerr->text),
        "failed to append array item");
      return NULL;
    }
  }

  *used = total;
  return arrval;
}

static json_t *bunser_template(const char *buf, const char *end,
    json_int_t *used, json_error_t *jerr)
{
  json_int_t needed = 0;
  json_int_t total = 0;
  json_int_t i, nelems;
  json_int_t ip, np;
  json_t *templ = NULL, *arrval, *ret = NULL;

  buf++;
  total++;

  if (*buf != BSER_ARRAY) {
    snprintf(jerr->text, sizeof(jerr->text),
        "Expected array encoding, but found 0x%02x", *buf);
    *used = total;
    return NULL;
  }

  // Load in the property names template
  templ = bunser_array(buf, end, &needed, jerr);
  if (!templ) {
    *used = needed + total;
    goto bail;
  }
  total += needed;
  buf += needed;

  // And the number of objects
  needed = 0;
  if (!bunser_int(buf, end - buf, &needed, &nelems)) {
    *used = needed + total;
    snprintf(jerr->text, sizeof(jerr->text),
        "invalid object number encoding (needed %d but have %d)",
        (int)needed, (int)(end - buf));
    goto bail;
  }
  total += needed;
  buf += needed;

  np = json_array_size(templ);

  // Now load up the array with object values
  arrval = json_array_of_size((size_t)nelems);
  for (i = 0; i < nelems; i++) {
    json_t *item, *val;

    item = json_object_of_size((size_t)np);
    for (ip = 0; ip < np; ip++) {
      if (*buf == BSER_SKIP) {
        buf++;
        total++;
        continue;
      }

      needed = 0;
      val = bunser(buf, end, &needed, jerr);
      if (!val) {
        *used = needed + total;
        goto bail;
      }
      buf += needed;
      total += needed;

      json_object_set_new_nocheck(item,
          json_string_value(json_array_get(templ, (size_t)ip)),
          val);
    }

    json_array_append_new(arrval, item);
  }

  *used = total;
  ret = arrval;
 bail:
  json_decref(templ);
  return ret;
}

static json_t *bunser_object(const char *buf, const char *end,
    json_int_t *used, json_error_t *jerr)
{
  json_int_t needed;
  json_int_t total = 0;
  json_int_t i, nelems;
  json_t *objval;
  char keybuf[128];

  total = 1;
  buf++;

  if (!bunser_int(buf, end - buf, &needed, &nelems)) {
    *used = needed + total;
    snprintf(jerr->text, sizeof(jerr->text),
        "invalid object property count encoding");
    return NULL;
  }

  total += needed;
  buf += needed;

  objval = json_object();
  for (i = 0; i < nelems; i++) {
    const char *start;
    json_int_t slen;
    json_t *item;

    // Read key
    if (!bunser_generic_string(buf, end - buf, &needed, &start, &slen)) {
      *used = total + needed;
      json_decref(objval);
      snprintf(jerr->text, sizeof(jerr->text),
          "invalid bytestring for object key");
      return NULL;
    }
    total += needed;
    buf += needed;

    // Saves us allocating a string when the library is going to
    // do that anyway
    if ((uint16_t)slen > sizeof(keybuf) - 1) {
      json_decref(objval);
      snprintf(jerr->text, sizeof(jerr->text),
          "object key is too long");
      return NULL;
    }
    memcpy(keybuf, start, (size_t)slen);
    keybuf[slen] = '\0';

    // Read value
    item = bunser(buf, end, &needed, jerr);
    total += needed;
    buf += needed;

    if (!item) {
      json_decref(objval);
      *used = total;
      return NULL;
    }

    if (json_object_set_new_nocheck(objval, keybuf, item)) {
      json_decref(item);
      json_decref(objval);
      *used = total;
      snprintf(jerr->text, sizeof(jerr->text),
          "failed to add object property");
      return NULL;
    }
  }

  *used = total;
  return objval;
}

json_t *bunser(const char *buf, const char *end, json_int_t *needed,
    json_error_t *jerr)
{
  json_int_t ival;

  switch (buf[0]) {
    case BSER_INT8:
    case BSER_INT16:
    case BSER_INT32:
    case BSER_INT64:
      if (!bunser_int(buf, end - buf, needed, &ival)) {
        snprintf(jerr->text, sizeof(jerr->text),
            "invalid integer encoding");
        return NULL;
      }
      return json_integer(ival);

    case BSER_BYTESTRING:
    case BSER_UTF8STRING:
    {
      const char *start;
      json_int_t len;

      if (!bunser_generic_string(buf, end - buf, needed, &start, &len)) {
        snprintf(jerr->text, sizeof(jerr->text),
            "invalid bytestring encoding");
        return NULL;
      }

      return typed_string_len_to_json(start, len,
          buf[0] == BSER_BYTESTRING? W_STRING_BYTE : W_STRING_UNICODE);
    }

    case BSER_REAL:
    {
      double dval;
      *needed = sizeof(double) + 1;
      memcpy(&dval, buf + 1, sizeof(dval));
      return json_real(dval);
    }

    case BSER_TRUE:
      *needed = 1;
      return json_true();
    case BSER_FALSE:
      *needed = 1;
      return json_false();
    case BSER_NULL:
      *needed = 1;
      return json_null();
    case BSER_ARRAY:
      return bunser_array(buf, end, needed, jerr);
    case BSER_TEMPLATE:
      return bunser_template(buf, end, needed, jerr);
    case BSER_OBJECT:
      return bunser_object(buf, end, needed, jerr);
    default:
      snprintf(jerr->text, sizeof(jerr->text),
            "invalid bser encoding type %02x", (int)buf[0]);
      return NULL;
  }

#ifndef _WIN32 // It knows this is unreachable
  return NULL;
#endif
}

/* vim:ts=2:sw=2:et:
 */
