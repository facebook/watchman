/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/bser.h"
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GTest.h>
#include "watchman/thirdparty/jansson/jansson_private.h"
#include "watchman/watchman.h"

#define UTF8_PILE_OF_POO "\xf0\x9f\x92\xa9"

// Construct a std::string from a literal that may have embedded NUL bytes.
// The -1 compensates for the NUL terminator that is included in sizeof()
#define S(str_literal) std::string(str_literal, sizeof(str_literal) - 1)

static int dump_to_string(const char* buffer, size_t size, void* data) {
  auto str = (std::string*)data;
  str->append(buffer, size);
  return 0;
}

static void hexdump(const char* start, const char* end) {
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

static std::unique_ptr<std::string>
bdumps(uint32_t version, uint32_t capabilities, const json_ref& json) {
  std::string buffer;
  bser_ctx_t ctx{version, capabilities, dump_to_string};

  if (w_bser_dump(&ctx, json, &buffer) == 0) {
    return std::make_unique<std::string>(std::move(buffer));
  }

  return nullptr;
}

static std::unique_ptr<std::string>
bdumps_pdu(uint32_t version, uint32_t capabilities, const json_ref& json) {
  std::string buffer;

  if (w_bser_write_pdu(version, capabilities, dump_to_string, json, &buffer) ==
      0) {
    return std::make_unique<std::string>(std::move(buffer));
  }

  return nullptr;
}

static const char* json_inputs[] = {
    "{\"bar\": true, \"foo\": 42}",
    "[1, 2, 3]",
    "[null, true, false, 65536]",
    "[1.5, 2.0]",
    "[{\"lemon\": 2.5}, null, 16000, true, false]",
    "[1, 16000, 65536, 90000, 2147483648, 4294967295]",
};

static struct {
  const char* json_text;
  const char* template_text;
} template_tests[] = {
    {"["
     "{\"age\": 20, \"name\": \"fred\"}, "
     "{\"age\": 30, \"name\": \"pete\"}, "
     "{\"age\": 25}"
     "]",
     "[\"name\", \"age\"]"}};

static struct {
  const char* json_text;
  std::string bserv1;
  std::string bserv2;
} serialization_tests[] = {
    {
        "[\"Tom\", \"Jerry\"]",
        S("\x00\x01\x03\x11\x00\x03\x02\x02\x03\x03\x54\x6f\x6d\x02\x03\x05\x4a"
          "\x65\x72\x72\x79"),
        S("\x00\x02\x00\x00\x00\x00\x03\x11\x00\x03\x02\x02\x03\x03\x54\x6f\x6d"
          "\x02\x03\x05\x4a\x65\x72\x72\x79"),
    },
    {
        "[1, 123, 12345, 1234567, 12345678912345678]",
        S("\x00\x01\x03\x18\x00\x03\x05\x03\x01\x03\x7b\x04\x39\x30\x05\x87\xd6"
          "\x12\x00\x06\x4e\xd6\x14\x5e\x54\xdc\x2b\x00"),
        S("\x00\x02\x00\x00\x00\x00\x03\x18\x00\x03\x05\x03\x01\x03\x7b\x04\x39"
          "\x30\x05\x87\xd6\x12\x00\x06\x4e\xd6\x14\x5e\x54\xdc\x2b\x00"),
    }};

static void check_roundtrip(
    uint32_t bser_version,
    uint32_t bser_capabilities,
    const char* input,
    const char* template_text) {
  XLOG(ERR) << "testing BSER version " << bser_version << ", capabilities "
            << bser_capabilities;
  json_ref templ;
  json_error_t jerr;
  json_int_t needed;

  auto expected = json_loads(input, 0, &jerr);
  ASSERT_TRUE(expected) << "loaded " << input << " " << jerr.text;
  if (template_text) {
    templ = json_loads(template_text, 0, &jerr);
    json_array_set_template(expected, templ);
  }

  auto dump_buf = bdumps(bser_version, bser_capabilities, expected);
  ASSERT_NE(dump_buf, nullptr) << "dumped something";
  const char* end = dump_buf->data() + dump_buf->size();
  hexdump(dump_buf->data(), end);

  jerr = json_error_t();
  auto decoded = bunser(dump_buf->data(), end, &needed, &jerr);
  EXPECT_TRUE(decoded) << "decoded something err = " << jerr.text;

  auto jdump = json_dumps(decoded, JSON_SORT_KEYS);
  EXPECT_TRUE(json_equal(expected, decoded)) << "round-tripped json_equal";
  EXPECT_EQ(jdump, input) << "round-tripped";
}

static void check_serialization(
    uint32_t bser_version,
    uint32_t bser_capabilities,
    const char* json_in,
    const std::string& bser_out) {
  XLOG(ERR) << "testing BSER version " << bser_version << ", capabilities "
            << bser_capabilities;

  // Test JSON -> BSER serialization.
  json_error_t jerr;
  auto input = json_loads(json_in, 0, &jerr);
  auto bser_in = bdumps_pdu(bser_version, bser_capabilities, input);
  EXPECT_EQ(*bser_in, bser_out) << "raw bser comparison " << json_in;
}

// The strings are left as mixed escaped and unescaped bytes so that it's easy
// to see how it's constructed.
// The breaks in the middle of the string literals here are to prevent "\x05f"
// etc from being treated as a single character.
static const std::string bser_typed_intro = S("\x00\x03\x03");
static const std::string bser_typed_bytestring =
    S("\x02\x03\x05"
      "foo\xd0\xff");

static const std::string bser_typed_utf8string_byte =
    S("\x02\x03\x07"
      "bar" UTF8_PILE_OF_POO);
static const std::string bser_typed_utf8string_utf8 =
    S("\x0d\x03\x07"
      "bar" UTF8_PILE_OF_POO);

static const std::string bser_typed_mixedstring_byte =
    S("\x02\x03\x0e"
      "baz\xb1\xc1\xe0\x90\x40" UTF8_PILE_OF_POO "\xf4\xff");
static const std::string bser_typed_mixedstring_utf8 =
    S("\x0d\x03\x0e"
      "baz?????" UTF8_PILE_OF_POO "??");

// The tuples are (bser version, bser capabilities, expected BSER serialization)
static std::vector<std::tuple<uint32_t, uint32_t, std::string>>
    typed_string_checks = {
        std::make_tuple(
            1,
            0,
            bser_typed_intro + bser_typed_bytestring +
                bser_typed_utf8string_byte + bser_typed_mixedstring_byte),
        std::make_tuple(
            2,
            0,
            bser_typed_intro + bser_typed_bytestring +
                bser_typed_utf8string_utf8 + bser_typed_mixedstring_utf8),
        std::make_tuple(
            2,
            BSER_CAP_DISABLE_UNICODE,
            bser_typed_intro + bser_typed_bytestring +
                bser_typed_utf8string_byte + bser_typed_mixedstring_byte),
        std::make_tuple(
            2,
            BSER_CAP_DISABLE_UNICODE_FOR_ERRORS,
            bser_typed_intro + bser_typed_bytestring +
                bser_typed_utf8string_utf8 + bser_typed_mixedstring_byte),

        std::make_tuple(
            2,
            BSER_CAP_DISABLE_UNICODE | BSER_CAP_DISABLE_UNICODE_FOR_ERRORS,
            bser_typed_intro + bser_typed_bytestring +
                bser_typed_utf8string_byte + bser_typed_mixedstring_byte)};

static void check_bser_typed_strings() {
  auto bytestring = typed_string_to_json("foo\xd0\xff", W_STRING_BYTE);
  auto utf8string =
      typed_string_to_json("bar" UTF8_PILE_OF_POO, W_STRING_UNICODE);
  // This consists of
  // - ASCII (valid)
  // - bare continuation byte 0xB1 (invalid)
  // - overlong encoding of an ASCII byte 0xC1 (invalid)
  // - 3 byte sequence with valid start (0xE0 0x90) but an invalid byte (0x40)
  // - 4 byte sequence (valid)
  // - 4 byte sequence (0xF4) past the end of the string (invalid)
  auto mixedstring = typed_string_to_json(
      "baz\xb1\xc1\xe0\x90\x40" UTF8_PILE_OF_POO "\xf4\xff", W_STRING_MIXED);

  auto str_array = json_array({bytestring, utf8string, mixedstring});

  // check that this gets serialized correctly
  for (const auto& t : typed_string_checks) {
    uint32_t bser_version = std::get<0>(t);
    uint32_t bser_capabilities = std::get<1>(t);
    const std::string& bser_out = std::get<2>(t);
    XLOG(ERR) << "testing BSER version " << bser_version << ", capabilities "
              << bser_capabilities;

    auto bser_buf = bdumps(bser_version, bser_capabilities, str_array);
    EXPECT_EQ(*bser_buf, bser_out);
  }
}

TEST(Bser, bser_tests) {
  int i, num_json_inputs, num_templ;

  num_json_inputs = sizeof(json_inputs) / sizeof(json_inputs[0]);
  num_templ = sizeof(template_tests) / sizeof(template_tests[0]);
  int num_serial = sizeof(serialization_tests) / sizeof(serialization_tests[0]);

  for (i = 0; i < num_json_inputs; i++) {
    check_roundtrip(1, 0, json_inputs[i], nullptr);
    check_roundtrip(2, 0, json_inputs[i], nullptr);
    check_roundtrip(2, BSER_CAP_DISABLE_UNICODE, json_inputs[i], nullptr);
    check_roundtrip(
        2, BSER_CAP_DISABLE_UNICODE_FOR_ERRORS, json_inputs[i], nullptr);
    check_roundtrip(
        2,
        BSER_CAP_DISABLE_UNICODE | BSER_CAP_DISABLE_UNICODE_FOR_ERRORS,
        json_inputs[i],
        nullptr);
  }

  for (i = 0; i < num_templ; i++) {
    check_roundtrip(
        1, 0, template_tests[i].json_text, template_tests[i].template_text);
    check_roundtrip(
        2, 0, template_tests[i].json_text, template_tests[i].template_text);
    check_roundtrip(
        2,
        BSER_CAP_DISABLE_UNICODE,
        template_tests[i].json_text,
        template_tests[i].template_text);
    check_roundtrip(
        2,
        BSER_CAP_DISABLE_UNICODE_FOR_ERRORS,
        template_tests[i].json_text,
        template_tests[i].template_text);
    check_roundtrip(
        2,
        BSER_CAP_DISABLE_UNICODE | BSER_CAP_DISABLE_UNICODE_FOR_ERRORS,
        template_tests[i].json_text,
        template_tests[i].template_text);
  }

  for (i = 0; i < num_serial; i++) {
    check_serialization(
        1, 0, serialization_tests[i].json_text, serialization_tests[i].bserv1);
    check_serialization(
        2, 0, serialization_tests[i].json_text, serialization_tests[i].bserv2);
  }

  check_bser_typed_strings();
}

/* vim:ts=2:sw=2:et:
 */
