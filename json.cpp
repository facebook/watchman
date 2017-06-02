/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

W_CAP_REG("bser-v2")
watchman_json_buffer::watchman_json_buffer()
    : buf((char*)malloc(WATCHMAN_IO_BUF_SIZE)),
      allocd(WATCHMAN_IO_BUF_SIZE),
      rpos(0),
      wpos(0),
      pdu_type(need_data),
      capabilities(0) {
  if (!buf) {
    throw std::bad_alloc();
  }
}

void watchman_json_buffer::clear() {
  wpos = 0;
  rpos = 0;
}

watchman_json_buffer::~watchman_json_buffer() {
  free(buf);
}

// Shunt down, return available size
uint32_t watchman_json_buffer::shuntDown() {
  if (rpos && rpos == wpos) {
    rpos = 0;
    wpos = 0;
  }
  if (rpos && rpos < wpos) {
    memmove(buf, buf + rpos, wpos - rpos);
    wpos -= rpos;
    rpos = 0;
  }
  return allocd - wpos;
}

bool watchman_json_buffer::fillBuffer(w_stm_t stm) {
  uint32_t avail;
  int r;

  avail = shuntDown();

  // Get some more space if we need it
  if (avail == 0) {
    char* newBuf = (char*)realloc(buf, allocd * 2);

    if (!newBuf) {
      return false;
    }

    buf = newBuf;
    allocd *= 2;

    avail = allocd - wpos;
  }

  errno = 0;
  r = stm->read(buf + wpos, avail);
  if (r <= 0) {
    return false;
  }

  wpos += r;

  return true;
}

inline enum w_pdu_type watchman_json_buffer::detectPdu() {
  if (wpos - rpos < 2) {
    return need_data;
  }
  if (memcmp(buf + rpos, BSER_MAGIC, 2) == 0) {
    return is_bser;
  }
  if (memcmp(buf + rpos, BSER_V2_MAGIC, 2) == 0) {
    return is_bser_v2;
  }
  return is_json_compact;
}

json_ref watchman_json_buffer::readJsonPrettyPdu(
    w_stm_t stm,
    json_error_t* jerr) {
  char *nl;
  int r;
  json_ref res;

  // Assume newline is at the end of what we have
  nl = buf + wpos;
  r = (int)(nl - (buf + rpos));
  res = json_loadb(buf + rpos, r, 0, jerr);
  while (!res) {
    // Maybe we can fill more data into the buffer and retry?
    if (!fillBuffer(stm)) {
      // No, then error is terminal
      return nullptr;
    }
    // Recompute end of buffer
    nl = buf + wpos;
    r = (int)(nl - (buf + rpos));
    // And try parsing this
    res = json_loadb(buf + rpos, r, 0, jerr);
  }

  // update read pos to look beyond this point
  rpos += r + 1;

  return res;
}

json_ref watchman_json_buffer::readJsonPdu(w_stm_t stm, json_error_t* jerr) {
  int r;

  /* look for a newline; that indicates the end of
   * a json packet */
  auto nl = (char*)memchr(buf + rpos, '\n', wpos - rpos);

  // If we don't have a newline, we need to fill the
  // buffer
  while (!nl) {
    if (!fillBuffer(stm)) {
      if (errno == 0 && stm == w_stm_stdin()) {
        // Ugly-ish hack to support the -j CLI option.  This allows
        // us to consume a JSON input that doesn't end with a newline.
        // We only allow this on EOF when reading from stdin
        nl = buf + wpos;
        break;
      }
      return nullptr;
    }
    nl = (char*)memchr(buf + rpos, '\n', wpos - rpos);
  }

  // buflen
  r = (int)(nl - (buf + rpos));
  auto res = json_loadb(buf + rpos, r, 0, jerr);

  // update read pos to look beyond this point
  rpos += r + 1;

  return res;
}

bool watchman_json_buffer::decodePduInfo(
    w_stm_t stm,
    uint32_t bser_version,
    json_int_t* len,
    json_int_t* bser_capabilities,
    json_error_t* jerr) {
  json_int_t needed;
  if (bser_version == 2) {
    uint32_t capabilities;
    while (wpos - rpos < sizeof(capabilities)) {
      if (!fillBuffer(stm)) {
        snprintf(jerr->text, sizeof(jerr->text),
            "unable to fill buffer");
        return false;
      }
    }
    // json_int_t is architecture-dependent, so go through the uint32_t for
    // safety.
    memcpy(&capabilities, buf + rpos, sizeof(capabilities));
    *bser_capabilities = capabilities;
    rpos += sizeof(capabilities);
  }

  while (!bunser_int(buf + rpos, wpos - rpos, &needed, len)) {
    if (needed == -1) {
      snprintf(jerr->text, sizeof(jerr->text),
          "failed to read PDU size");
      return false;
    }
    if (!fillBuffer(stm)) {
      snprintf(jerr->text, sizeof(jerr->text),
          "unable to fill buffer");
      return false;
    }
  }
  rpos += (uint32_t)needed;

  return true;
}

json_ref watchman_json_buffer::readBserPdu(
    w_stm_t stm,
    uint32_t bser_version,
    json_error_t* jerr) {
  json_int_t needed;
  json_int_t val;
  json_int_t bser_capabilities;
  uint32_t ideal;
  int r;
  json_ref obj;

  rpos += 2;

  // We don't handle EAGAIN cleanly in here
  stm->setNonBlock(false);
  if (!decodePduInfo(stm, bser_version, &val, &bser_capabilities, jerr)) {
    return nullptr;
  }

  // val tells us exactly how much storage we need for this PDU
  if (val > allocd - wpos) {
    ideal = allocd;
    while ((ideal - wpos) < (uint32_t)val) {
      ideal *= 2;
    }
    if (ideal > allocd) {
      auto newBuf = (char*)realloc(buf, ideal);

      if (!newBuf) {
        snprintf(jerr->text, sizeof(jerr->text),
            "out of memory while allocating %" PRIu32 " bytes",
            ideal);
        return nullptr;
      }

      buf = newBuf;
      allocd = ideal;
    }
  }

  // We have enough room for the whole thing, let's read it in
  while ((wpos - rpos) < val) {
    r = stm->read(buf + wpos, allocd - wpos);
    if (r <= 0) {
      jerr->position = wpos - rpos;
      snprintf(
          jerr->text,
          sizeof(jerr->text),
          "error reading %" PRIu32 " bytes val=%" PRIu64
          " wpos=%" PRIu32 " rpos=%" PRIu32 " for PDU: %s",
          uint32_t(allocd - wpos),
          int64_t(val),
          wpos,
          rpos,
          strerror(errno));
      return nullptr;
    }
    wpos += r;
  }

  obj = bunser(buf + rpos, buf + wpos, &needed, jerr);

  // Ensure that we move the read position to the wpos; we consumed it all
  rpos = wpos;

  stm->setNonBlock(true);
  return obj;
}

bool watchman_json_buffer::readAndDetectPdu(w_stm_t stm, json_error_t* jerr) {
  enum w_pdu_type pdu;
  // The client might send us different kinds of PDUs over the same connection,
  // so reset the capabilities.
  capabilities = 0;

  shuntDown();
  pdu = detectPdu();
  if (pdu == need_data) {
    if (!fillBuffer(stm)) {
      if (errno != EAGAIN) {
        snprintf(jerr->text, sizeof(jerr->text),
          "fill_buffer: %s",
          errno ? strerror(errno) : "EOF");
      }
      return false;
    }
    pdu = detectPdu();
  }

  if (pdu == is_bser_v2) {
    // read capabilities (since we haven't increased rpos, first two bytes are
    // still the header)
    while (wpos - rpos < 2 + sizeof(capabilities)) {
      if (!fillBuffer(stm)) {
        if (errno != EAGAIN) {
          snprintf(
              jerr->text,
              sizeof(jerr->text),
              "fillBuffer: %s",
              errno ? strerror(errno) : "EOF");
        }
        return false;
      }
    }

    // Copy the capabilities over. BSER is system-endian so this is safe.
    memcpy(&capabilities, buf + rpos + 2, sizeof(capabilities));
  }

  if (pdu == is_json_compact && stm == w_stm_stdin()) {
    // Minor hack for the `-j` option for reading pretty printed
    // json from stdin
    pdu = is_json_pretty;
  }

  pdu_type = pdu;
  return true;
}

static bool output_bytes(const char *buf, int x)
{
  int res;

  while (x > 0) {
    res = (int)fwrite(buf, 1, x, stdout);
    if (res == 0) {
      return false;
    }
    buf += res;
    x -= res;
  }
  return true;
}

bool watchman_json_buffer::streamUntilNewLine(w_stm_t stm) {
  int x;
  char* localBuf;
  bool is_done = false;

  while (true) {
    localBuf = buf + rpos;
    auto nl = (char*)memchr(localBuf, '\n', wpos - rpos);
    if (nl) {
      x = 1 + (int)(nl - localBuf);
      is_done = true;
    } else {
      x = wpos - rpos;
    }

    if (!output_bytes(localBuf, x)) {
      return false;
    }
    localBuf += x;
    rpos += x;

    if (is_done) {
      break;
    }

    if (!fillBuffer(stm)) {
      break;
    }
  }
  return true;
}

bool watchman_json_buffer::streamN(
    w_stm_t stm,
    json_int_t len,
    json_error_t* jerr) {
  uint32_t total = 0;

  if (!output_bytes(buf, rpos)) {
    snprintf(
        jerr->text,
        sizeof(jerr->text),
        "failed output headers bytes %d: %s\n",
        rpos,
        strerror(errno));
    return false;
  }
  while (len > 0) {
    uint32_t avail = wpos - rpos;
    int r;

    if (avail) {
      if (!output_bytes(buf + rpos, avail)) {
        snprintf(
            jerr->text,
            sizeof(jerr->text),
            "output_bytes: avail=%d, failed %s\n",
            avail,
            strerror(errno));
        return false;
      }
      rpos += avail;
      len -= avail;

      if (len == 0) {
        return true;
      }
    }

    avail = std::min((uint32_t)len, shuntDown());
    r = stm->read(buf + wpos, avail);

    if (r <= 0) {
      snprintf(
          jerr->text,
          sizeof(jerr->text),
          "read: len=%" PRIi64 " wanted %" PRIu32 " got %d %s\n",
          (int64_t)len,
          avail,
          r,
          strerror(errno));
      return false;
    }
    wpos += r;
    total += r;
  }
  return true;
}

bool watchman_json_buffer::streamPdu(w_stm_t stm, json_error_t* jerr) {
  uint32_t bser_version = 1;
  json_int_t bser_capabilities;
  json_int_t len;

  switch (pdu_type) {
    case is_json_compact:
    case is_json_pretty:
      return streamUntilNewLine(stm);
    case is_bser:
    case is_bser_v2:
      {
      if (pdu_type == is_bser_v2) {
        bser_version = 2;
        } else {
          bser_version = 1;
        }
        rpos += 2;
        if (!decodePduInfo(stm, bser_version, &len, &bser_capabilities, jerr)) {
          return false;
        }
        return streamN(stm, len, jerr);
      }
    default:
      w_log(W_LOG_FATAL, "not streaming for pdu type %d\n", pdu_type);
      return false;
  }
}

json_ref watchman_json_buffer::decodePdu(w_stm_t stm, json_error_t* jerr) {
  switch (pdu_type) {
    case is_json_compact:
      return readJsonPdu(stm, jerr);
    case is_json_pretty:
      return readJsonPrettyPdu(stm, jerr);
    case is_bser_v2:
      return readBserPdu(stm, 2, jerr);
    default: // bser v1
      return readBserPdu(stm, 1, jerr);
  }
}

bool watchman_json_buffer::passThru(
    enum w_pdu_type output_pdu,
    uint32_t output_capabilities,
    w_jbuffer_t* output_pdu_buf,
    w_stm_t stm) {
  json_error_t jerr;
  bool res;

  stm->setNonBlock(false);
  if (!readAndDetectPdu(stm, &jerr)) {
    w_log(W_LOG_ERR, "failed to identify PDU: %s\n",
        jerr.text);
    return false;
  }

  if (pdu_type == output_pdu) {
    // We can stream it through
    if (!streamPdu(stm, &jerr)) {
      w_log(W_LOG_ERR, "stream_pdu: %s\n", jerr.text);
      return false;
    }
    return true;
  }

  auto j = decodePdu(stm, &jerr);

  if (!j) {
    w_log(W_LOG_ERR, "failed to parse response: %s\n",
        jerr.text);
    return false;
  }

  output_pdu_buf->clear();

  res = output_pdu_buf->pduEncodeToStream(
      output_pdu, output_capabilities, j, w_stm_stdout());

  return res;
}

json_ref watchman_json_buffer::decodeNext(w_stm_t stm, json_error_t* jerr) {
  memset(jerr, 0, sizeof(*jerr));
  if (!readAndDetectPdu(stm, jerr)) {
    return nullptr;
  }
  return decodePdu(stm, jerr);
}

struct jbuffer_write_data {
  w_stm_t stm;
  w_jbuffer_t* jr;

  bool flush() {
    int x;

    while (jr->wpos - jr->rpos) {
      x = stm->write(jr->buf + jr->rpos, jr->wpos - jr->rpos);

      if (x <= 0) {
        return false;
      }

      jr->rpos += x;
    }

    jr->clear();
    return true;
  }

  static int write(const char* buffer, size_t size, void* ptr) {
    auto data = (jbuffer_write_data*)ptr;
    return data->write(buffer, size);
  }

  int write(const char* buffer, size_t size) {
    while (size) {
      // Accumulate in the buffer
      int room = jr->allocd - jr->wpos;

      // No room? send it over the wire
      if (!room) {
        if (!flush()) {
          return -1;
        }
        room = jr->allocd - jr->wpos;
      }

      if ((int)size < room) {
        room = (int)size;
      }

      // Stick it in the buffer
      memcpy(jr->buf + jr->wpos, buffer, room);

      buffer += room;
      size -= room;
      jr->wpos += room;
    }

    return 0;
  }
};

bool watchman_json_buffer::bserEncodeToStream(
    uint32_t bser_version,
    uint32_t bser_capabilities,
    const json_ref& json,
    w_stm_t stm) {
  struct jbuffer_write_data data = {stm, this};
  int res;

  res = w_bser_write_pdu(
      bser_version, bser_capabilities, jbuffer_write_data::write, json, &data);

  if (res != 0) {
    return false;
  }

  return data.flush();
}

bool watchman_json_buffer::jsonEncodeToStream(
    const json_ref& json,
    w_stm_t stm,
    int flags) {
  struct jbuffer_write_data data = {stm, this};
  int res;

  res = json_dump_callback(json, jbuffer_write_data::write, &data, flags);

  if (res != 0) {
    return false;
  }

  if (data.write("\n", 1) != 0) {
    return false;
  }

  return data.flush();
}

bool watchman_json_buffer::pduEncodeToStream(
    enum w_pdu_type pdu_type,
    uint32_t capabilities,
    const json_ref& json,
    w_stm_t stm) {
  switch (pdu_type) {
    case is_json_compact:
      return jsonEncodeToStream(json, stm, JSON_COMPACT);
    case is_json_pretty:
      return jsonEncodeToStream(json, stm, JSON_INDENT(4));
    case is_bser:
      return bserEncodeToStream(1, capabilities, json, stm);
    case is_bser_v2:
      return bserEncodeToStream(2, capabilities, json, stm);
    case need_data:
    default:
      return false;
  }
}

/* vim:ts=2:sw=2:et:
 */
