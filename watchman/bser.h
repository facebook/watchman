// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#pragma once

#include "watchman/thirdparty/jansson/jansson.h"

typedef struct bser_ctx {
  uint32_t bser_version;
  uint32_t bser_capabilities;
  json_dump_callback_t dump;
} bser_ctx_t;

#define BSER_MAGIC "\x00\x01"
#define BSER_V2_MAGIC "\x00\x02"

// BSERv2 capabilities. Must be powers of 2.
#define BSER_CAP_DISABLE_UNICODE 0x1
#define BSER_CAP_DISABLE_UNICODE_FOR_ERRORS 0x2

int w_bser_write_pdu(
    const uint32_t bser_version,
    const uint32_t capabilities,
    json_dump_callback_t dump,
    const json_ref& json,
    void* data);
int w_bser_dump(const bser_ctx_t* ctx, const json_ref& json, void* data);
bool bunser_int(
    const char* buf,
    json_int_t avail,
    json_int_t* needed,
    json_int_t* val);
json_ref bunser(
    const char* buf,
    const char* end,
    json_int_t* needed,
    json_error_t* jerr);
