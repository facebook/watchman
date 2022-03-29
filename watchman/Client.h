/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <deque>
#include <unordered_map>
#include "watchman/Clock.h"
#include "watchman/Logging.h"
#include "watchman/PDU.h"
#include "watchman/PerfSample.h"
#include "watchman/watchman_preprocessor.h"
#include "watchman/watchman_stream.h"

namespace watchman {

class ClientStateAssertion;
class Root;
struct Query;
struct QueryResult;

class Client : public std::enable_shared_from_this<Client> {
 public:
  Client();
  explicit Client(std::unique_ptr<watchman_stream> stm);
  virtual ~Client();

  void enqueueResponse(json_ref resp);

  void sendErrorResponse(WATCHMAN_FMT_STRING(const char* fmt), ...)
      WATCHMAN_FMT_ATTR(2, 3);

  const uint64_t unique_id;
  std::unique_ptr<watchman_stream> stm;
  std::unique_ptr<watchman_event> ping;
  PduBuffer reader;
  PduBuffer writer;
  bool client_mode = false;
  bool client_is_owner = false;
  PduType pdu_type;
  uint32_t capabilities;

  // The command currently being processed by dispatch_command
  json_ref current_command;
  PerfSample* perf_sample{nullptr};

  // Queue of things to send to the client.
  std::deque<json_ref> responses;

  // Logging Subscriptions
  std::shared_ptr<Publisher::Subscriber> debugSub;
  std::shared_ptr<Publisher::Subscriber> errorSub;
};

enum class OnStateTransition { QueryAnyway, DontAdvance };

class UserClient;

class ClientSubscription
    : public std::enable_shared_from_this<ClientSubscription> {
 public:
  explicit ClientSubscription(
      const std::shared_ptr<Root>& root,
      std::weak_ptr<Client> client);
  ~ClientSubscription();

  void processSubscription();

  std::shared_ptr<UserClient> lockClient();
  json_ref buildSubscriptionResults(
      const std::shared_ptr<Root>& root,
      ClockSpec& position,
      OnStateTransition onStateTransition);

 public:
  struct LoggedResponse {
    // TODO: also track the time when the response was enqueued
    std::chrono::system_clock::time_point written;
    json_ref response;
  };

  std::shared_ptr<Root> root;
  w_string name;
  /* whether this subscription is paused */
  bool debug_paused = false;

  std::shared_ptr<Query> query;
  bool vcs_defer;
  uint32_t last_sub_tick{0};
  // map of statename => bool.  If true, policy is drop, else defer
  std::unordered_map<w_string, bool> drop_or_defer;
  std::weak_ptr<Client> weakClient;

  std::deque<LoggedResponse> lastResponses;

 private:
  ClockSpec runSubscriptionRules(
      UserClient* client,
      const std::shared_ptr<Root>& root);
  void updateSubscriptionTicks(QueryResult* res);
  void processSubscriptionImpl();
};

class ClientStatus {
 public:
  enum State {
    /// UserClient is allocated, but its thread is not started.
    THREAD_STARTING,
    /// The client thread has begun.
    THREAD_STARTED,
    /// The client thread is waiting for a request.
    WAITING_FOR_REQUEST,
    /// The client thread is decoding request data.
    DECODING_REQUEST,
    /// The client thread is executing a request.
    DISPATCHING_COMMAND,
    /// The client thread is reading subscription events and processing them.
    PROCESSING_SUBSCRIPTION,
    /// The client thread is sending responses.
    SENDING_SUBSCRIPTION_RESPONSES,
    /// The client thread is shutting down.
    THREAD_STOPPING,
  };

  void transitionTo(State state) {
    state_.store(state, std::memory_order_release);
  }

 private:
  // No locking or CAS required, as the tag is only written by UserClient's
  // constructor and the client thread. There will never be simultaneous state
  // transitions.
  std::atomic<State> state_{THREAD_STARTING};
};

/**
 * Represents the server side session maintained for a client of
 * the watchman per-user process.
 *
 * Each UserClient has a corresponding thread that reads and decodes json
 * packets and dispatches the commands that it finds.
 */
class UserClient final : public Client {
 public:
  static void create(std::unique_ptr<watchman_stream> stm);
  ~UserClient() override;

  static std::vector<std::shared_ptr<UserClient>> getAllClients();

  /* map of subscription name => struct watchman_client_subscription */
  std::unordered_map<w_string, std::shared_ptr<ClientSubscription>>
      subscriptions;

  /* map of state-name => ClientStateAssertion
   * The values are owned by root::assertedStates */
  std::unordered_map<w_string, std::weak_ptr<ClientStateAssertion>> states;

  // Subscriber to root::unilateralResponses
  std::unordered_map<
      std::shared_ptr<ClientSubscription>,
      std::shared_ptr<Publisher::Subscriber>>
      unilateralSub;

  bool unsubByName(const w_string& name);

 private:
  UserClient() = delete;
  UserClient(UserClient&&) = delete;
  UserClient& operator=(UserClient&&) = delete;

  // To allow make_shared to construct UserClient.
  struct PrivateBadge {};

 public: // Public for std::make_shared
  explicit UserClient(PrivateBadge, std::unique_ptr<watchman_stream> stm);

 private:
  // Abandon any states that haven't been explicit vacated.
  void vacateStates();

  void clientThread() noexcept;

  ClientStatus status_;
};

} // namespace watchman

void w_leave_state(
    watchman::UserClient* client,
    std::shared_ptr<watchman::ClientStateAssertion> assertion,
    bool abandoned,
    json_t* metadata);
