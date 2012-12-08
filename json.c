/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman.h"

bool w_json_buffer_init(w_jbuffer_t *jr)
{
  memset(jr, 0, sizeof(*jr));

  jr->allocd = 8192;
  jr->buf = malloc(jr->allocd);

  if (!jr->buf) {
    return false;
  }

  return true;
}

void w_json_buffer_free(w_jbuffer_t *jr)
{
  free(jr->buf);
  memset(jr, 0, sizeof(*jr));
}

json_t *w_json_buffer_next(w_jbuffer_t *jr, int fd, json_error_t *jerr)
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
    uint32_t avail = jr->allocd - jr->wpos;

    // Shunt down
    if (jr->rpos && jr->rpos < jr->wpos) {
      memmove(jr->buf, jr->buf + jr->rpos, jr->wpos - jr->rpos);
      jr->wpos -= jr->rpos;
      jr->rpos = 0;

      avail = jr->allocd - jr->wpos;
    }

    // Get some more space if we need it
    if (avail == 0) {
      char *buf = realloc(jr->buf, jr->allocd * 2);

      if (!buf) {
        return NULL;
      }

      jr->buf = buf;
      jr->allocd *= 2;

      avail = jr->allocd - jr->wpos;
    }

    r = read(fd, jr->buf + jr->wpos, avail);
    if (r <= 0) {
      return NULL;
    }

    jr->wpos += r;

    // Look again for a newline
    nl = memchr(jr->buf + jr->rpos, '\n', jr->wpos - jr->rpos);
  }

  // buflen
  r = nl - (jr->buf + jr->rpos);
  res = json_loadb(jr->buf + jr->rpos, r, 0, jerr);

  // update read pos to look beyond this point
  jr->rpos += r + 1;

  return res;
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


/* vim:ts=2:sw=2:et:
 */

