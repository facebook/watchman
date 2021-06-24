/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/Exception.h>
#include <folly/MapUtil.h>
#include <folly/Optional.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/Synchronized.h>
#include <folly/net/NetworkSocket.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "SignalHandler.h"
#include "watchman/watchman.h"

using namespace watchman;

folly::Synchronized<std::unordered_set<std::shared_ptr<watchman_client>>>
    clients;
static FileDescriptor listener_fd;
static std::vector<std::shared_ptr<watchman_event>> listener_thread_events;
static std::atomic<bool> stopping = false;
static constexpr size_t kResponseLogLimit = 8;

bool w_is_stopping() {
  return stopping.load(std::memory_order_relaxed);
}

json_ref make_response() {
  auto resp = json_object();

  resp.set("version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE));

  return resp;
}

void send_and_dispose_response(
    struct watchman_client* client,
    json_ref&& response) {
  client->enqueueResponse(std::move(response), false);
}

void send_error_response(struct watchman_client* client, const char* fmt, ...) {
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
    auto command = json_dumps(client->current_command, 0);
    watchman::log(
        watchman::ERR,
        "send_error_response: ",
        command,
        ", failed: ",
        errorText,
        "\n");
  } else {
    watchman::log(watchman::ERR, "send_error_response: ", errorText, "\n");
  }

  send_and_dispose_response(client, std::move(resp));
}

namespace {
// TODO: If used in a hot loop, EdenFS has a faster implementation.
// https://github.com/facebookexperimental/eden/blob/c745d644d969dae1e4c0d184c19320fac7c27ae5/eden/fs/utils/IDGen.h
std::atomic<uint64_t> id_generator{1};
} // namespace

watchman_client::watchman_client() : watchman_client(nullptr) {}

watchman_client::watchman_client(std::unique_ptr<watchman_stream>&& stm)
    : unique_id{id_generator++},
      stm(std::move(stm)),
      ping(
#ifdef _WIN32
          (this->stm &&
           this->stm->getFileDescriptor().fdType() ==
               FileDescriptor::FDType::Socket)
              ? w_event_make_sockets()
              : w_event_make_named_pipe()
#else
          w_event_make_sockets()
#endif

      ) {
  logf(DBG, "accepted client:stm={}\n", fmt::ptr(this->stm.get()));
}

watchman_client::~watchman_client() {
  debugSub.reset();
  errorSub.reset();

  logf(DBG, "client_delete {}\n", unique_id);

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

void w_request_shutdown() {
  stopping.store(true, std::memory_order_relaxed);
  // Knock listener thread out of poll/accept
  for (auto& evt : listener_thread_events) {
    evt->notify();
  }
}

// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
static void client_thread(
    std::shared_ptr<watchman_user_client> client) noexcept {
  // Keep a persistent vector around so that we can avoid allocating
  // and releasing heap memory when we collect items from the publisher
  std::vector<std::shared_ptr<const watchman::Publisher::Item>> pending;

  client->stm->setNonBlock(true);
  w_set_thread_name(
      "client=",
      client->unique_id,
      ":stm=",
      uintptr_t(client->stm.get()),
      ":pid=",
      client->stm->getPeerProcessID());

  client->client_is_owner = client->stm->peerIsOwner();

  struct watchman_event_poll pfd[2];
  pfd[0].evt = client->stm->getEvents();
  pfd[1].evt = client->ping.get();

  bool client_alive = true;
  while (!w_is_stopping() && client_alive) {
    // Wait for input from either the client socket or
    // via the ping pipe, which signals that some other
    // thread wants to unilaterally send data to the client

    ignore_result(w_poll_events(pfd, 2, 2000));
    if (w_is_stopping()) {
      break;
    }

    if (pfd[0].ready) {
      json_error_t jerr;
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
        logf(ERR, "invalid data from client: {}\n", jerr.text);

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
        std::vector<w_string> subsToDelete;
        for (auto& subiter : client->unilateralSub) {
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
                dumped,
                "\n");

            if (item->payload.get_default("canceled")) {
              watchman::log(
                  watchman::ERR,
                  "Cancel subscription ",
                  sub->name,
                  " due to root cancellation\n");

              auto resp = make_response();
              resp.set(
                  {{"root", item->payload.get_default("root")},
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
              // We have the opportunity to populate additional response
              // fields here (since we don't want to block the command).
              // We don't populate the fat clock for SCM aware queries
              // because determination of mergeBase could add latency.
              resp.set(
                  {{"unilateral", json_true()},
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
          client->unsubByName(name);
        }
      }
    }

    /* now send our response(s) */
    while (!client->responses.empty() && client_alive) {
      auto& response_to_send = client->responses.front();

      client->stm->setNonBlock(false);
      /* Return the data in the same format that was used to ask for it.
       * Update client liveness based on send success.
       */
      client_alive = client->writer.pduEncodeToStream(
          client->pdu_type,
          client->capabilities,
          response_to_send,
          client->stm.get());
      client->stm->setNonBlock(true);

      json_ref subscriptionValue = response_to_send.get_default("subscription");
      if (subscriptionValue && subscriptionValue.isString() &&
          json_string_value(subscriptionValue)) {
        auto subscriptionName = json_to_w_string(subscriptionValue);
        if (auto* sub =
                folly::get_ptr(client->subscriptions, subscriptionName)) {
          if ((*sub)->lastResponses.size() >= kResponseLogLimit) {
            (*sub)->lastResponses.pop_front();
          }
          (*sub)->lastResponses.push_back(
              watchman_client_subscription::LoggedResponse{
                  std::chrono::system_clock::now(), response_to_send});
        }
      }

      client->responses.pop_front();
    }
  }

disconnected:
  w_set_thread_name(
      "NOT_CONN:client=",
      client->unique_id,
      ":stm=",
      uintptr_t(client->stm.get()),
      ":pid=",
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
#include <sys/siginfo.h> // @manual
#endif
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
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

  listener_fd = FileDescriptor(
      dup(STDIN_FILENO),
      "dup(stdin) for listener",
      // It's probably a socket but we don't know for sure
      FileDescriptor::FDType::Unknown);
}

#endif

static FileDescriptor get_listener_tcp_socket() {
  FileDescriptor listener_fd;

  folly::SocketAddress addr;
  addr.setFromHostPort(
      Configuration().getString("tcp-listener-address", nullptr));

  listener_fd = FileDescriptor(
      ::socket(addr.getFamily(), SOCK_STREAM, 0),
      "socket() for TCP socket",
      FileDescriptor::FDType::Socket);

  int one = 1;
  ::setsockopt(
      listener_fd.system_handle(),
      SOL_SOCKET,
      SO_REUSEADDR,
      (char*)&one,
      sizeof(one));
  // Most of our TCP transactions are quite small, so this seems appropriate
  ::setsockopt(
      listener_fd.system_handle(),
      IPPROTO_TCP,
      TCP_NODELAY,
      (char*)&one,
      sizeof(one));

  sockaddr_storage storage;
  auto addrLen = addr.getAddress(&storage);

  folly::checkUnixError(
      ::bind(listener_fd.system_handle(), (struct sockaddr*)&storage, addrLen),
      "bind to ",
      addr.getAddressStr(),
      "failed");

  folly::checkUnixError(
      ::listen(listener_fd.system_handle(), 200),
      "listen on ",
      addr.getAddressStr(),
      "failed");

  addr.setFromLocalAddress(folly::NetworkSocket(listener_fd.system_handle()));
  log(ERR, "Started TCP listener on ", addr.describe(), "\n");

  return listener_fd;
}

static FileDescriptor get_listener_unix_domain_socket(const char* path) {
  struct sockaddr_un un {};

#ifndef _WIN32
  mode_t perms = cfg_get_perms(
      "sock_access", true /* write bits */, false /* execute bits */);
#endif
  FileDescriptor listener_fd;

#ifdef __APPLE__
  listener_fd = w_get_listener_socket_from_launchd();
  if (listener_fd) {
    logf(ERR, "Using socket from launchd as listening socket\n");
    return listener_fd;
  }
#endif

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    logf(ERR, "{}: path is too long\n", path);
    return FileDescriptor();
  }

  listener_fd = FileDescriptor(
      ::socket(PF_LOCAL, SOCK_STREAM, 0),
      "socket",
      FileDescriptor::FDType::Socket);

  un.sun_family = PF_LOCAL;
  memcpy(un.sun_path, path, strlen(path) + 1);
  unlink(path);

  if (::bind(listener_fd.system_handle(), (struct sockaddr*)&un, sizeof(un)) !=
      0) {
    logf(ERR, "bind({}): {}\n", path, folly::errnoStr(errno));
    return FileDescriptor();
  }

#ifndef _WIN32
  // The permissions in the containing directory should be correct, so this
  // should be correct as well. But set the permissions in any case.
  if (chmod(path, perms) == -1) {
    logf(ERR, "chmod({}, {:o}): {}", path, perms, folly::errnoStr(errno));
    return FileDescriptor();
  }

  // Double-check that the socket has the right permissions. This can happen
  // when the containing directory was created in a previous run, with a group
  // the user is no longer in.
  struct stat st;
  if (lstat(path, &st) == -1) {
    watchman::log(
        watchman::ERR, "lstat(", path, "): ", folly::errnoStr(errno), "\n");
    return FileDescriptor();
  }

  // This is for testing only
  // (test_sock_perms.py:test_user_previously_in_sock_group). Do not document.
  const char* sock_group_name = cfg_get_string("__sock_file_group", nullptr);
  if (!sock_group_name) {
    sock_group_name = cfg_get_string("sock_group", nullptr);
  }

  if (sock_group_name) {
    const struct group* sock_group = w_get_group(sock_group_name);
    if (!sock_group) {
      return FileDescriptor();
    }
    if (st.st_gid != sock_group->gr_gid) {
      watchman::log(
          watchman::ERR,
          "for socket '",
          path,
          "', gid ",
          st.st_gid,
          " doesn't match expected gid ",
          sock_group->gr_gid,
          " (group name ",
          sock_group_name,
          "). Ensure that you are still a member of group ",
          sock_group_name,
          ".\n");
      return FileDescriptor();
    }
  }
#endif

  if (::listen(listener_fd.system_handle(), 200) != 0) {
    logf(ERR, "listen({}): {}\n", path, folly::errnoStr(errno));
    return FileDescriptor();
  }

  return listener_fd;
}

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
    std::thread thr([client] { client_thread(client); });

    thr.detach();
  } catch (const std::exception&) {
    clients.wlock()->erase(client);
    throw;
  }

  return client;
}

#ifdef _WIN32

static FileDescriptor create_pipe_server(const char* path) {
  return FileDescriptor(
      intptr_t(CreateNamedPipe(
          path,
          PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
          PIPE_UNLIMITED_INSTANCES,
          WATCHMAN_IO_BUF_SIZE,
          512,
          0,
          nullptr)),
      FileDescriptor::FDType::Pipe);
}

static void named_pipe_accept_loop_internal(
    std::shared_ptr<watchman_event> listener_event) {
  HANDLE handles[2];
  auto olap = OVERLAPPED();
  HANDLE connected_event = CreateEvent(NULL, FALSE, TRUE, NULL);
  auto path = get_named_pipe_sock_path();

  if (!connected_event) {
    logf(
        ERR,
        "named_pipe_accept_loop_internal: CreateEvent failed: {}\n",
        win32_strerror(GetLastError()));
    return;
  }

  handles[0] = connected_event;
  handles[1] = (HANDLE)listener_event->system_handle();
  olap.hEvent = connected_event;

  logf(ERR, "waiting for pipe clients on {}\n", get_named_pipe_sock_path());
  while (!w_is_stopping()) {
    FileDescriptor client_fd;
    DWORD res;

    client_fd = create_pipe_server(path.c_str());
    if (!client_fd) {
      logf(
          ERR,
          "CreateNamedPipe(%s) failed: %s\n",
          path,
          win32_strerror(GetLastError()));
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
        logf(ERR, "ConnectNamedPipe: {}\n", win32_strerror(GetLastError()));
        continue;
      }

      res = WaitForMultipleObjectsEx(2, handles, false, INFINITE, true);
      if (res == WAIT_OBJECT_0 + 1) {
        // Signalled to stop
        CancelIoEx((HANDLE)client_fd.handle(), &olap);
        continue;
      }

      if (res != WAIT_OBJECT_0) {
        logf(
            ERR,
            "WaitForMultipleObjectsEx: ConnectNamedPipe: "
            "unexpected status {}\n",
            res);
        CancelIoEx((HANDLE)client_fd.handle(), &olap);
        continue;
      }
    }
    make_new_client(w_stm_fdopen(std::move(client_fd)));
  }
  logf(ERR, "is_stopping is true, so acceptor is done\n");
}

static void named_pipe_accept_loop() {
  log(DBG, "Starting pipe listener on ", get_named_pipe_sock_path(), "\n");

  std::vector<std::thread> acceptors;
  std::shared_ptr<watchman_event> listener_event(w_event_make_named_pipe());

  listener_thread_events.push_back(listener_event);

  for (json_int_t i = 0; i < cfg_get_int("win32_concurrent_accepts", 32); ++i) {
    acceptors.push_back(std::thread([i, listener_event]() {
      w_set_thread_name("accept", i);
      named_pipe_accept_loop_internal(listener_event);
    }));
  }
  for (auto& thr : acceptors) {
    thr.join();
  }
}
#endif

/** A helper for owning and running a socket-style (rather than
 * named pipe style) accept loop that runs in another thread.
 */
class AcceptLoop {
  std::thread thread_;
  bool joined_{false};

  static void accept_thread(
      FileDescriptor&& listenerDescriptor,
      std::shared_ptr<watchman_event> listener_event) {
    auto listener = w_stm_fdopen(std::move(listenerDescriptor));
    while (!w_is_stopping()) {
      FileDescriptor client_fd;
      struct watchman_event_poll pfd[2];
      int bufsize;

      pfd[0].evt = listener->getEvents();
      pfd[1].evt = listener_event.get();

      if (w_poll_events(pfd, 2, 60000) == 0) {
        if (w_is_stopping()) {
          break;
        }
        // Timed out, or error.
        continue;
      }

      if (w_is_stopping()) {
        break;
      }

#ifdef HAVE_ACCEPT4
      client_fd = FileDescriptor(
          accept4(
              listener->getFileDescriptor().system_handle(),
              nullptr,
              0,
              SOCK_CLOEXEC),
          FileDescriptor::FDType::Socket);
#else
      client_fd = FileDescriptor(
          ::accept(listener->getFileDescriptor().system_handle(), nullptr, 0),
          FileDescriptor::FDType::Socket);
#endif
      if (!client_fd) {
        continue;
      }
      client_fd.setCloExec();
      bufsize = WATCHMAN_IO_BUF_SIZE;
      ::setsockopt(
          client_fd.system_handle(),
          SOL_SOCKET,
          SO_SNDBUF,
          (char*)&bufsize,
          sizeof(bufsize));

      make_new_client(w_stm_fdopen(std::move(client_fd)));
    }
  }

 public:
  /** Start an accept loop thread using the provided socket
   * descriptor (`fd`).  The `name` parameter is used to name the
   * thread */
  AcceptLoop(std::string name, FileDescriptor&& fd) {
    fd.setCloExec();
    fd.setNonBlock();

    std::shared_ptr<watchman_event> listener_event(w_event_make_sockets());
    listener_thread_events.push_back(listener_event);

    thread_ = std::thread(
        [listener_fd = std::move(fd), name, listener_event]() mutable {
          w_set_thread_name(name);
          accept_thread(std::move(listener_fd), listener_event);
        });
  }

  AcceptLoop(const AcceptLoop&) = delete;
  AcceptLoop& operator=(const AcceptLoop&) = delete;

  AcceptLoop(AcceptLoop&& other) {
    *this = std::move(other);
  }

  AcceptLoop& operator=(AcceptLoop&& other) {
    thread_ = std::move(other.thread_);
    joined_ = other.joined_;
    // Ensure that we don't try to join the source,
    // as std::thread::join will std::terminate in that case.
    // If it weren't for this we could use the compiler
    // default implementation of move.
    other.joined_ = true;
    return *this;
  }

  ~AcceptLoop() {
    join();
  }

  void join() {
    if (joined_) {
      return;
    }
    thread_.join();
    joined_ = true;
  }
};

bool w_start_listener() {
#ifndef _WIN32
  struct sigaction sa;
  sigset_t sigset;
#endif

#if defined(HAVE_KQUEUE) || defined(HAVE_FSEVENTS)
  {
    struct rlimit limit;
#ifndef __OpenBSD__
    int mib[2] = {
        CTL_KERN,
#ifdef KERN_MAXFILESPERPROC
        KERN_MAXFILESPERPROC
#else
        KERN_MAXFILES
#endif
    };
#endif
    int maxperproc;

    getrlimit(RLIMIT_NOFILE, &limit);

#ifndef __OpenBSD__
    {
      size_t len;

      len = sizeof(maxperproc);
      sysctl(mib, 2, &maxperproc, &len, NULL, 0);
      logf(
          ERR,
          "file limit is {} kern.maxfilesperproc={}\n",
          limit.rlim_cur,
          maxperproc);
    }
#else
    maxperproc = limit.rlim_max;
    logf(
        ERR,
        "openfiles-cur is {} openfiles-max={}\n",
        limit.rlim_cur,
        maxperproc);
#endif

    if (limit.rlim_cur != RLIM_INFINITY && maxperproc > 0 &&
        limit.rlim_cur < (rlim_t)maxperproc) {
      limit.rlim_cur = maxperproc;

      if (setrlimit(RLIMIT_NOFILE, &limit)) {
        logf(
            ERR,
            "failed to raise limit to {} ({}).\n",
            limit.rlim_cur,
            folly::errnoStr(errno));
      } else {
        logf(ERR, "raised file limit to {}\n", limit.rlim_cur);
      }
    }

    getrlimit(RLIMIT_NOFILE, &limit);
#ifndef HAVE_FSEVENTS
    if (limit.rlim_cur < 10240) {
      logf(
          ERR,
          "Your file descriptor limit is very low ({})"
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
#endif
  setup_signal_handlers();

  folly::Optional<AcceptLoop> tcp_loop;
  folly::Optional<AcceptLoop> unix_loop;

  // When we unwind, ensure that we stop the accept threads
  SCOPE_EXIT {
    if (!w_is_stopping()) {
      w_request_shutdown();
    }
    unix_loop.clear();
    tcp_loop.clear();
  };

  if (listener_fd) {
    // Assume that it was prepped by w_listener_prep_inetd()
    logf(ERR, "Using socket from inetd as listening socket\n");
  } else {
    listener_fd = get_listener_unix_domain_socket(get_unix_sock_name().c_str());
    if (!listener_fd) {
      logf(ERR, "Failed to initialize unix domain listener\n");
      return false;
    }
  }

  if (listener_fd && !disable_unix_socket) {
    unix_loop.assign(AcceptLoop("unix-listener", std::move(listener_fd)));
  }

  if (Configuration().getBool("tcp-listener-enable", false)) {
    tcp_loop.assign(AcceptLoop("tcp-listener", get_listener_tcp_socket()));
  }

  startSanityCheckThread();

#ifdef _WIN32
  // Start the named pipes and join them; this will
  // block until the server is shutdown.
  if (!disable_named_pipe) {
    named_pipe_accept_loop();
  }
#endif

  // Clearing these will cause .join() to be called,
  // so the next two lines will block until the server
  // shutdown is initiated, rather than cause the server
  // to shutdown.
  unix_loop.clear();
  tcp_loop.clear();

  // Wait for clients, waking any sleeping clients up in the process
  {
    auto interval = std::chrono::microseconds(2000);
    const auto max_interval = std::chrono::seconds(1);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);

    size_t last_count = 0, n_clients = 0;

    while (true) {
      {
        auto clientsLock = clients.rlock();
        n_clients = clientsLock->size();

        for (auto client : *clientsLock) {
          client->ping->notify();
        }
      }

      if (n_clients == 0) {
        break;
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        log(ERR, "Abandoning wait for ", n_clients, " outstanding clients\n");
        break;
      }

      if (n_clients != last_count) {
        log(ERR, "waiting for ", n_clients, " clients to terminate\n");
      }

      /* sleep override */
      std::this_thread::sleep_for(interval);
      interval *= 2;
      if (interval > max_interval) {
        interval = max_interval;
      }
    }
  }

  w_state_shutdown();

  return true;
}

/* get-pid */
static void cmd_get_pid(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();

  resp.set("pid", json_integer(::getpid()));

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("get-pid", cmd_get_pid, CMD_DAEMON, NULL)

/* vim:ts=2:sw=2:et:
 */
