/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_LIBGIMLI_H
# include <libgimli.h>
#endif
#include <thread>

using watchman::FileDescriptor;

watchman::Synchronized<std::unordered_set<std::shared_ptr<watchman_client>>>
    clients;
static FileDescriptor listener_fd;
#ifndef _WIN32
static std::unique_ptr<watchman_event> listener_thread_event;
#else
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

bool enqueue_response(
    struct watchman_client* client,
    json_ref&& json,
    bool ping) {
  client->enqueueResponse(std::move(json), ping);
  return true;
}

void send_and_dispose_response(
    struct watchman_client* client,
    json_ref&& response) {
  enqueue_response(client, std::move(response), false);
}

void send_error_response(struct watchman_client *client,
    const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  auto errorText = w_string::vprintf(fmt, ap);
  va_end(ap);

  auto resp = make_response();
  resp.set("error", w_string_to_json(errorText));

  if (client->perf_sample) {
    client->perf_sample->add_meta("error", w_string_to_json(errorText));
  }

  if (client->current_command) {
    char *command = NULL;
    command = json_dumps(client->current_command, 0);
    watchman::log(
        watchman::ERR,
        "send_error_response: ",
        command,
        ", failed: ",
        errorText,
        "\n");
    free(command);
  } else {
    watchman::log(watchman::ERR, "send_error_response: ", errorText, "\n");
  }

  send_and_dispose_response(client, std::move(resp));
}

watchman_client::watchman_client() : watchman_client(nullptr) {}

watchman_client::watchman_client(std::unique_ptr<watchman_stream>&& stm)
    : stm(std::move(stm)), ping(w_event_make()) {
  w_log(W_LOG_DBG, "accepted client:stm=%p\n", this->stm.get());
}

watchman_client::~watchman_client() {
  debugSub.reset();
  errorSub.reset();

  w_log(W_LOG_DBG, "client_delete %p\n", this);

  if (stm) {
    stm->shutdown();
  }
}

void watchman_client::enqueueResponse(json_ref&& resp, bool ping) {
  responses.emplace_back(std::move(resp));

  if (ping) {
    this->ping->notify();
  }
}

void w_request_shutdown(void) {
  stopping = true;
  // Knock listener thread out of poll/accept
#ifndef _WIN32
  if (listener_thread_event) {
    listener_thread_event->notify();
  }
#else
  SetEvent(listener_thread_event);
#endif
}

// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
static void client_thread(std::shared_ptr<watchman_client> client) {
  struct watchman_event_poll pfd[2];
  json_error_t jerr;
  bool send_ok = true;
  // Keep a persistent vector around so that we can avoid allocating
  // and releasing heap memory when we collect items from the publisher
  std::vector<std::shared_ptr<const watchman::Publisher::Item>> pending;

  client->stm->setNonBlock(true);
  w_set_thread_name(
      "client=%p:stm=%p:pid=%d",
      client.get(),
      client->stm.get(),
      client->stm->getPeerProcessID());

  client->client_is_owner = client->stm->peerIsOwner();

  pfd[0].evt = client->stm->getEvents();
  pfd[1].evt = client->ping.get();

  while (!stopping) {
    // Wait for input from either the client socket or
    // via the ping pipe, which signals that some other
    // thread wants to unilaterally send data to the client

    ignore_result(w_poll_events(pfd, 2, 2000));
    if (stopping) {
      break;
    }

    if (pfd[0].ready) {
      auto request = client->reader.decodeNext(client->stm.get(), &jerr);

      if (!request && errno == EAGAIN) {
        // That's fine
      } else if (!request) {
        // Not so cool
        if (client->reader.wpos == client->reader.rpos) {
          // If they disconnected in between PDUs, no need to log
          // any error
          goto disconnected;
        }
        send_error_response(
            client.get(),
            "invalid json at position %d: %s",
            jerr.position,
            jerr.text);
        w_log(W_LOG_ERR, "invalid data from client: %s\n", jerr.text);

        goto disconnected;
      } else if (request) {
        client->pdu_type = client->reader.pdu_type;
        client->capabilities = client->reader.capabilities;
        dispatch_command(client.get(), request, CMD_DAEMON);
      }
    }

    if (pfd[1].ready) {
      while (client->ping->testAndClear()) {
        // Enqueue refs to pending log payloads
        pending.clear();
        getPending(pending, client->debugSub, client->errorSub);
        for (auto& item : pending) {
          client->enqueueResponse(json_ref(item->payload), false);
        }

        // Maybe we have subscriptions to dispatch?
        auto userClient =
            std::dynamic_pointer_cast<watchman_user_client>(client);

        if (userClient) {
          std::vector<w_string> subsToDelete;
          for (auto& subiter : userClient->unilateralSub) {
            auto sub = subiter.first;
            auto subStream = subiter.second;

            watchman::log(
                watchman::DBG, "consider fan out sub ", sub->name, "\n");

            pending.clear();
            subStream->getPending(pending);
            bool seenSettle = false;
            for (auto& item : pending) {
              auto dumped = json_dumps(item->payload, 0);
              watchman::log(
                  watchman::DBG,
                  "Unilateral payload for sub ",
                  sub->name,
                  " ",
                  dumped ? dumped : "<<MISSING!!>>",
                  "\n");
              free(dumped);

              if (item->payload.get_default("canceled")) {
                auto resp = make_response();

                watchman::log(
                    watchman::ERR,
                    "Cancel subscription ",
                    sub->name,
                    " due to root cancellation\n");

                resp.set({{"root", item->payload.get_default("root")},
                          {"unilateral", json_true()},
                          {"canceled", json_true()},
                          {"subscription", w_string_to_json(sub->name)}});
                client->enqueueResponse(std::move(resp), false);
                // Remember to cancel this subscription.
                // We can't do it in this loop because that would
                // invalidate the iterators and cause a headache.
                subsToDelete.push_back(sub->name);
                continue;
              }

              if (item->payload.get_default("state-enter") ||
                  item->payload.get_default("state-leave")) {
                auto resp = make_response();
                json_object_update(item->payload, resp);
                resp.set({{"unilateral", json_true()},
                          {"subscription", w_string_to_json(sub->name)}});
                client->enqueueResponse(std::move(resp), false);

                watchman::log(
                    watchman::DBG,
                    "Fan out subscription state change for ",
                    sub->name,
                    "\n");
                continue;
              }

              if (!sub->debug_paused && item->payload.get_default("settled")) {
                seenSettle = true;
                continue;
              }
            }

            if (seenSettle) {
              sub->processSubscription();
            }
          }

          for (auto& name : subsToDelete) {
            userClient->unsubByName(name);
          }
        }
      }
    }

    /* now send our response(s) */
    while (!client->responses.empty()) {
      auto& response_to_send = client->responses.front();

      if (send_ok) {
        client->stm->setNonBlock(false);
        /* Return the data in the same format that was used to ask for it.
         * Don't bother sending any more messages if the client disconnects,
         * but still free their memory.
         */
        send_ok = client->writer.pduEncodeToStream(
            client->pdu_type,
            client->capabilities,
            response_to_send,
            client->stm.get());
        client->stm->setNonBlock(true);
      }

      client->responses.pop_front();
    }
  }

disconnected:
  w_set_thread_name(
      "NOT_CONN:client=%p:stm=%p:pid=%d",
      client.get(),
      client->stm.get(),
      client->stm->getPeerProcessID());
  // Remove the client from the map before we tear it down, as this makes
  // it easier to flush out pending writes on windows without worrying
  // about w_log_to_clients contending for the write buffers
  clients.wlock()->erase(client);
}

// This is just a placeholder.
// This catches SIGUSR1 so we don't terminate.
// We use this to interrupt blocking syscalls
// on the worker threads
static void wakeme(int) {}

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
void w_listener_prep_inetd() {
  if (listener_fd) {
    throw std::runtime_error(
        "w_listener_prep_inetd: listener_fd is already assigned");
  }

  listener_fd = FileDescriptor(dup(STDIN_FILENO), "dup(stdin) for listener");
}

static FileDescriptor get_listener_socket(const char *path)
{
  struct sockaddr_un un;
  mode_t perms = cfg_get_perms(
      "sock_access", true /* write bits */, false /* execute bits */);
  FileDescriptor listener_fd;

#ifdef __APPLE__
  listener_fd = w_get_listener_socket_from_launchd();
  if (listener_fd) {
    w_log(W_LOG_ERR, "Using socket from launchd as listening socket\n");
    return listener_fd;
  }
#endif

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "%s: path is too long\n",
        path);
    return FileDescriptor();
  }

  listener_fd = FileDescriptor(socket(PF_LOCAL, SOCK_STREAM, 0), "socket");

  un.sun_family = PF_LOCAL;
  memcpy(un.sun_path, path, strlen(path) + 1);
  unlink(path);

  if (bind(listener_fd.fd(), (struct sockaddr*)&un, sizeof(un)) != 0) {
    w_log(W_LOG_ERR, "bind(%s): %s\n",
      path, strerror(errno));
    return FileDescriptor();
  }

  // The permissions in the containing directory should be correct, so this
  // should be correct as well. But set the permissions in any case.
  if (chmod(path, perms) == -1) {
    w_log(W_LOG_ERR, "chmod(%s, %#o): %s", path, perms, strerror(errno));
    return FileDescriptor();
  }

  // Double-check that the socket has the right permissions. This can happen
  // when the containing directory was created in a previous run, with a group
  // the user is no longer in.
  struct stat st;
  if (lstat(path, &st) == -1) {
    watchman::log(watchman::ERR, "lstat(", path, "): ", strerror(errno), "\n");
    return FileDescriptor();
  }

  // This is for testing only
  // (test_sock_perms.py:test_user_previously_in_sock_group). Do not document.
  const char *sock_group_name = cfg_get_string("__sock_file_group", nullptr);
  if (!sock_group_name) {
    sock_group_name = cfg_get_string("sock_group", nullptr);
  }

  if (sock_group_name) {
    const struct group *sock_group = w_get_group(sock_group_name);
    if (!sock_group) {
      return FileDescriptor();
    }
    if (st.st_gid != sock_group->gr_gid) {
      watchman::log(
        watchman::ERR,
        "for socket '", path, "', gid ", st.st_gid,
        " doesn't match expected gid ", sock_group->gr_gid, " (group name ",
        sock_group_name, "). Ensure that you are still a member of group ",
        sock_group_name, ".\n");
      return FileDescriptor();
    }
  }

  if (listen(listener_fd.fd(), 200) != 0) {
    w_log(W_LOG_ERR, "listen(%s): %s\n",
        path, strerror(errno));
    return FileDescriptor();
  }

  return listener_fd;
}
#endif

static std::shared_ptr<watchman_client> make_new_client(
    std::unique_ptr<watchman_stream>&& stm) {
  auto client = std::make_shared<watchman_user_client>(std::move(stm));

  clients.wlock()->insert(client);

  // Start a thread for the client.
  // We used to use libevent for this, but we have
  // a low volume of concurrent clients and the json
  // parse/encode APIs are not easily used in a non-blocking
  // server architecture.
  try {
    std::thread thr([client]() { client_thread(client); });
    thr.detach();
  } catch (const std::exception& e) {
    clients.wlock()->erase(client);
    throw;
  }

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
    FileDescriptor client_fd;
    DWORD res;

    client_fd = FileDescriptor(intptr_t(CreateNamedPipe(
        path,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        WATCHMAN_IO_BUF_SIZE,
        512,
        0,
        nullptr)));

    if (!client_fd) {
      w_log(W_LOG_ERR, "CreateNamedPipe(%s) failed: %s\n",
          path, win32_strerror(GetLastError()));
      continue;
    }

    ResetEvent(connected_event);
    if (!ConnectNamedPipe((HANDLE)client_fd.handle(), &olap)) {
      res = GetLastError();

      if (res == ERROR_PIPE_CONNECTED) {
        make_new_client(w_stm_fdopen(std::move(client_fd)));
        continue;
      }

      if (res != ERROR_IO_PENDING) {
        w_log(W_LOG_ERR, "ConnectNamedPipe: %s\n",
            win32_strerror(GetLastError()));
        continue;
      }

      res = WaitForMultipleObjectsEx(2, handles, false, INFINITE, true);
      if (res == WAIT_OBJECT_0 + 1) {
        // Signalled to stop
        CancelIoEx((HANDLE)client_fd.handle(), &olap);
        continue;
      }

      if (res != WAIT_OBJECT_0) {
        w_log(
            W_LOG_ERR,
            "WaitForMultipleObjectsEx: ConnectNamedPipe: "
            "unexpected status %u\n",
            res);
        CancelIoEx((HANDLE)client_fd.handle(), &olap);
        continue;
      }
    }
    make_new_client(w_stm_fdopen(std::move(client_fd)));
  }
}
#endif

#ifndef _WIN32
static void accept_loop(FileDescriptor&& listenerDescriptor) {
  auto listener = w_stm_fdopen(std::move(listenerDescriptor));
  while (!stopping) {
    FileDescriptor client_fd;
    struct watchman_event_poll pfd[2];
    int bufsize;

#ifdef HAVE_LIBGIMLI_H
    if (hb) {
      gimli_heartbeat_set(hb, GIMLI_HB_RUNNING);
    }
#endif

    pfd[0].evt = listener->getEvents();
    pfd[1].evt = listener_thread_event.get();

    if (w_poll_events(pfd, 2, 60000) == 0) {
      if (stopping) {
        break;
      }
      // Timed out, or error.
      // Arrange to sanity check that we're working
      w_check_my_sock();
      continue;
    }

    if (stopping) {
      break;
    }

#ifdef HAVE_ACCEPT4
    client_fd = FileDescriptor(
        accept4(listener->getFileDescriptor().fd(), nullptr, 0, SOCK_CLOEXEC));
#else
    client_fd =
        FileDescriptor(accept(listener->getFileDescriptor().fd(), nullptr, 0));
#endif
    if (!client_fd) {
      continue;
    }
    client_fd.setCloExec();
    bufsize = WATCHMAN_IO_BUF_SIZE;
    setsockopt(
        client_fd.fd(),
        SOL_SOCKET,
        SO_SNDBUF,
        (void*)&bufsize,
        sizeof(bufsize));

    make_new_client(w_stm_fdopen(std::move(client_fd)));
  }
}
#endif

bool w_start_listener(const char *path)
{
#ifndef _WIN32
  struct sigaction sa;
  sigset_t sigset;
#endif

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

  if (listener_fd) {
    // Assume that it was prepped by w_listener_prep_inetd()
    w_log(W_LOG_ERR, "Using socket from inetd as listening socket\n");
  } else {
    listener_fd = get_listener_socket(path);
    if (!listener_fd) {
      return false;
    }
  }
  listener_fd.setCloExec();
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
  listener_fd.setNonBlock();

  // Now run the dispatch
#ifndef _WIN32
  listener_thread_event = w_event_make();
  accept_loop(std::move(listener_fd));
#else
  named_pipe_accept_loop(path);
#endif

  // Wait for clients, waking any sleeping clients up in the process
  {
    int interval = 2000;
    int last_count = 0, n_clients = 0;
    const int max_interval = 1000000; // 1 second

    do {
      {
        auto clientsLock = clients.rlock();
        n_clients = clientsLock->size();

        for (auto client : *clientsLock) {
          client->ping->notify();
        }
      }

      if (n_clients != last_count) {
        w_log(W_LOG_ERR, "waiting for %d clients to terminate\n", n_clients);
      }
      usleep(interval);
      interval = std::min(interval * 2, max_interval);
    } while (n_clients > 0);
  }

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
