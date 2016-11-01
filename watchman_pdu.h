/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

enum w_pdu_type {
  need_data,
  is_json_compact,
  is_json_pretty,
  is_bser,
  is_bser_v2
};

struct watchman_json_buffer {
  char *buf;
  uint32_t allocd;
  uint32_t rpos, wpos;
  enum w_pdu_type pdu_type;
};

typedef struct bser_ctx {
  uint32_t bser_version;
  uint32_t bser_capabilities;
  json_dump_callback_t dump;
} bser_ctx_t;

typedef struct watchman_json_buffer w_jbuffer_t;

bool w_json_buffer_init(w_jbuffer_t* jr);
void w_json_buffer_reset(w_jbuffer_t* jr);
void w_json_buffer_free(w_jbuffer_t* jr);
json_ref w_json_buffer_next(w_jbuffer_t* jr, w_stm_t stm, json_error_t* jerr);
bool w_json_buffer_passthru(
    w_jbuffer_t* jr,
    enum w_pdu_type output_pdu,
    w_jbuffer_t* output_pdu_buf,
    w_stm_t stm);
bool w_json_buffer_write(
    w_jbuffer_t* jr,
    w_stm_t stm,
    const json_ref& json,
    int flags);
bool w_json_buffer_write_bser(
    uint32_t bser_version,
    uint32_t bser_capabilities,
    w_jbuffer_t* jr,
    w_stm_t stm,
    const json_ref& json);
bool w_ser_write_pdu(
    enum w_pdu_type pdu_type,
    w_jbuffer_t* jr,
    w_stm_t stm,
    const json_ref& json);

#define BSER_MAGIC "\x00\x01"
#define BSER_V2_MAGIC "\x00\x02"
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
