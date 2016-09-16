/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "thirdparty/tap.h"

extern "C" {
#include "thirdparty/jansson/jansson_private.h"
#include "thirdparty/jansson/strbuffer.h"
}

static int dump_to_strbuffer(const char *buffer, size_t size, void *data)
{
    return strbuffer_append_bytes((strbuffer_t *)data, buffer, size);
}

static void hexdump(char *start, char *end)
{
  int i;
  int maxbytes = 24;

  while (start < end) {
    ptrdiff_t limit = end - start;
    if (limit > maxbytes) {
      limit = maxbytes;
    }
    printf("# ");
    for (i = 0; i < limit; i++) {
      printf("%02x", (int)(uint8_t)start[i]);
    }
    while (i <= maxbytes) {
      printf("  ");
      i++;
    }
    printf("   ");
    for (i = 0; i < limit; i++) {
      printf("%c", isprint((uint8_t)start[i]) ? start[i] : '.');
    }
    printf("\n");
    start += limit;
  }
}

static char *bdumps(json_t *json, char **end)
{
    strbuffer_t strbuff;
    bser_ctx_t ctx{1, 0, dump_to_strbuffer};

    if (strbuffer_init(&strbuff)) {
        return NULL;
    }

    if (w_bser_dump(&ctx, json, &strbuff) == 0) {
      *end = strbuff.value + strbuff.length;
      return strbuff.value;
    }

    strbuffer_close(&strbuff);
    return NULL;
}

static char *bdumps_pdu(json_t *json, char **end)
{
    strbuffer_t strbuff;

    if (strbuffer_init(&strbuff)) {
        return NULL;
    }

    if (w_bser_write_pdu(1, 0, dump_to_strbuffer, json, &strbuff) == 0) {
      *end = strbuff.value + strbuff.length;
      return strbuff.value;
    }

    strbuffer_close(&strbuff);
    return NULL;
}

static const char *json_inputs[] = {
  "{\"foo\": 42, \"bar\": true}",
  "[1, 2, 3]",
  "[null, true, false, 65536]",
  "[1.5, 2.0]",
  "[{\"lemon\": 2.5}, null, 16000, true, false]",
  "[1, 16000, 65536, 90000, 2147483648, 4294967295]",
};

static struct {
  const char *json_text;
  const char *template_text;
} template_tests[] = {
  {
    "["
      "{\"name\": \"fred\", \"age\": 20}, "
      "{\"name\": \"pete\", \"age\": 30}, "
      "{\"age\": 25}"
    "]",
    "[\"name\", \"age\"]"
  }
};

static bool check_roundtrip(const char *input, const char *template_text,
    json_t **expect_p, json_t **got_p)
{
  char *dump_buf, *end;
  char *jdump;
  json_t *expected, *decoded, *templ = NULL;
  json_error_t jerr;
  json_int_t needed;

  expected = json_loads(input, 0, &jerr);
  ok(expected != NULL, "loaded %s: %s", input, jerr.text);
  if (!expected) {
      return false;
  }
  if (template_text) {
    templ = json_loads(template_text, 0, &jerr);
    json_array_set_template(expected, templ);
  }

  dump_buf = bdumps(expected, &end);
  ok(dump_buf != NULL, "dumped something");
  if (!dump_buf) {
    json_decref(templ);
    return false;;
  }
  hexdump(dump_buf, end);

  memset(&jerr, 0, sizeof(jerr));
  decoded = bunser(dump_buf, end, &needed, &jerr);
  ok(decoded != NULL, "decoded something (err = %s)", jerr.text);

  jdump = json_dumps(decoded, 0);
  ok(jdump != NULL, "dumped %s", jdump);

  ok(json_equal(expected, decoded), "round-tripped json_equal");
  ok(!strcmp(jdump, input), "round-tripped strcmp");

  free(jdump);
  free(dump_buf);
  json_decref(templ);
  *expect_p = expected;
  *got_p = decoded;
  return true;
}

static void check_serialization(const char* json_in, const char* bser_out) {
  char *bser_in;
  char *end = NULL;
  json_error_t jerr;
  json_t *input;
  unsigned int length;
  input = json_loads(json_in, 0, &jerr);
  bser_in = bdumps_pdu(input, &end);
  length = (unsigned int)(end - bser_in);
  ok(memcmp(bser_in, bser_out, length) == 0, "raw bser comparison %s",
      json_in);
  free(bser_in);
  json_decref(input);
}

int main(int argc, char **argv)
{
  int i, num_json_inputs, num_templ;
  json_t *expected, *decoded;
  (void)argc;
  (void)argv;

  num_json_inputs = sizeof(json_inputs)/sizeof(json_inputs[0]);
  num_templ = sizeof(template_tests)/sizeof(template_tests[0]);

  plan_tests(
      (6 * num_json_inputs) +
      (6 * num_templ) +
      4 // raw tests
  );

  for (i = 0; i < num_json_inputs; i++) {
    expected = NULL;
    decoded = NULL;
    check_roundtrip(json_inputs[i], NULL, &expected, &decoded);
    json_decref(expected);
    json_decref(decoded);
  }

  for (i = 0; i < num_templ; i++) {
    expected = NULL;
    decoded = NULL;
    check_roundtrip(template_tests[i].json_text,
        template_tests[i].template_text,
        &expected, &decoded);
    json_decref(expected);
    json_decref(decoded);

  }
  check_serialization("{\"name\": \"Tom\", \"age\": 24}",
      "\x00\x01\x03\x18\x01\x03\x02\x02\x03\x04\x6e\x61\x6d\x65\x02\x03\x03"
      "\x54\x6f\x6d\x02\x03\x03\x61\x67\x65\x03\x18");
  check_serialization("{\"names\": [\"Tom\", \"Jerry\"], \"age\": 24}",
      "\x00\x01\x03\x24\x01\x03\x02\x02\x03\x05\x6e\x61\x6d\x65\x73\x00\x03"
      "\x02\x02\x03\x03\x54\x6f\x6d\x02\x03\x05\x4a\x65\x72\x72\x79\x02\x03"
      "\x03\x61\x67\x65\x03\x18");
  check_serialization("[\"Tom\", \"Jerry\"]",
      "\x00\x01\x03\x11\x00\x03\x02\x02\x03\x03\x54\x6f\x6d\x02\x03\x05\x4a"
      "\x65\x72\x72\x79");
  check_serialization("[1, 123, 12345, 1234567, 12345678912345678]",
      "\x00\x01\x03\x18\x00\x03\x05\x03\x01\x03\x7b\x04\x39\x30\x05\x87\xd6"
      "\x12\x00\x06\x4e\xd6\x14\x5e\x54\xdc\x2b\x00");
  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */
