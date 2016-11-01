/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool w_json_buffer_init(w_jbuffer_t *jr)
{
  memset(jr, 0, sizeof(*jr));

  jr->allocd = WATCHMAN_IO_BUF_SIZE;
  jr->buf = (char*)malloc(jr->allocd);

  if (!jr->buf) {
    return false;
  }

  return true;
}

void w_json_buffer_reset(w_jbuffer_t *jr)
{
  jr->wpos = 0;
  jr->rpos = 0;
}

void w_json_buffer_free(w_jbuffer_t *jr)
{
  free(jr->buf);
  memset(jr, 0, sizeof(*jr));
}

// Shunt down, return available size
static inline uint32_t shunt_down(w_jbuffer_t *jr)
{
  if (jr->rpos && jr->rpos == jr->wpos) {
    jr->rpos = 0;
    jr->wpos = 0;
  }
  if (jr->rpos && jr->rpos < jr->wpos) {
    memmove(jr->buf, jr->buf + jr->rpos, jr->wpos - jr->rpos);
    jr->wpos -= jr->rpos;
    jr->rpos = 0;

  }
  return jr->allocd - jr->wpos;
}

static bool fill_buffer(w_jbuffer_t *jr, w_stm_t stm)
{
  uint32_t avail;
  int r;

  avail = shunt_down(jr);

  // Get some more space if we need it
  if (avail == 0) {
    char *buf = (char*)realloc(jr->buf, jr->allocd * 2);

    if (!buf) {
      return false;
    }

    jr->buf = buf;
    jr->allocd *= 2;

    avail = jr->allocd - jr->wpos;
  }

  errno = 0;
  r = w_stm_read(stm, jr->buf + jr->wpos, avail);
  if (r <= 0) {
    return false;
  }

  jr->wpos += r;

  return true;
}

static inline enum w_pdu_type detect_pdu(w_jbuffer_t *jr)
{
  if (jr->wpos - jr->rpos < 2) {
    return need_data;
  }
  if (memcmp(jr->buf + jr->rpos, BSER_MAGIC, 2) == 0) {
    return is_bser;
  }
  if (memcmp(jr->buf + jr->rpos, BSER_V2_MAGIC, 2) == 0) {
    return is_bser_v2;
  }
  return is_json_compact;
}

static json_ref
read_json_pretty_pdu(w_jbuffer_t* jr, w_stm_t stm, json_error_t* jerr) {
  char *nl;
  int r;
  json_ref res;

  // Assume newline is at the end of what we have
  nl = jr->buf + jr->wpos;
  r = (int)(nl - (jr->buf + jr->rpos));
  res = json_loadb(jr->buf + jr->rpos, r, 0, jerr);
  if (!res) {
    // Maybe we can fill more data into the buffer and retry?
    if (!fill_buffer(jr, stm)) {
      // No, then error is terminal
      return nullptr;
    }
    // Recompute end of buffer
    nl = jr->buf + jr->wpos;
    r = (int)(nl - (jr->buf + jr->rpos));
    // And try parsing this
    res = json_loadb(jr->buf + jr->rpos, r, 0, jerr);
  }

  // update read pos to look beyond this point
  jr->rpos += r + 1;

  return res;
}

static json_ref
read_json_pdu(w_jbuffer_t* jr, w_stm_t stm, json_error_t* jerr) {
  int r;

  /* look for a newline; that indicates the end of
   * a json packet */
  auto nl = (char*)memchr(jr->buf + jr->rpos, '\n', jr->wpos - jr->rpos);

  // If we don't have a newline, we need to fill the
  // buffer
  while (!nl) {
    if (!fill_buffer(jr, stm)) {
      if (errno == 0 && stm == w_stm_stdin()) {
        // Ugly-ish hack to support the -j CLI option.  This allows
        // us to consume a JSON input that doesn't end with a newline.
        // We only allow this on EOF when reading from stdin
        nl = jr->buf + jr->wpos;
        break;
      }
      return nullptr;
    }
    nl = (char*)memchr(jr->buf + jr->rpos, '\n', jr->wpos - jr->rpos);
  }

  // buflen
  r = (int)(nl - (jr->buf + jr->rpos));
  auto res = json_loadb(jr->buf + jr->rpos, r, 0, jerr);

  // update read pos to look beyond this point
  jr->rpos += r + 1;

  return res;
}

bool w_bser_decode_pdu_info(w_jbuffer_t *jr, w_stm_t stm, uint32_t bser_version,
    json_int_t *len, json_int_t *bser_capabilities, json_error_t *jerr)
{
  json_int_t needed;
  if (bser_version == 2) {
    while (!bunser_int(jr->buf + jr->rpos, jr->wpos - jr->rpos,
          &needed, bser_capabilities)) {
      if (needed == -1) {
        snprintf(jerr->text, sizeof(jerr->text),
            "failed to read BSER capabilities");
        return false;
      }
      if (!fill_buffer(jr, stm)) {
        snprintf(jerr->text, sizeof(jerr->text),
            "unable to fill buffer");
        return false;
      }
    }
    jr->rpos += (uint32_t)needed;
  }
  while (!bunser_int(jr->buf + jr->rpos, jr->wpos - jr->rpos,
        &needed, len)) {
    if (needed == -1) {
      snprintf(jerr->text, sizeof(jerr->text),
          "failed to read PDU size");
      return false;
    }
    if (!fill_buffer(jr, stm)) {
      snprintf(jerr->text, sizeof(jerr->text),
          "unable to fill buffer");
      return false;
    }
  }
  jr->rpos += (uint32_t)needed;

  return true;
}

static json_ref read_bser_pdu(
    w_jbuffer_t* jr,
    w_stm_t stm,
    uint32_t bser_version,
    json_error_t* jerr) {
  json_int_t needed;
  json_int_t val;
  json_int_t bser_capabilities;
  uint32_t ideal;
  json_int_t need;
  int r;
  json_ref obj;

  jr->rpos += 2;

  // We don't handle EAGAIN cleanly in here
  w_stm_set_nonblock(stm, false);
  if (!w_bser_decode_pdu_info(jr, stm, bser_version, &val, &bser_capabilities,
      jerr)) {
    return nullptr;
  }

  // val tells us exactly how much storage we need for this PDU
  need = val - (jr->allocd - jr->wpos);
  if (need > 0) {
    ideal = jr->allocd;
    while (ideal < (uint32_t)need) {
      ideal *= 2;
    }
    if (ideal > jr->allocd) {
      auto buf = (char*)realloc(jr->buf, ideal);

      if (!buf) {
        snprintf(jerr->text, sizeof(jerr->text),
            "out of memory while allocating %" PRIu32 " bytes",
            ideal);
        return nullptr;
      }

      jr->buf = buf;
      jr->allocd = ideal;
    }
  }

  // We have enough room for the whole thing, let's read it in
  while ((jr->wpos - jr->rpos) < val) {
    r = w_stm_read(stm, jr->buf + jr->wpos, jr->allocd - jr->wpos);
    if (r <= 0) {
      snprintf(jerr->text, sizeof(jerr->text),
          "error reading PDU: %s",
          strerror(errno));
      return nullptr;
    }
    jr->wpos += r;
  }

  obj = bunser(jr->buf + jr->rpos, jr->buf + jr->wpos, &needed, jerr);

  // Ensure that we move the read position to the wpos; we consumed it all
  jr->rpos = jr->wpos;

  w_stm_set_nonblock(stm, true);
  return obj;
}

static bool read_and_detect_pdu(w_jbuffer_t *jr, w_stm_t stm,
    json_error_t *jerr)
{
  enum w_pdu_type pdu;

  shunt_down(jr);
  pdu = detect_pdu(jr);
  if (pdu == need_data) {
    if (!fill_buffer(jr, stm)) {
      if (errno != EAGAIN) {
        snprintf(jerr->text, sizeof(jerr->text),
          "fill_buffer: %s",
          errno ? strerror(errno) : "EOF");
      }
      return false;
    }
    pdu = detect_pdu(jr);
  }

  if (pdu == is_json_compact && stm == w_stm_stdin()) {
    // Minor hack for the `-j` option for reading pretty printed
    // json from stdin
    pdu = is_json_pretty;
  }

  jr->pdu_type = pdu;
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

static bool stream_until_newline(w_jbuffer_t *reader, w_stm_t stm)
{
  int x;
  char *buf;
  bool is_done = false;

  while (true) {
    buf = reader->buf + reader->rpos;
    auto nl = (char*)memchr(buf, '\n', reader->wpos - reader->rpos);
    if (nl) {
      x = 1 + (int)(nl - buf);
      is_done = true;
    } else {
      x = reader->wpos - reader->rpos;
    }

    if (!output_bytes(buf, x)) {
      return false;
    }
    buf += x;
    reader->rpos += x;

    if (is_done) {
      break;
    }

    if (!fill_buffer(reader, stm)) {
      break;
    }
  }
  return true;
}

static bool stream_n_bytes(w_jbuffer_t *jr, w_stm_t stm, json_int_t len,
    json_error_t *jerr)
{
  uint32_t total = 0;

  if (!output_bytes(jr->buf, jr->rpos)) {
    snprintf(jerr->text, sizeof(jerr->text),
        "failed output headers bytes %d: %s\n",
        jr->rpos, strerror(errno));
    return false;
  }
  while (len > 0) {
    uint32_t avail = jr->wpos - jr->rpos;
    int r;

    if (avail) {
      if (!output_bytes(jr->buf + jr->rpos, avail)) {
        snprintf(jerr->text, sizeof(jerr->text),
            "output_bytes: avail=%d, failed %s\n",
            avail, strerror(errno));
        return false;
      }
      jr->rpos += avail;
      len -= avail;

      if (len == 0) {
        return true;
      }
    }

    avail = MIN((uint32_t)len, shunt_down(jr));
    r = w_stm_read(stm, jr->buf + jr->wpos, avail);

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
    jr->wpos += r;
    total += r;
  }
  return true;
}

static bool stream_pdu(w_jbuffer_t *jr, w_stm_t stm, json_error_t *jerr)
{
  uint32_t bser_version = 1;
  json_int_t bser_capabilities;
  json_int_t len;

  switch (jr->pdu_type) {
    case is_json_compact:
    case is_json_pretty:
      return stream_until_newline(jr, stm);
    case is_bser:
    case is_bser_v2:
      {
        if (jr->pdu_type == is_bser_v2) {
          bser_version = 2;
        } else {
          bser_version = 1;
        }
        jr->rpos += 2;
        if (!w_bser_decode_pdu_info(jr, stm, bser_version, &len,
            &bser_capabilities, jerr)) {
          return false;
        }
        return stream_n_bytes(jr, stm, len, jerr);
      }
    default:
      w_log(W_LOG_FATAL, "not streaming for pdu type %d\n", jr->pdu_type);
      return false;
  }
}

static json_ref
read_pdu_into_json(w_jbuffer_t* jr, w_stm_t stm, json_error_t* jerr) {
  switch (jr->pdu_type) {
    case is_json_compact:
      return read_json_pdu(jr, stm, jerr);
    case is_json_pretty:
      return read_json_pretty_pdu(jr, stm, jerr);
    case is_bser_v2:
      return read_bser_pdu(jr, stm, 2, jerr);
    default: // bser v1
      return read_bser_pdu(jr, stm, 1, jerr);
  }
}

bool w_json_buffer_passthru(w_jbuffer_t *jr,
    enum w_pdu_type output_pdu,
    w_jbuffer_t *output_pdu_buf,
    w_stm_t stm)
{
  json_error_t jerr;
  bool res;

  w_stm_set_nonblock(stm, false);
  if (!read_and_detect_pdu(jr, stm, &jerr)) {
    w_log(W_LOG_ERR, "failed to identify PDU: %s\n",
        jerr.text);
    return false;
  }

  if (jr->pdu_type == output_pdu) {
    // We can stream it through
    if (!stream_pdu(jr, stm, &jerr)) {
      w_log(W_LOG_ERR, "stream_pdu: %s\n", jerr.text);
      return false;
    }
    return true;
  }

  auto j = read_pdu_into_json(jr, stm, &jerr);

  if (!j) {
    w_log(W_LOG_ERR, "failed to parse response: %s\n",
        jerr.text);
    return false;
  }

  w_json_buffer_reset(output_pdu_buf);

  res = w_ser_write_pdu(output_pdu, output_pdu_buf, w_stm_stdout(), j);

  return res;
}

json_ref w_json_buffer_next(w_jbuffer_t* jr, w_stm_t stm, json_error_t* jerr) {
  memset(jerr, 0, sizeof(*jerr));
  if (!read_and_detect_pdu(jr, stm, jerr)) {
    return nullptr;
  }
  return read_pdu_into_json(jr, stm, jerr);
}

struct jbuffer_write_data {
  w_stm_t stm;
  w_jbuffer_t *jr;
};

static bool jbuffer_flush(struct jbuffer_write_data *data)
{
  int x;

  while (data->jr->wpos - data->jr->rpos) {
    x = w_stm_write(data->stm, data->jr->buf + data->jr->rpos,
        data->jr->wpos - data->jr->rpos);

    if (x <= 0) {
      return false;
    }

    data->jr->rpos += x;
  }

  data->jr->rpos = data->jr->wpos = 0;
  return true;
}

static int jbuffer_write(const char *buffer, size_t size, void *ptr)
{
  auto data = (jbuffer_write_data *)ptr;

  while (size) {
    // Accumulate in the buffer
    int room = data->jr->allocd - data->jr->wpos;

    // No room? send it over the wire
    if (!room) {
      if (!jbuffer_flush(data)) {
        return -1;
      }
      room = data->jr->allocd - data->jr->wpos;
    }

    if ((int)size < room) {
      room = (int)size;
    }

    // Stick it in the buffer
    memcpy(data->jr->buf + data->jr->wpos,
        buffer, room);

    buffer += room;
    size -= room;
    data->jr->wpos += room;
  }

  return 0;
}

bool w_json_buffer_write_bser(
    uint32_t bser_version,
    uint32_t bser_capabilities,
    w_jbuffer_t* jr,
    w_stm_t stm,
    const json_ref& json) {
  struct jbuffer_write_data data = { stm, jr };
  int res;

  res = w_bser_write_pdu(bser_version, bser_capabilities, jbuffer_write, json,
      &data);

  if (res != 0) {
    return false;
  }

  return jbuffer_flush(&data);
}

bool w_json_buffer_write(
    w_jbuffer_t* jr,
    w_stm_t stm,
    const json_ref& json,
    int flags) {
  struct jbuffer_write_data data = { stm, jr };
  int res;

  res = json_dump_callback(json, jbuffer_write, &data, flags);

  if (res != 0) {
    return false;
  }

  if (jbuffer_write("\n", 1, &data) != 0) {
    return false;
  }

  return jbuffer_flush(&data);
}

bool w_ser_write_pdu(
    enum w_pdu_type pdu_type,
    w_jbuffer_t* jr,
    w_stm_t stm,
    const json_ref& json) {
  switch (pdu_type) {
    case is_json_compact:
      return w_json_buffer_write(jr, stm, json, JSON_COMPACT);
    case is_json_pretty:
      return w_json_buffer_write(jr, stm, json, JSON_INDENT(4));
    case is_bser:
      return w_json_buffer_write_bser(1, 0, jr, stm, json);
    case is_bser_v2:
      return w_json_buffer_write_bser(2, 0, jr, stm, json);
    case need_data:
    default:
      return false;
  }
}

/* vim:ts=2:sw=2:et:
 */
