/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "thirdparty/tap.h"
#include "thirdparty/jansson/jansson_private.h"
#include "thirdparty/jansson/strbuffer.h"

static int dump_to_strbuffer(const char *buffer, size_t size, void *data)
{
    return strbuffer_append_bytes((strbuffer_t *)data, buffer, size);
}

static void hexdump(char *start, char *end)
{
  int i;
  int maxbytes = 24;

  while (start < end) {
    int limit = end - start;
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
      printf("%c", isprint(start[i]) ? start[i] : '.');
    }
    printf("\n");
    start += limit;
  }
}

static char *bdumps(json_t *json, char **end)
{
    strbuffer_t strbuff;

    if (strbuffer_init(&strbuff)) {
        return NULL;
    }

    if (w_bser_dump(json, dump_to_strbuffer, &strbuff) == 0) {
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
  int needed;

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

  *expect_p = expected;
  *got_p = decoded;
  return true;
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
      (6 * num_templ)
  );

  for (i = 0; i < num_json_inputs; i++) {
    check_roundtrip(json_inputs[i], NULL, &expected, &decoded);
  }

  for (i = 0; i < num_templ; i++) {
    check_roundtrip(template_tests[i].json_text,
        template_tests[i].template_text,
        &expected, &decoded);
  }

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

