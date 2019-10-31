/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <folly/ScopeGuard.h>

using namespace watchman;

namespace {
struct reg {
  std::unordered_map<w_string, watchman_command_handler_def*> commands;
  std::unordered_set<w_string> capabilities;

  reg() {
    commands.reserve(16);
    capabilities.reserve(128);
  }
};

// Meyers singleton to avoid SIOF problems
reg& get_reg() {
  static struct reg reg;
  return reg;
}
} // namespace

/* Some error conditions will put us into a non-recoverable state where we
 * can't guarantee that we will be operating correctly.  Rather than suffering
 * in silence and misleading our clients, we'll poison ourselves and advertise
 * that we have done so and provide some advice on how the user can cure us. */
folly::Synchronized<std::string> poisoned_reason;

void print_command_list_for_help(FILE* where) {
  std::vector<watchman_command_handler_def*> defs;

  for (auto& it : get_reg().commands) {
    defs.emplace_back(it.second);
  }

  std::sort(
      defs.begin(),
      defs.end(),
      [](watchman_command_handler_def* A, watchman_command_handler_def* B) {
        return strcmp(A->name, B->name) < 0;
      });

  fprintf(where, "\n\nAvailable commands:\n\n");
  for (auto& def : defs) {
    fprintf(where, "      %s\n", def->name);
  }
}

void w_register_command(struct watchman_command_handler_def* defs) {
  char capname[128];

  get_reg().commands[w_string(defs->name, W_STRING_UNICODE)] = defs;

  snprintf(capname, sizeof(capname), "cmd-%s", defs->name);
  w_capability_register(capname);
}

static struct watchman_command_handler_def* lookup(
    const json_ref& args,
    int mode) {
  const char* cmd_name;

  if (!json_array_size(args)) {
    throw CommandValidationError(
        "invalid command (expected an array with some elements!)");
  }

  const auto jstr = json_array_get(args, 0);
  cmd_name = json_string_value(jstr);
  if (!cmd_name) {
    throw CommandValidationError(
        "invalid command: expected element 0 to be the command name");
  }
  auto cmd = json_to_w_string(jstr);
  auto it = get_reg().commands.find(cmd);
  auto def = it == get_reg().commands.end() ? nullptr : it->second;

  if (def) {
    if (mode && ((def->flags & mode) == 0)) {
      throw CommandValidationError(
          "command ", cmd_name, " not available in this mode");
    }
    return def;
  }

  if (mode) {
    throw CommandValidationError("unknown command ", cmd_name);
  }

  return nullptr;
}

void preprocess_command(
    json_ref& args,
    enum w_pdu_type output_pdu,
    uint32_t output_capabilities) {
  struct watchman_command_handler_def* def;

  try {
    def = lookup(args, 0);

    if (!def) {
      // Nothing known about it, pass the command on anyway for forwards
      // compatibility
      return;
    }

    if (def->cli_validate) {
      def->cli_validate(args);
    }
  } catch (const std::exception& exc) {
    w_jbuffer_t jr;

    auto err = json_object(
        {{"error", typed_string_to_json(exc.what(), W_STRING_MIXED)},
         {"version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE)},
         {"cli_validated", json_true()}});

    jr.pduEncodeToStream(output_pdu, output_capabilities, err, w_stm_stdout());
    exit(1);
  }
}

bool dispatch_command(
    struct watchman_client* client,
    const json_ref& args,
    int mode) {
  struct watchman_command_handler_def* def;
  char sample_name[128];

  try {
    // Stash a reference to the current command to make it easier to log
    // the command context in some of the error paths
    client->current_command = args;
    SCOPE_EXIT {
      client->current_command = nullptr;
    };

    def = lookup(args, mode);
    if (!def) {
      send_error_response(client, "Unknown command");
      return false;
    }

    if (!poisoned_reason.rlock()->empty() &&
        (def->flags & CMD_POISON_IMMUNE) == 0) {
      send_error_response(client, "%s", poisoned_reason.rlock()->c_str());
      return false;
    }

    if (!client->client_is_owner && (def->flags & CMD_ALLOW_ANY_USER) == 0) {
      send_error_response(
          client, "you must be the process owner to execute '%s'", def->name);
      return false;
    }

    // Scope for the perf sample
    {
      logf(DBG, "dispatch_command: {}\n", def->name);
      snprintf(
          sample_name, sizeof(sample_name), "dispatch_command:%s", def->name);
      w_perf_t sample(sample_name);
      client->perf_sample = &sample;
      SCOPE_EXIT {
        client->perf_sample = nullptr;
      };

      sample.set_wall_time_thresh(
          cfg_get_double("slow_command_log_threshold_seconds", 1.0));

      def->func(client, args);

      if (sample.finish()) {
        sample.add_meta("args", json_ref(args));
        sample.add_meta(
            "client",
            json_object(
                {{"pid", json_integer(client->stm->getPeerProcessID())}}));
        sample.log();
      } else {
        logf(DBG, "dispatch_command: {} (completed)\n", def->name);
      }
    }

    return true;
  } catch (const std::exception& e) {
    send_error_response(client, "%s", e.what());
    return false;
  }
}

void w_capability_register(const char* name) {
  get_reg().capabilities.insert(w_string(name, W_STRING_UNICODE));
}

bool w_capability_supported(const w_string& name) {
  return get_reg().capabilities.find(name) != get_reg().capabilities.end();
}

json_ref w_capability_get_list(void) {
  auto arr = json_array_of_size(get_reg().capabilities.size());

  for (auto& name : get_reg().capabilities) {
    json_array_append(arr, w_string_to_json(name));
  }

  return arr;
}

/* vim:ts=2:sw=2:et:
 */
