/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool w_json_buffer_init(w_jbuffer_t *jr)
{
  memset(jr, 0, sizeof(*jr));

  jr->allocd = WATCHMAN_IO_BUF_SIZE;
  jr->buf = malloc(jr->allocd);

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

static bool fill_buffer(w_jbuffer_t *jr, int fd)
{
  uint32_t avail;
  int r;

  avail = shunt_down(jr);

  // Get some more space if we need it
  if (avail == 0) {
    char *buf = realloc(jr->buf, jr->allocd * 2);

    if (!buf) {
      return false;
    }

    jr->buf = buf;
    jr->allocd *= 2;

    avail = jr->allocd - jr->wpos;
  }

  r = read(fd, jr->buf + jr->wpos, avail);
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
  return is_json_compact;
}

static json_t *read_json_pdu(w_jbuffer_t *jr, int fd, json_error_t *jerr)
{
  char *nl;
  int r;
  json_t *res;

  /* look for a newline; that indicates the end of
   * a json packet */
  nl = memchr(jr->buf + jr->rpos, '\n', jr->wpos - jr->rpos);

  // If we don't have a newline, we need to fill the
  // buffer
  while (!nl) {
    if (!fill_buffer(jr, fd)) {
      return NULL;
    }
    nl = memchr(jr->buf + jr->rpos, '\n', jr->wpos - jr->rpos);
  }

  // buflen
  r = nl - (jr->buf + jr->rpos);
  res = json_loadb(jr->buf + jr->rpos, r, 0, jerr);

  // update read pos to look beyond this point
  jr->rpos += r + 1;

  return res;
}

bool w_bser_decode_pdu_len(w_jbuffer_t *jr, int fd,
    json_int_t *len, json_error_t *jerr)
{
  int needed;

  while (!bunser_int(jr->buf + jr->rpos, jr->wpos - jr->rpos,
        &needed, len)) {
    if (needed == -1) {
      snprintf(jerr->text, sizeof(jerr->text),
          "failed to read PDU size");
      return false;
    }
    if (!fill_buffer(jr, fd)) {
      snprintf(jerr->text, sizeof(jerr->text),
          "unable to fill buffer");
      return false;
    }
  }
  jr->rpos += needed;

  return true;
}

static json_t *read_bser_pdu(w_jbuffer_t *jr, int fd, json_error_t *jerr)
{
  int needed;
  json_int_t val;
  uint32_t ideal;
  int need;
  int r;
  json_t *obj;

  jr->rpos += 2;

  // We don't handle EAGAIN cleanly in here
  w_clear_nonblock(fd);
  if (!w_bser_decode_pdu_len(jr, fd, &val, jerr)) {
    return NULL;
  }

  // val tells us exactly how much storage we need for this PDU
  need = val - (jr->allocd - jr->wpos);
  if (need > 0) {
    ideal = jr->allocd;
    while (ideal < (uint32_t)need) {
      ideal *= 2;
    }
    if (ideal > jr->allocd) {
      char *buf = realloc(jr->buf, ideal);

      if (!buf) {
        snprintf(jerr->text, sizeof(jerr->text),
            "out of memory while allocating %" PRIu32 " bytes",
            ideal);
        return NULL;
      }

      jr->buf = buf;
      jr->allocd = ideal;
    }
  }

  // We have enough room for the whole thing, let's read it in
  while ((jr->wpos - jr->rpos) < val) {
    r = read(fd, jr->buf + jr->wpos, jr->allocd - jr->wpos);
    if (r <= 0) {
      snprintf(jerr->text, sizeof(jerr->text),
          "error reading PDU: %s",
          strerror(errno));
      return NULL;
    }
    jr->wpos += r;
  }

  obj = bunser(jr->buf + jr->rpos, jr->buf + jr->wpos, &needed, jerr);

  // Ensure that we move the read position to the wpos; we consumed it all
  jr->rpos = jr->wpos;

  w_set_nonblock(fd);
  return obj;
}

static bool read_and_detect_pdu(w_jbuffer_t *jr, int fd, json_error_t *jerr)
{
  enum w_pdu_type pdu;

  shunt_down(jr);
  pdu = detect_pdu(jr);
  if (pdu == need_data) {
    if (!fill_buffer(jr, fd)) {
      snprintf(jerr->text, sizeof(jerr->text),
          "fill_buffer: %s",
          strerror(errno));
      return false;
    }
    pdu = detect_pdu(jr);
  }

  jr->pdu_type = pdu;
  return true;
}

static bool output_bytes(const char *buf, int x)
{
  int res;

  while (x > 0) {
    res = fwrite(buf, 1, x, stdout);
    if (res == 0) {
      return false;
    }
    buf += res;
    x -= res;
  }
  return true;
}

static bool stream_until_newline(w_jbuffer_t *reader, int fd)
{
  int x;
  char *buf, *nl;
  bool is_done = false;

  while (true) {
    buf = reader->buf + reader->rpos;
    nl = memchr(buf, '\n', reader->wpos - reader->rpos);
    if (nl) {
      x = 1 + (nl - buf);
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

    if (!fill_buffer(reader, fd)) {
      break;
    }
  }
  return true;
}

static bool stream_n_bytes(w_jbuffer_t *jr, int fd, json_int_t len,
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

    avail = MIN(len, shunt_down(jr));
    r = read(fd, jr->buf + jr->wpos, avail);

    if (r <= 0) {
      snprintf(jerr->text, sizeof(jerr->text),
        "read: len=%"PRIi64" wanted %"PRIu32" got %d %s\n",
        (int64_t)len, avail,
        r, strerror(errno));
      return false;
    }
    jr->wpos += r;
    total += r;
  }
  return true;
}

static bool stream_pdu(w_jbuffer_t *jr, int fd, json_error_t *jerr)
{
  switch (jr->pdu_type) {
    case is_json_compact:
    case is_json_pretty:
      return stream_until_newline(jr, fd);
    case is_bser:
      {
        json_int_t len;
        jr->rpos += 2;
        if (!w_bser_decode_pdu_len(jr, fd, &len, jerr)) {
          return false;
        }
        return stream_n_bytes(jr, fd, len, jerr);
      }
    default:
      w_log(W_LOG_FATAL, "not streaming for pdu type %d\n", jr->pdu_type);
      return false;
  }
}

static json_t *read_pdu_into_json(w_jbuffer_t *jr, int fd, json_error_t *jerr)
{
  if (jr->pdu_type == is_json_compact) {
    return read_json_pdu(jr, fd, jerr);
  }
  return read_bser_pdu(jr, fd, jerr);
}

bool w_json_buffer_passthru(w_jbuffer_t *jr,
    enum w_pdu_type output_pdu,
    int fd)
{
  json_t *j;
  json_error_t jerr;
  bool res;

  if (!read_and_detect_pdu(jr, fd, &jerr)) {
    w_log(W_LOG_ERR, "failed to identify PDU: %s\n",
        jerr.text);
    return false;
  }

  if (jr->pdu_type == output_pdu) {
    // We can stream it through
    if (!stream_pdu(jr, fd, &jerr)) {
      w_log(W_LOG_ERR, "stream_pdu: %s\n", jerr.text);
      return false;
    }
    return true;
  }

  j = read_pdu_into_json(jr, fd, &jerr);

  if (!j) {
    w_log(W_LOG_ERR, "failed to parse response: %s\n",
        jerr.text);
    return false;
  }

  w_json_buffer_reset(jr);
  res = w_ser_write_pdu(output_pdu, jr, STDOUT_FILENO, j);

  json_decref(j);
  return res;
}

json_t *w_json_buffer_next(w_jbuffer_t *jr, int fd, json_error_t *jerr)
{
  if (!read_and_detect_pdu(jr, fd, jerr)) {
    return NULL;
  }
  return read_pdu_into_json(jr, fd, jerr);
}

struct jbuffer_write_data {
  int fd;
  w_jbuffer_t *jr;
};

static bool jbuffer_flush(struct jbuffer_write_data *data)
{
  int x;

  while (data->jr->wpos - data->jr->rpos) {
    x = write(data->fd, data->jr->buf + data->jr->rpos,
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
  struct jbuffer_write_data *data = ptr;

  while (size) {
    // Accumulate in the buffer
    size_t room = data->jr->allocd - data->jr->wpos;

    // No room? send it over the wire
    if (!room) {
      if (!jbuffer_flush(data)) {
        return -1;
      }
      room = data->jr->allocd - data->jr->wpos;
    }

    if (size < room) {
      room = size;
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

bool w_json_buffer_write_bser(w_jbuffer_t *jr, int fd, json_t *json)
{
  struct jbuffer_write_data data = { fd, jr };
  int res;

  res = w_bser_write_pdu(json, jbuffer_write, &data);

  if (res != 0) {
    return false;
  }

  return jbuffer_flush(&data);
}

bool w_json_buffer_write(w_jbuffer_t *jr, int fd, json_t *json, int flags)
{
  struct jbuffer_write_data data = { fd, jr };
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

bool w_ser_write_pdu(enum w_pdu_type pdu_type,
    w_jbuffer_t *jr, int fd, json_t *json)
{
  switch (pdu_type) {
    case is_json_compact:
      return w_json_buffer_write(jr, fd, json, JSON_COMPACT);
    case is_json_pretty:
      return w_json_buffer_write(jr, fd, json, JSON_INDENT(4));
    case is_bser:
      return w_json_buffer_write_bser(jr, fd, json);
    case need_data:
    default:
      return false;
  }
}


/* vim:ts=2:sw=2:et:
 */

