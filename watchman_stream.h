/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#ifndef WATCHMAN_STREAM_H
#define WATCHMAN_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

// Very limited stream abstraction to make it easier to
// deal with portability between Windows and POSIX.

struct watchman_stream;
typedef struct watchman_stream *w_stm_t;

struct watchman_event;
typedef struct watchman_event *w_evt_t;

struct watchman_stream_ops {
  int (*op_close)(w_stm_t stm);
  int (*op_read)(w_stm_t stm, void *buf, int size);
  int (*op_write)(w_stm_t stm, const void *buf, int size);
  void (*op_get_events)(w_stm_t stm, w_evt_t *readable);
  void (*op_set_nonblock)(w_stm_t stm, bool nonb);
  bool (*op_rewind)(w_stm_t stm);
  bool (*op_shutdown)(w_stm_t stm);
  bool (*op_peer_is_owner)(w_stm_t stm);
};

struct watchman_stream {
  void *handle;
  struct watchman_stream_ops *ops;
};

struct watchman_event_poll {
  struct watchman_event *evt;
  bool ready;
};

// Make a event that can be manually signalled
w_evt_t w_event_make(void);
// Manually signal an event
void w_event_set(w_evt_t evt);
void w_event_destroy(w_evt_t evt);
bool w_event_test_and_clear(w_evt_t evt);

// Go to sleep for up to timeoutms.
// Returns sooner if any of the watchman_event objects referenced
// in the array P are signalled
int w_poll_events(struct watchman_event_poll *p, int n, int timeoutms);

// Create a connected unix socket or a named pipe client stream
w_stm_t w_stm_connect(const char *path, int timeoutms);

int w_stm_close(w_stm_t stm);
int w_stm_read(w_stm_t stm, void *buf, int size);
int w_stm_write(w_stm_t stm, const void *buf, int size);
void w_stm_get_events(w_stm_t stm, w_evt_t *readable);
void w_stm_set_nonblock(w_stm_t stm, bool nonb);
bool w_stm_rewind(w_stm_t stm);
bool w_stm_shutdown(w_stm_t stm);
bool w_stm_peer_is_owner(w_stm_t stm);

w_stm_t w_stm_stdout(void);
w_stm_t w_stm_stdin(void);
#ifndef _WIN32
w_stm_t w_stm_connect_unix(const char *path, int timeoutms);
w_stm_t w_stm_fdopen(int fd);
w_stm_t w_stm_open(const char *path, int flags, ...);
int w_stm_fileno(w_stm_t stm);
#else
w_stm_t w_stm_connect_named_pipe(const char *path, int timeoutms);
w_stm_t w_stm_handleopen(HANDLE h);
w_stm_t w_stm_open(const char *path, int flags, ...);
HANDLE w_stm_handle(w_stm_t stm);
HANDLE w_handle_open(const char *path, int flags);
#endif

#ifdef __cplusplus
}
#endif

#endif
