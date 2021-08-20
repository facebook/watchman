/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>
#include "watchman/thirdparty/jansson/jansson.h"

class watchman_stream;

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
  w_pdu_type pdu_type;
  uint32_t capabilities;

  ~watchman_json_buffer();
  watchman_json_buffer();
  watchman_json_buffer(const watchman_json_buffer&) = delete;
  watchman_json_buffer& operator=(const watchman_json_buffer&) = delete;

  void clear();
  bool
  jsonEncodeToStream(const json_ref& json, watchman_stream* stm, int flags);
  bool bserEncodeToStream(
      uint32_t bser_version,
      uint32_t bser_capabilities,
      const json_ref& json,
      watchman_stream* stm);

  bool pduEncodeToStream(
      w_pdu_type pdu_type,
      uint32_t capabilities,
      const json_ref& json,
      watchman_stream* stm);

  json_ref decodeNext(watchman_stream* stm, json_error_t* jerr);

  bool passThru(
      w_pdu_type output_pdu,
      uint32_t output_capabilities,
      watchman_json_buffer* output_pdu_buf,
      watchman_stream* stm);

 private:
  bool readAndDetectPdu(watchman_stream* stm, json_error_t* jerr);
  inline uint32_t shuntDown();
  bool fillBuffer(watchman_stream* stm);
  inline w_pdu_type detectPdu();
  json_ref readJsonPrettyPdu(watchman_stream* stm, json_error_t* jerr);
  json_ref readJsonPdu(watchman_stream* stm, json_error_t* jerr);
  json_ref
  readBserPdu(watchman_stream* stm, uint32_t bser_version, json_error_t* jerr);
  json_ref decodePdu(watchman_stream* stm, json_error_t* jerr);
  bool decodePduInfo(
      watchman_stream* stm,
      uint32_t bser_version,
      json_int_t* len,
      json_int_t* bser_capabilities,
      json_error_t* jerr);
  bool streamPdu(watchman_stream* stm, json_error_t* jerr);
  bool streamUntilNewLine(watchman_stream* stm);
  bool streamN(watchman_stream* stm, json_int_t len, json_error_t* jerr);
};

typedef struct watchman_json_buffer w_jbuffer_t;
