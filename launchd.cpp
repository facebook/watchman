/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#ifdef __APPLE__
#include <launch.h>

using watchman::FileDescriptor;

/* When running under launchd, we prefer to obtain our listening
 * socket from it.  We don't strictly need to run this way, but if we didn't,
 * when the user runs `watchman shutdown-server` the launchd job is left in
 * a waiting state and needs to be explicitly triggered to get it working
 * again.
 * By having the socket registered in our job description, launchd knows
 * that we want to be activated in this way and takes care of it for us.
 *
 * This is made more fun because Yosemite introduces launch_activate_socket()
 * as a shortcut for this flow and deprecated pretty much everything else
 * in launch.h.  We use the deprecated functions so that we can run on
 * older releases.
 * */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
FileDescriptor w_get_listener_socket_from_launchd() {
  launch_data_t req, resp, socks;

  req = launch_data_new_string(LAUNCH_KEY_CHECKIN);
  if (req == NULL) {
    w_log(W_LOG_ERR, "unable to create LAUNCH_KEY_CHECKIN\n");
    return FileDescriptor();
  }

  resp = launch_msg(req);
  launch_data_free(req);

  if (resp == NULL) {
    w_log(W_LOG_ERR, "launchd checkin failed %s\n", strerror(errno));
    return FileDescriptor();
  }

  if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
    w_log(W_LOG_ERR, "launchd checkin failed: %s\n",
          strerror(launch_data_get_errno(resp)));
    launch_data_free(resp);
    return FileDescriptor();
  }

  socks = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_SOCKETS);
  if (socks == NULL) {
    w_log(W_LOG_ERR, "launchd didn't provide any sockets\n");
    launch_data_free(resp);
    return FileDescriptor();
  }

  // the "sock" name here is coupled with the plist in main.c
  socks = launch_data_dict_lookup(socks, "sock");
  if (socks == NULL) {
    w_log(W_LOG_ERR, "launchd: \"sock\" wasn't present in Sockets\n");
    launch_data_free(resp);
    return FileDescriptor();
  }

  return FileDescriptor(
      launch_data_get_fd(launch_data_array_get_index(socks, 0)));
}
#endif

/* vim:ts=2:sw=2:et:
 */
