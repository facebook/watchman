/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_LIBGIMLI_H
# include <libgimli.h>
#endif

/* This needs to be recursive safe because we may log to clients
 * while we are dispatching subscriptions to clients */
watchman::
    Synchronized<std::unordered_set<watchman_client*>, std::recursive_mutex>
        clients;
static int listener_fd = -1;
pthread_t reaper_thread;
static pthread_t listener_thread;
#ifdef _WIN32
static HANDLE listener_thread_event;
#endif
static volatile bool stopping = false;
#ifdef HAVE_LIBGIMLI_H
static volatile struct gimli_heartbeat *hb = NULL;
#endif

bool w_is_stopping(void) {
  return stopping;
}

json_ref make_response(void) {
  auto resp = json_object();

  resp.set("version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE));

  return resp;
}

/* must be called with the clients.wlock() held */
bool enqueue_response(
    struct watchman_client* client,
    json_ref&& json,
    bool ping) {
  client->responses.emplace_back(std::move(json));

  if (ping) {
    w_event_set(client->ping);
  }

  return true;
}

void send_and_dispose_response(
    struct watchman_client* client,
    json_ref&& response) {
  auto lock = clients.wlock();
  enqueue_response(client, std::move(response), false);
}

void send_error_response(struct watchman_client *client,
    const char *fmt, ...)
{
  char buf[WATCHMAN_NAME_MAX];
  va_list ap;
  auto resp = make_response();

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  auto errstr = typed_string_to_json(buf, W_STRING_MIXED);
  resp.set("error", std::move(errstr));

  if (client->perf_sample) {
    client->perf_sample->add_meta("error", std::move(errstr));
  }

  if (client->current_command) {
    char *command = NULL;
    command = json_dumps(client->current_command, 0);
    w_log(W_LOG_ERR, "send_error_response: %s failed: %s\n",
        command, buf);
    free(command);
  } else {
    w_log(W_LOG_ERR, "send_error_response: %s\n", buf);
  }

  send_and_dispose_response(client, std::move(resp));
}

watchman_client::~watchman_client() {
  w_log(W_LOG_DBG, "client_delete %p\n", this);

  w_json_buffer_free(&reader);
  w_json_buffer_free(&writer);
  w_event_destroy(ping);
  w_stm_shutdown(stm);
  w_stm_close(stm);
}

void w_request_shutdown(void) {
  stopping = true;
// Knock listener thread out of poll/accept
#ifndef _WIN32
  pthread_kill(listener_thread, SIGUSR1);
  pthread_kill(reaper_thread, SIGUSR1);
#else
  SetEvent(listener_thread_event);
#endif
}

// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
static void *client_thread(void *ptr)
{
  auto client = (watchman_client*)ptr;
  struct watchman_event_poll pfd[2];
  json_error_t jerr;
  bool send_ok = true;

  w_stm_set_nonblock(client->stm, true);
  w_set_thread_name("client=%p:stm=%p", client, client->stm);

  client->client_is_owner = w_stm_peer_is_owner(client->stm);

  w_stm_get_events(client->stm, &pfd[0].evt);
  pfd[1].evt = client->ping;

  while (!stopping) {
    // Wait for input from either the client socket or
    // via the ping pipe, which signals that some other
    // thread wants to unilaterally send data to the client

    ignore_result(w_poll_events(pfd, 2, 2000));

    if (stopping) {
      break;
    }

    if (pfd[0].ready) {
      auto request = w_json_buffer_next(&client->reader, client->stm, &jerr);

      if (!request && errno == EAGAIN) {
        // That's fine
      } else if (!request) {
        // Not so cool
        if (client->reader.wpos == client->reader.rpos) {
          // If they disconnected in between PDUs, no need to log
          // any error
          goto disconnected;
        }
        send_error_response(client, "invalid json at position %d: %s",
            jerr.position, jerr.text);
        w_log(W_LOG_ERR, "invalid data from client: %s\n", jerr.text);

        goto disconnected;
      } else if (request) {
        client->pdu_type = client->reader.pdu_type;
        dispatch_command(client, request, CMD_DAEMON);
      }
    }

    if (pfd[1].ready) {
      w_event_test_and_clear(client->ping);
    }

    /* de-queue the pending responses under the lock */
    std::deque<json_ref> queued_responses_to_send;
    {
      auto lock = clients.wlock();
      std::swap(queued_responses_to_send, client->responses);
    }

    /* now send our response(s) */
    while (!queued_responses_to_send.empty()) {
      auto& response_to_send = queued_responses_to_send.front();

      if (send_ok) {
        w_stm_set_nonblock(client->stm, false);
        /* Return the data in the same format that was used to ask for it.
         * Don't bother sending any more messages if the client disconnects,
         * but still free their memory.
         */
        send_ok = w_ser_write_pdu(
            client->pdu_type, &client->writer, client->stm, response_to_send);
        w_stm_set_nonblock(client->stm, true);
      }

      queued_responses_to_send.pop_front();
    }
  }

disconnected:
  w_set_thread_name("NOT_CONN:client=%p:stm=%p", client, client->stm);
  // Remove the client from the map before we tear it down, as this makes
  // it easier to flush out pending writes on windows without worrying
  // about w_log_to_clients contending for the write buffers
  clients.wlock()->erase(client);

  delete client;

  return NULL;
}

bool w_should_log_to_clients(int level)
{
  bool result = false;

  auto clientsLock = clients.wlock();

  for (auto client : *clientsLock) {
    if (client->log_level != W_LOG_OFF && client->log_level >= level) {
      result = true;
      break;
    }
  }

  return result;
}

void w_log_to_clients(int level, const char *buf)
{
  auto clientsLock = clients.wlock();

  for (auto client : *clientsLock) {
    if (client->log_level != W_LOG_OFF && client->log_level >= level) {
      auto json = make_response();
      if (json) {
        json.set({{"log", typed_string_to_json(buf, W_STRING_MIXED)},
                  {"unilateral", json_true()}});
        enqueue_response(client, std::move(json), true);
      }
    }
  }
}

// This is just a placeholder.
// This catches SIGUSR1 so we don't terminate.
// We use this to interrupt blocking syscalls
// on the worker threads
static void wakeme(int signo)
{
  unused_parameter(signo);
}

#if defined(HAVE_KQUEUE) || defined(HAVE_FSEVENTS)
#ifdef __OpenBSD__
#include <sys/siginfo.h>
#endif
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifndef _WIN32

// If we are running under inetd-style supervision, call this function
// to move the inetd provided socket descriptor(s) to a new descriptor
// number and remember that we can just use these when we're starting
// up the listener.
bool w_listener_prep_inetd(void) {
  if (listener_fd != -1) {
    w_log(W_LOG_ERR,
          "w_listener_prep_inetd: listener_fd is already assigned\n");
    return false;
  }

  listener_fd = dup(STDIN_FILENO);
  if (listener_fd == -1) {
    w_log(W_LOG_ERR, "w_listener_prep_inetd: failed to dup stdin: %s\n",
          strerror(errno));
    return false;
  }

  return true;
}

static int get_listener_socket(const char *path)
{
  struct sockaddr_un un;
  mode_t perms = cfg_get_perms(
      "sock_access", true /* write bits */, false /* execute bits */);

  if (listener_fd != -1) {
    // Assume that it was prepped by w_listener_prep_inetd()
    w_log(W_LOG_ERR, "Using socket from inetd as listening socket\n");
    return listener_fd;
  }

#ifdef __APPLE__
  listener_fd = w_get_listener_socket_from_launchd();
  if (listener_fd != -1) {
    w_log(W_LOG_ERR, "Using socket from launchd as listening socket\n");
    return listener_fd;
  }
#endif

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "%s: path is too long\n",
        path);
    return -1;
  }

  listener_fd = socket(PF_LOCAL, SOCK_STREAM, 0);
  if (listener_fd == -1) {
    w_log(W_LOG_ERR, "socket: %s\n",
        strerror(errno));
    return -1;
  }

  un.sun_family = PF_LOCAL;
  strcpy(un.sun_path, path);
  unlink(path);

  if (bind(listener_fd, (struct sockaddr*)&un, sizeof(un)) != 0) {
    w_log(W_LOG_ERR, "bind(%s): %s\n",
      path, strerror(errno));
    close(listener_fd);
    return -1;
  }

  // The permissions in the containing directory should be correct, so this
  // should be correct as well. But set the permissions in any case.
  if (chmod(path, perms) == -1) {
    w_log(W_LOG_ERR, "chmod(%s, %#o): %s", path, perms, strerror(errno));
    close(listener_fd);
    return -1;
  }

  if (listen(listener_fd, 200) != 0) {
    w_log(W_LOG_ERR, "listen(%s): %s\n",
        path, strerror(errno));
    close(listener_fd);
    return -1;
  }

  return listener_fd;
}
#endif

static struct watchman_client *make_new_client(w_stm_t stm) {
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  auto client = new watchman_user_client();
  if (!client) {
    pthread_attr_destroy(&attr);
    return NULL;
  }
  client->stm = stm;
  w_log(W_LOG_DBG, "accepted client:stm=%p\n", client->stm);

  if (!w_json_buffer_init(&client->reader)) {
    // FIXME: error handling
  }
  if (!w_json_buffer_init(&client->writer)) {
    // FIXME: error handling
  }
  client->ping = w_event_make();
  if (!client->ping) {
    // FIXME: error handling
  }

  clients.wlock()->insert(client);

  // Start a thread for the client.
  // We used to use libevent for this, but we have
  // a low volume of concurrent clients and the json
  // parse/encode APIs are not easily used in a non-blocking
  // server architecture.
  if (pthread_create(&client->thread_handle, &attr, client_thread, client)) {
    // It didn't work out, sorry!
    clients.wlock()->erase(client);
    delete client;
  }

  pthread_attr_destroy(&attr);

  return client;
}

#ifdef _WIN32
static void named_pipe_accept_loop(const char *path) {
  HANDLE handles[2];
  OVERLAPPED olap;
  HANDLE connected_event = CreateEvent(NULL, FALSE, TRUE, NULL);

  if (!connected_event) {
    w_log(W_LOG_ERR, "named_pipe_accept_loop: CreateEvent failed: %s\n",
        win32_strerror(GetLastError()));
    return;
  }

  listener_thread_event = CreateEvent(NULL, FALSE, TRUE, NULL);

  handles[0] = connected_event;
  handles[1] = listener_thread_event;
  memset(&olap, 0, sizeof(olap));
  olap.hEvent = connected_event;

  w_log(W_LOG_ERR, "waiting for pipe clients on %s\n", path);
  while (!stopping) {
    w_stm_t stm;
    HANDLE client_fd;
    DWORD res;

    client_fd = CreateNamedPipe(
        path,
        PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|
        PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        WATCHMAN_IO_BUF_SIZE,
        512, 0, NULL);

    if (client_fd == INVALID_HANDLE_VALUE) {
      w_log(W_LOG_ERR, "CreateNamedPipe(%s) failed: %s\n",
          path, win32_strerror(GetLastError()));
      continue;
    }

    ResetEvent(connected_event);
    if (!ConnectNamedPipe(client_fd, &olap)) {
      res = GetLastError();

      if (res == ERROR_PIPE_CONNECTED) {
        goto good_client;
      }

      if (res != ERROR_IO_PENDING) {
        w_log(W_LOG_ERR, "ConnectNamedPipe: %s\n",
            win32_strerror(GetLastError()));
        CloseHandle(client_fd);
        continue;
      }

      res = WaitForMultipleObjectsEx(2, handles, false, INFINITE, true);
      if (res == WAIT_OBJECT_0 + 1) {
        // Signalled to stop
        CancelIoEx(client_fd, &olap);
        CloseHandle(client_fd);
        continue;
      }

      if (res == WAIT_OBJECT_0) {
        goto good_client;
      }

      w_log(W_LOG_ERR, "WaitForMultipleObjectsEx: ConnectNamedPipe: "
          "unexpected status %u\n", res);
      CancelIoEx(client_fd, &olap);
      CloseHandle(client_fd);
    } else {
good_client:
      stm = w_stm_handleopen(client_fd);
      if (!stm) {
        w_log(W_LOG_ERR, "Failed to allocate stm for pipe handle: %s\n",
            strerror(errno));
        CloseHandle(client_fd);
        continue;
      }

      make_new_client(stm);
    }
  }
}
#endif

#ifndef _WIN32
static void accept_loop() {
  while (!stopping) {
    int client_fd;
    struct pollfd pfd;
    int bufsize;
    w_stm_t stm;

#ifdef HAVE_LIBGIMLI_H
    if (hb) {
      gimli_heartbeat_set(hb, GIMLI_HB_RUNNING);
    }
#endif

    pfd.events = POLLIN;
    pfd.fd = listener_fd;
    if (poll(&pfd, 1, 60000) < 1 || (pfd.revents & POLLIN) == 0) {
      if (stopping) {
        break;
      }
      // Timed out, or error.
      // Arrange to sanity check that we're working
      w_check_my_sock();
      continue;
    }

#ifdef HAVE_ACCEPT4
    client_fd = accept4(listener_fd, NULL, 0, SOCK_CLOEXEC);
#else
    client_fd = accept(listener_fd, NULL, 0);
#endif
    if (client_fd == -1) {
      continue;
    }
    w_set_cloexec(client_fd);
    bufsize = WATCHMAN_IO_BUF_SIZE;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF,
        (void*)&bufsize, sizeof(bufsize));

    stm = w_stm_fdopen(client_fd);
    if (!stm) {
      w_log(W_LOG_ERR, "Failed to allocate stm for fd: %s\n",
          strerror(errno));
      close(client_fd);
      continue;
    }
    make_new_client(stm);
  }
}
#endif

bool w_start_listener(const char *path)
{
#ifndef _WIN32
  struct sigaction sa;
  sigset_t sigset;
#endif
  void *ignored;

  listener_thread = pthread_self();

#ifdef HAVE_LIBGIMLI_H
  hb = gimli_heartbeat_attach();
#endif

#if defined(HAVE_KQUEUE) || defined(HAVE_FSEVENTS)
  {
    struct rlimit limit;
# ifndef __OpenBSD__
    int mib[2] = { CTL_KERN,
#  ifdef KERN_MAXFILESPERPROC
      KERN_MAXFILESPERPROC
#  else
      KERN_MAXFILES
#  endif
    };
# endif
    int maxperproc;

    getrlimit(RLIMIT_NOFILE, &limit);

# ifndef __OpenBSD__
    {
      size_t len;

      len = sizeof(maxperproc);
      sysctl(mib, 2, &maxperproc, &len, NULL, 0);
      w_log(W_LOG_ERR, "file limit is %" PRIu64
          " kern.maxfilesperproc=%i\n",
          limit.rlim_cur, maxperproc);
    }
# else
    maxperproc = limit.rlim_max;
    w_log(W_LOG_ERR, "openfiles-cur is %" PRIu64
        " openfiles-max=%i\n",
        limit.rlim_cur, maxperproc);
# endif

    if (limit.rlim_cur != RLIM_INFINITY &&
        maxperproc > 0 &&
        limit.rlim_cur < (rlim_t)maxperproc) {
      limit.rlim_cur = maxperproc;

      if (setrlimit(RLIMIT_NOFILE, &limit)) {
        w_log(W_LOG_ERR,
          "failed to raise limit to %" PRIu64 " (%s).\n",
          limit.rlim_cur,
          strerror(errno));
      } else {
        w_log(W_LOG_ERR,
            "raised file limit to %" PRIu64 "\n",
            limit.rlim_cur);
      }
    }

    getrlimit(RLIMIT_NOFILE, &limit);
#ifndef HAVE_FSEVENTS
    if (limit.rlim_cur < 10240) {
      w_log(W_LOG_ERR,
          "Your file descriptor limit is very low (%" PRIu64 "), "
          "please consult the watchman docs on raising the limits\n",
          limit.rlim_cur);
    }
#endif
  }
#endif

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);

  /* allow SIGUSR1 and SIGCHLD to wake up a blocked thread, without restarting
   * syscalls */
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = wakeme;
  sa.sa_flags = 0;
  sigaction(SIGUSR1, &sa, NULL);
  sigaction(SIGCHLD, &sa, NULL);

  // Block SIGCHLD everywhere
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGCHLD);
  sigprocmask(SIG_BLOCK, &sigset, NULL);

  listener_fd = get_listener_socket(path);
  if (listener_fd == -1) {
    return false;
  }
  w_set_cloexec(listener_fd);
#endif

#ifdef HAVE_LIBGIMLI_H
  if (hb) {
    gimli_heartbeat_set(hb, GIMLI_HB_RUNNING);
  } else {
    w_setup_signal_handlers();
  }
#else
  w_setup_signal_handlers();
#endif
  w_set_nonblock(listener_fd);

  // Now run the dispatch
#ifndef _WIN32
  accept_loop();
#else
  named_pipe_accept_loop(path);
#endif

#ifndef _WIN32
  /* close out some resources to persuade valgrind to run clean */
  close(listener_fd);
  listener_fd = -1;
#endif

  // Wait for clients, waking any sleeping clients up in the process
  {
    int interval = 2000;
    int last_count = 0, n_clients = 0;
    const int max_interval = 1000000; // 1 second

    do {
      {
        auto clientsLock = clients.wlock();
        n_clients = clientsLock->size();

        for (auto client : *clientsLock) {
          w_event_set(client->ping);

#ifndef _WIN32
          // If we've been waiting around for a while, interrupt
          // the client thread; it may be blocked on a write
          if (interval >= max_interval) {
            pthread_kill(client->thread_handle, SIGUSR1);
          }
#endif
        }
      }

      if (n_clients != last_count) {
        w_log(W_LOG_ERR, "waiting for %d clients to terminate\n", n_clients);
      }
      usleep(interval);
      interval = MIN(interval * 2, max_interval);
    } while (n_clients > 0);
  }

  pthread_join(reaper_thread, &ignored);
  w_state_shutdown();

  return true;
}

/* get-pid */
static void cmd_get_pid(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();

  resp.set("pid", json_integer(getpid()));

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("get-pid", cmd_get_pid, CMD_DAEMON, NULL)


/* vim:ts=2:sw=2:et:
 */
