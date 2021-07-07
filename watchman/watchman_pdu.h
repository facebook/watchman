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
  char* buf;
  uint32_t allocd;
  uint32_t rpos, wpos;
  enum w_pdu_type pdu_type;
  uint32_t capabilities;

  ~watchman_json_buffer();
  watchman_json_buffer();
  watchman_json_buffer(const watchman_json_buffer&) = delete;
  watchman_json_buffer& operator=(const watchman_json_buffer&) = delete;

  void clear();
  bool jsonEncodeToStream(const json_ref& json, w_stm_t stm, int flags);
  bool bserEncodeToStream(
      uint32_t bser_version,
      uint32_t bser_capabilities,
      const json_ref& json,
      w_stm_t stm);

  bool pduEncodeToStream(
      enum w_pdu_type pdu_type,
      uint32_t capabilities,
      const json_ref& json,
      w_stm_t stm);

  json_ref decodeNext(w_stm_t stm, json_error_t* jerr);

  bool passThru(
      enum w_pdu_type output_pdu,
      uint32_t output_capabilities,
      watchman_json_buffer* output_pdu_buf,
      w_stm_t stm);

 private:
  bool readAndDetectPdu(w_stm_t stm, json_error_t* jerr);
  inline uint32_t shuntDown();
  bool fillBuffer(w_stm_t stm);
  inline enum w_pdu_type detectPdu();
  json_ref readJsonPrettyPdu(w_stm_t stm, json_error_t* jerr);
  json_ref readJsonPdu(w_stm_t stm, json_error_t* jerr);
  json_ref readBserPdu(w_stm_t stm, uint32_t bser_version, json_error_t* jerr);
  json_ref decodePdu(w_stm_t stm, json_error_t* jerr);
  bool decodePduInfo(
      w_stm_t stm,
      uint32_t bser_version,
      json_int_t* len,
      json_int_t* bser_capabilities,
      json_error_t* jerr);
  bool streamPdu(w_stm_t stm, json_error_t* jerr);
  bool streamUntilNewLine(w_stm_t stm);
  bool streamN(w_stm_t stm, json_int_t len, json_error_t* jerr);
};

typedef struct watchman_json_buffer w_jbuffer_t;
