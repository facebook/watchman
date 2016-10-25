/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Periodically connect to our endpoint and verify that we're talking
 * to ourselves.  This is normally a sign of madness, but if we don't
 * get an answer, or get a reply from someone else, we know things
 * are bad; someone removed our socket file or there was some kind of
 * race condition that resulted in multiple instances starting up.
 */

static void *check_my_sock(void *unused) {
  auto cmd = json_array({typed_string_to_json("get-pid", W_STRING_UNICODE)});
  w_stm_t client = NULL;
  w_jbuffer_t buf;
  json_error_t jerr;
  json_int_t remote_pid = 0;
  pid_t my_pid = getpid();

  unused_parameter(unused);
  w_set_thread_name("sockcheck");

  client = w_stm_connect(get_sock_name(), 6000);
  if (!client) {
    w_log(W_LOG_FATAL, "Failed to connect to myself for get-pid check: %s\n",
        strerror(errno));
    /* NOTREACHED */
  }

  w_stm_set_nonblock(client, false);

  w_json_buffer_init(&buf);
  if (!w_ser_write_pdu(is_bser, &buf, client, cmd)) {
    w_log(W_LOG_FATAL, "Failed to send get-pid PDU: %s\n",
          strerror(errno));
    /* NOTREACHED */
  }

  w_json_buffer_reset(&buf);
  auto result = w_json_buffer_next(&buf, client, &jerr);
  if (!result) {
    w_log(W_LOG_FATAL, "Failed to decode get-pid response: %s %s\n",
        jerr.text, strerror(errno));
    /* NOTREACHED */
  }

  if (json_unpack_ex(result, &jerr, 0, "{s:i}",
        "pid", &remote_pid) != 0) {
    w_log(W_LOG_FATAL, "Failed to extract pid from get-pid response: %s\n",
        jerr.text);
    /* NOTREACHED */
  }

  if (remote_pid != my_pid) {
    w_log(W_LOG_FATAL,
        "remote pid from get-pid (%ld) doesn't match my pid (%ld)\n",
        (long)remote_pid, (long)my_pid);
    /* NOTREACHED */
  }
  w_json_buffer_free(&buf);
  w_stm_close(client);
  return NULL;
}

void w_check_my_sock(void) {
  pthread_attr_t attr;
  pthread_t thr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&thr, &attr, check_my_sock, NULL);
  pthread_attr_destroy(&attr);
}

/* vim:ts=2:sw=2:et:
 */
