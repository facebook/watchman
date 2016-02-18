/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_LIBGIMLI_H
# include <libgimli.h>
#endif

/* This needs to be recursive safe because we may log to clients
 * while we are dispatching subscriptions to clients */
pthread_mutex_t w_client_lock;
w_ht_t *clients = NULL;
static int listener_fd;
static pthread_t reaper_thread;
static pthread_t listener_thread;
#ifdef _WIN32
static HANDLE listener_thread_event;
#endif
static volatile bool stopping = false;
#ifdef HAVE_LIBGIMLI_H
static volatile struct gimli_heartbeat *hb = NULL;
#endif

json_t *make_response(void)
{
  json_t *resp = json_object();

  set_prop(resp, "version", json_string_nocheck(PACKAGE_VERSION));

  return resp;
}

static int proc_pid;
static uint64_t proc_start_time;

bool clock_id_string(uint32_t root_number, uint32_t ticks, char *buf,
    size_t bufsize)
{
  int res = snprintf(buf, bufsize, "c:%" PRIu64 ":%d:%u:%" PRIu32,
                     proc_start_time, proc_pid, root_number, ticks);

  if (res == -1) {
    return false;
  }
  return (size_t)res < bufsize;
}

// Renders the current clock id string to the supplied buffer.
// Must be called with the root locked.
static bool current_clock_id_string(w_root_t *root,
    char *buf, size_t bufsize)
{
  return clock_id_string(root->number, root->ticks, buf, bufsize);
}

/* Add the current clock value to the response.
 * must be called with the root locked */
void annotate_with_clock(w_root_t *root, json_t *resp)
{
  char buf[128];

  if (current_clock_id_string(root, buf, sizeof(buf))) {
    set_prop(resp, "clock", json_string_nocheck(buf));
  }
}

/* must be called with the w_client_lock held */
bool enqueue_response(struct watchman_client *client,
    json_t *json, bool ping)
{
  struct watchman_client_response *resp;

  resp = calloc(1, sizeof(*resp));
  if (!resp) {
    return false;
  }
  resp->json = json;

  if (client->tail) {
    client->tail->next = resp;
  } else {
    client->head = resp;
  }
  client->tail = resp;

  if (ping) {
    w_event_set(client->ping);
  }

  return true;
}

void send_and_dispose_response(struct watchman_client *client,
    json_t *response)
{
  pthread_mutex_lock(&w_client_lock);
  if (!enqueue_response(client, response, false)) {
    json_decref(response);
  }
  pthread_mutex_unlock(&w_client_lock);
}

void send_error_response(struct watchman_client *client,
    const char *fmt, ...)
{
  char buf[WATCHMAN_NAME_MAX];
  va_list ap;
  json_t *resp = make_response();

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  w_log(W_LOG_ERR, "send_error_response: %s\n", buf);
  set_prop(resp, "error", json_string_nocheck(buf));

  send_and_dispose_response(client, resp);
}

static void client_delete(struct watchman_client *client)
{
  struct watchman_client_response *resp;

  w_log(W_LOG_DBG, "client_delete %p\n", client);

  /* cancel subscriptions */
  w_ht_free(client->subscriptions);

  while (client->head) {
    resp = client->head;
    client->head = resp->next;
    json_decref(resp->json);
    free(resp);
  }

  w_json_buffer_free(&client->reader);
  w_json_buffer_free(&client->writer);
  w_event_destroy(client->ping);
  w_stm_shutdown(client->stm);
  w_stm_close(client->stm);
  free(client);
}

static void delete_subscription(w_ht_val_t val)
{
  struct watchman_client_subscription *sub = w_ht_val_ptr(val);

  w_string_delref(sub->name);
  w_query_delref(sub->query);
  if (sub->drop_or_defer) {
    w_ht_free(sub->drop_or_defer);
  }
  free(sub);
}

static const struct watchman_hash_funcs subscription_hash_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL,
  delete_subscription
};

struct w_clockspec *w_clockspec_new_clock(uint32_t root_number, uint32_t ticks)
{
  struct w_clockspec *spec;
  spec = calloc(1, sizeof(*spec));
  if (!spec) {
    return NULL;
  }
  spec->tag = w_cs_clock;
  spec->clock.start_time = proc_start_time;
  spec->clock.pid = proc_pid;
  spec->clock.root_number = root_number;
  spec->clock.ticks = ticks;
  return spec;
}

struct w_clockspec *w_clockspec_parse(json_t *value)
{
  const char *str;
  uint64_t start_time;
  int pid;
  uint32_t root_number;
  uint32_t ticks;

  struct w_clockspec *spec;

  spec = calloc(1, sizeof(*spec));
  if (!spec) {
    return NULL;
  }

  if (json_is_integer(value)) {
    spec->tag = w_cs_timestamp;
    spec->timestamp.tv_usec = 0;
    spec->timestamp.tv_sec = (time_t)json_integer_value(value);
    return spec;
  }

  str = json_string_value(value);
  if (!str) {
    free(spec);
    return NULL;
  }

  if (str[0] == 'n' && str[1] == ':') {
    spec->tag = w_cs_named_cursor;
    // spec owns the ref to the string
    spec->named_cursor.cursor = w_string_new(str);
    return spec;
  }

  if (sscanf(str, "c:%" PRIu64 ":%d:%" PRIu32 ":%" PRIu32,
             &start_time, &pid, &root_number, &ticks) == 4) {
    spec->tag = w_cs_clock;
    spec->clock.start_time = start_time;
    spec->clock.pid = pid;
    spec->clock.root_number = root_number;
    spec->clock.ticks = ticks;
    return spec;
  }

  if (sscanf(str, "c:%d:%" PRIu32, &pid, &ticks) == 2) {
    // old-style clock value (<= 2.8.2) -- by setting clock time and root number
    // to 0 we guarantee that this is treated as a fresh instance
    spec->tag = w_cs_clock;
    spec->clock.start_time = 0;
    spec->clock.pid = pid;
    spec->clock.root_number = root_number;
    spec->clock.ticks = ticks;
    return spec;
  }

  free(spec);
  return NULL;
}

// must be called with the root locked
// spec can be null, in which case a fresh instance is assumed
void w_clockspec_eval(w_root_t *root,
    const struct w_clockspec *spec,
    struct w_query_since *since)
{
  if (spec == NULL) {
    since->is_timestamp = false;
    since->clock.is_fresh_instance = true;
    since->clock.ticks = 0;
    return;
  }

  if (spec->tag == w_cs_timestamp) {
    // just copy the values over
    since->is_timestamp = true;
    since->timestamp = spec->timestamp;
    return;
  }

  since->is_timestamp = false;

  if (spec->tag == w_cs_named_cursor) {
    w_ht_val_t ticks_val;
    w_string_t *cursor = spec->named_cursor.cursor;
    since->clock.is_fresh_instance = !w_ht_lookup(root->cursors,
                                                  w_ht_ptr_val(cursor),
                                                  &ticks_val, false);
    if (!since->clock.is_fresh_instance) {
      since->clock.is_fresh_instance = ticks_val < root->last_age_out_tick;
    }
    if (since->clock.is_fresh_instance) {
      since->clock.ticks = 0;
    } else {
      since->clock.ticks = (uint32_t)ticks_val;
    }

    // Bump the tick value and record it against the cursor.
    // We need to bump the tick value so that repeated queries
    // when nothing has changed in the filesystem won't continue
    // to return the same set of files; we only want the first
    // of these to return the files and the rest to return nothing
    // until something subsequently changes
    w_ht_replace(root->cursors, w_ht_ptr_val(cursor), ++root->ticks);

    w_log(W_LOG_DBG, "resolved cursor %.*s -> %" PRIu32 "\n",
        cursor->len, cursor->buf, since->clock.ticks);
    return;
  }

  // spec->tag == w_cs_clock
  if (spec->clock.start_time == proc_start_time &&
      spec->clock.pid == proc_pid &&
      spec->clock.root_number == root->number) {

    since->clock.is_fresh_instance =
      spec->clock.ticks < root->last_age_out_tick;
    if (since->clock.is_fresh_instance) {
      since->clock.ticks = 0;
    } else {
      since->clock.ticks = spec->clock.ticks;
    }
    if (spec->clock.ticks == root->ticks) {
      /* Force ticks to increment.  This avoids returning and querying the
       * same tick value over and over when no files have changed in the
       * meantime */
      root->ticks++;
    }
    return;
  }

  // If the pid, start time or root number don't match, they asked a different
  // incarnation of the server or a different instance of this root, so we treat
  // them as having never spoken to us before
  since->clock.is_fresh_instance = true;
  since->clock.ticks = 0;
}

void w_clockspec_free(struct w_clockspec *spec)
{
  if (spec->tag == w_cs_named_cursor) {
    w_string_delref(spec->named_cursor.cursor);
  }
  free(spec);
}

void add_root_warnings_to_response(json_t *response, w_root_t *root) {
  char *str = NULL;
  char *full = NULL;

  if (!root->last_recrawl_reason && !root->warning) {
    return;
  }

  if (root->last_recrawl_reason) {
    ignore_result(asprintf(&str,
          "Recrawled this watch %d times, most recently because:\n"
          "%.*s\n"
          "To resolve, please review the information on\n"
          "%s#recrawl",
          root->recrawl_count,
          root->last_recrawl_reason->len,
          root->last_recrawl_reason->buf,
          cfg_get_trouble_url()));
  }

  ignore_result(asprintf(&full,
      "%.*s%s"   // root->warning
      "%s\n"     // str (last recrawl reason)
      "To clear this warning, run:\n"
      "`watchman watch-del %.*s ; watchman watch-project %.*s`\n",
      root->warning ? root->warning->len : 0,
      root->warning ? root->warning->buf : "",
      root->warning && str ? "\n" : "", // newline if we have both strings
      str ? str : "",
      root->root_path->len,
      root->root_path->buf,
      root->root_path->len,
      root->root_path->buf));

  if (full) {
    set_prop(response, "warning", json_string_nocheck(full));
  }
  free(str);
  free(full);
}

w_root_t *resolve_root_or_err(
    struct watchman_client *client,
    json_t *args,
    int root_index,
    bool create)
{
  w_root_t *root;
  const char *root_name;
  char *errmsg = NULL;
  json_t *ele;

  ele = json_array_get(args, root_index);
  if (!ele) {
    send_error_response(client, "wrong number of arguments");
    return NULL;
  }

  root_name = json_string_value(ele);
  if (!root_name) {
    send_error_response(client,
        "invalid value for argument %d, expected "
        "a string naming the root dir",
        root_index);
    return NULL;
  }

  if (client->client_mode) {
    root = w_root_resolve_for_client_mode(root_name, &errmsg);
  } else {
    root = w_root_resolve(root_name, create, &errmsg);
  }

  if (!root) {
    send_error_response(client,
        "unable to resolve root %s: %s",
        root_name, errmsg);
    free(errmsg);
  }
  return root;
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

static void cmd_shutdown(
    struct watchman_client *client,
    json_t *args)
{
  json_t *resp = make_response();
  unused_parameter(args);

  w_log(W_LOG_ERR, "shutdown-server was requested, exiting!\n");
  stopping = true;

  w_request_shutdown();

  set_prop(resp, "shutdown-server", json_true());
  send_and_dispose_response(client, resp);
}
W_CMD_REG("shutdown-server", cmd_shutdown, CMD_DAEMON|CMD_POISON_IMMUNE, NULL)

// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
static void *client_thread(void *ptr)
{
  struct watchman_client *client = ptr;
  struct watchman_event_poll pfd[2];
  struct watchman_client_response *queued_responses_to_send;
  json_t *request;
  json_error_t jerr;
  bool send_ok = true;

  w_stm_set_nonblock(client->stm, true);
  w_set_thread_name("client:stm=%p", client->stm);

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
      request = w_json_buffer_next(&client->reader, client->stm, &jerr);

      if (!request && errno == EAGAIN) {
        // That's fine
      } else if (!request) {
        // Not so cool
        if (client->reader.wpos == client->reader.rpos) {
          // If they disconnected in between PDUs, no need to log
          // any error
          goto disconected;
        }
        send_error_response(client, "invalid json at position %d: %s",
            jerr.position, jerr.text);
        w_log(W_LOG_ERR, "invalid data from client: %s\n", jerr.text);

        goto disconected;
      } else if (request) {
        client->pdu_type = client->reader.pdu_type;
        dispatch_command(client, request, CMD_DAEMON);
        json_decref(request);
      }
    }

    if (pfd[1].ready) {
      w_event_test_and_clear(client->ping);
    }

    /* de-queue the pending responses under the lock */
    pthread_mutex_lock(&w_client_lock);
    queued_responses_to_send = client->head;
    client->head = NULL;
    client->tail = NULL;
    pthread_mutex_unlock(&w_client_lock);

    /* now send our response(s) */
    while (queued_responses_to_send) {
      struct watchman_client_response *response_to_send =
        queued_responses_to_send;

      if (send_ok) {
        w_stm_set_nonblock(client->stm, false);
        /* Return the data in the same format that was used to ask for it.
         * Don't bother sending any more messages if the client disconnects,
         * but still free their memory.
         */
        send_ok = w_ser_write_pdu(client->pdu_type, &client->writer,
                                  client->stm, response_to_send->json);
        w_stm_set_nonblock(client->stm, true);
      }

      queued_responses_to_send = response_to_send->next;

      json_decref(response_to_send->json);
      free(response_to_send);
    }
  }

disconected:
  // Remove the client from the map before we tear it down, as this makes
  // it easier to flush out pending writes on windows without worrying
  // about w_log_to_clients contending for the write buffers
  pthread_mutex_lock(&w_client_lock);
  w_ht_del(clients, w_ht_ptr_val(client));
  pthread_mutex_unlock(&w_client_lock);

  w_client_vacate_states(client);
  client_delete(client);

  return NULL;
}

bool w_should_log_to_clients(int level)
{
  w_ht_iter_t iter;
  bool result = false;

  pthread_mutex_lock(&w_client_lock);

  if (!clients) {
    pthread_mutex_unlock(&w_client_lock);
    return false;
  }

  if (w_ht_first(clients, &iter)) do {
    struct watchman_client *client = w_ht_val_ptr(iter.value);

    if (client->log_level != W_LOG_OFF && client->log_level >= level) {
      result = true;
      break;
    }

  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);

  return result;
}

void w_log_to_clients(int level, const char *buf)
{
  json_t *json = NULL;
  w_ht_iter_t iter;

  if (!clients) {
    return;
  }

  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    struct watchman_client *client = w_ht_val_ptr(iter.value);

    if (client->log_level != W_LOG_OFF && client->log_level >= level) {
      json = make_response();
      if (json) {
        set_prop(json, "log", json_string_nocheck(buf));
        if (!enqueue_response(client, json, true)) {
          json_decref(json);
        }
      }
    }

  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);
}

static void *child_reaper(void *arg)
{
#ifndef _WIN32
  sigset_t sigset;

  // By default, keep both SIGCHLD and SIGUSR1 blocked
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGUSR1);
  sigaddset(&sigset, SIGCHLD);
  pthread_sigmask(SIG_BLOCK, &sigset, NULL);

  // SIGCHLD is ordinarily blocked, so we listen for it only in
  // sigsuspend, when we're also listening for the SIGUSR1 that tells
  // us to exit.
  pthread_sigmask(SIG_BLOCK, NULL, &sigset);
  sigdelset(&sigset, SIGCHLD);
  sigdelset(&sigset, SIGUSR1);

#endif
  unused_parameter(arg);
  w_set_thread_name("child_reaper");

#ifdef _WIN32
  while (!stopping) {
    usleep(200000);
    w_reap_children(true);
  }
#else
  while (!stopping) {
    w_reap_children(false);
    sigsuspend(&sigset);
  }
#endif

  return 0;
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
static int get_listener_socket(const char *path)
{
  struct sockaddr_un un;

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
  struct watchman_client *client;
  pthread_attr_t attr;
  pthread_t thr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  client = calloc(1, sizeof(*client));
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
  client->subscriptions = w_ht_new(2, &subscription_hash_funcs);

  pthread_mutex_lock(&w_client_lock);
  w_ht_set(clients, w_ht_ptr_val(client), w_ht_ptr_val(client));
  pthread_mutex_unlock(&w_client_lock);

  // Start a thread for the client.
  // We used to use libevent for this, but we have
  // a low volume of concurrent clients and the json
  // parse/encode APIs are not easily used in a non-blocking
  // server architecture.
  if (pthread_create(&thr, &attr, client_thread, client)) {
    // It didn't work out, sorry!
    pthread_mutex_lock(&w_client_lock);
    w_ht_del(clients, w_ht_ptr_val(client));
    pthread_mutex_unlock(&w_client_lock);
    client_delete(client);
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
  struct timeval tv;

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

  proc_pid = (int)getpid();
  if (gettimeofday(&tv, NULL) == -1) {
    w_log(W_LOG_ERR, "gettimeofday failed: %s\n", strerror(errno));
    return false;
  }
  proc_start_time = (uint64_t)tv.tv_sec;

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

  if (pthread_create(&reaper_thread, NULL, child_reaper, NULL)) {
    w_log(W_LOG_FATAL, "pthread_create(reaper): %s\n",
        strerror(errno));
    return false;
  }

  if (!clients) {
    clients = w_ht_new(2, NULL);
  }

  w_state_load();

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

    do {
      w_ht_iter_t iter;

      pthread_mutex_lock(&w_client_lock);
      n_clients = w_ht_size(clients);

      if (w_ht_first(clients, &iter)) do {
        struct watchman_client *client = w_ht_val_ptr(iter.value);
        w_event_set(client->ping);
      } while (w_ht_next(clients, &iter));

      pthread_mutex_unlock(&w_client_lock);

      if (n_clients != last_count) {
        w_log(W_LOG_ERR, "waiting for %d clients to terminate\n", n_clients);
      }
      usleep(interval);
      interval = MIN(interval * 2, 1000000);
    } while (n_clients > 0);
  }

  w_root_free_watched_roots();

  pthread_join(reaper_thread, &ignored);
  cfg_shutdown();

  return true;
}

/* vim:ts=2:sw=2:et:
 */
