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

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
static w_ht_t *clients = NULL;

typedef void (*watchman_command_func)(
    struct watchman_client *client,
    json_t *args);
static w_ht_t *command_funcs = NULL;

static json_t *make_response(void)
{
  json_t *resp = json_object();

  json_object_set_new(resp, "version", json_string(PACKAGE_VERSION));

  return resp;
}

// Renders the current clock id string to the supplied buffer.
// Must be called with the root locked.
static bool current_clock_id_string(w_root_t *root,
    char *buf, size_t bufsize)
{
  int res = snprintf(buf, bufsize, "c:%d:%" PRIu32,
              (int)getpid(), root->ticks);

  if (res == -1) {
    return false;
  }
  return (size_t)res < bufsize;
}

/* Add the current clock value to the response.
 * must be called with the root locked */
static void annotate_with_clock(w_root_t *root, json_t *resp)
{
  char buf[128];

  if (current_clock_id_string(root, buf, sizeof(buf))) {
    json_object_set_new(resp, "clock", json_string(buf));
  }
}

/* must be called with the client_lock held */
static bool enqueue_response(struct watchman_client *client,
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
    ignore_result(write(client->ping[1], "a", 1));
  }

  return true;
}

static void send_and_dispose_response(struct watchman_client *client,
    json_t *response)
{
  pthread_mutex_lock(&client_lock);
  if (!enqueue_response(client, response, false)) {
    json_decref(response);
  }
  pthread_mutex_unlock(&client_lock);
}

static void send_error_response(struct watchman_client *client,
    const char *fmt, ...)
{
  char buf[WATCHMAN_NAME_MAX];
  va_list ap;
  json_t *resp = make_response();

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  json_object_set_new(resp, "error", json_string(buf));

  send_and_dispose_response(client, resp);
}

static void client_delete(w_ht_val_t val)
{
  struct watchman_client *client = (struct watchman_client*)val;

  close(client->fd);
  free(client);
}

static struct watchman_hash_funcs client_hash_funcs = {
  NULL, // copy_key
  NULL, // del_key
  NULL, // equal_key
  NULL, // hash_key
  NULL, // copy_val
  client_delete
};

void w_free_rules(struct watchman_rule *head)
{
  struct watchman_rule *r;

  while (head) {
    r = head;
    head = r->next;

    free((char*)r->pattern);
    free(r);
  }
}

/* Parses filename match rules.
 * By default, we want to include items that positively match
 * the set of fnmatch(3) patterns specified.
 * If -X is specified, we switch to exclude mode; any patterns
 * that are encountered after -X are excluded from the result set.
 * If -I is specified, we switch to include mode, so you can use
 * -I to turn on include mode again after using -X.
 * If "!" is specified, the following pattern is negated.
 * We switch back out of negation mode after that pattern.
 *
 * We stop processing args when we find "--" and update
 * *next_arg to the argv index after that argument.
 */
static bool parse_watch_params(int start, json_t *args,
    struct watchman_rule **head_ptr,
    uint32_t *next_arg)
{
  bool include = true;
  bool negated = false;
  struct watchman_rule *rule, *prior = NULL;
  uint32_t i;

  if (!json_is_array(args)) {
    return false;
  }
  *head_ptr = NULL;

  for (i = start; i < json_array_size(args); i++) {
    const char *arg = json_string_value(json_array_get(args, i));
    if (!arg) {
      /* not a string value! */
      return false; // FIXME: leak
    }

    if (!strcmp(arg, "--")) {
      i++;
      break;
    }
    if (!strcmp(arg, "-X")) {
      include = false;
      continue;
    }
    if (!strcmp(arg, "-I")) {
      include = true;
      continue;
    }
    if (!strcmp(arg, "!")) {
      negated = true;
      continue;
    }

    rule = calloc(1, sizeof(*rule));
    if (!rule) {
      return false; // FIXME: leak
    }

    rule->include = include;
    rule->negated = negated;
    // We default the fnmatch so that we can match against paths that include
    // slashes.
    // To recursively match the contents of a dir, use "dir/*".  To match all
    // "C" source files, use "*.c".  To match all makefiles, use
    // "*/Makefile" + "Makefile" (include the latter if the Makefile might
    // be at the top level).
    rule->pattern = strdup(arg);
    rule->flags = FNM_PERIOD;

    if (!prior) {
      *head_ptr = rule;
    } else {
      prior->next = rule;
    }
    prior = rule;

    w_log(W_LOG_DBG, "made rule %s %s %s\n",
        rule->include ? "-I" : "-X",
        rule->negated ? "!" : "",
        rule->pattern);

    // Reset negated flag
    negated = false;
  }

  if (next_arg) {
    *next_arg = i;
  }

  return true;
}

// must be called with root locked
uint32_t w_rules_match(w_root_t *root,
    struct watchman_file *oldest_file,
    struct watchman_rule_match **results,
    struct watchman_rule *head)
{
  struct watchman_file *file;
  struct watchman_rule *rule;
  w_string_t *full_name;
  w_string_t *relname;
  uint32_t num_matches = 0;
  uint32_t name_start;
  struct watchman_rule_match *res = NULL;
  uint32_t num_allocd = 0;

  name_start = root->root_path->len + 1;

  for (file = oldest_file; file; file = file->prev) {
    // no rules means return everything
    bool matched = (head == NULL) ? true : false;

    full_name = w_string_path_cat(file->parent->path, file->name);
    // Record the name relative to the root
    relname = w_string_slice(full_name, name_start,
                full_name->len - name_start);
    w_string_delref(full_name);

    // Work through the rules; we stop as soon as we get a match.
    for (rule = head; rule && !matched; rule = rule->next) {

      // In theory, relname->buf may not be NUL terminated in
      // the right spot if it was created as a slice.
      // In practice, we don't see those, but if we do, we should
      // probably make a copy of the string into a stack buffer :-/
      matched = fnmatch(rule->pattern, relname->buf, rule->flags) == 0;

      // If the rule is negated, we negate the sense of the
      // match result
      if (rule->negated) {
        matched = !matched;
      }

      // If the pattern matched then we're going to include the file
      // in our result set, but only if it is set to include.
      // If we're not including, we explicitly don't want to know
      // about the file, so pretend it didn't match and stop processing
      // rules for the file.
      if (matched && !rule->include) {
        matched = false;
        break;
      }
    }

    if (matched) {

      if (num_matches + 1 > num_allocd) {
        struct watchman_rule_match *new_res;

        num_allocd = num_allocd ? num_allocd * 2 : 64;
        new_res = realloc(res, num_allocd * sizeof(struct watchman_rule_match));
        if (!new_res) {
          w_log(W_LOG_DBG, "out of memory while running rules!\n");
          w_string_delref(relname);
          free(res);
          return 0;
        }
        res = new_res;
      }

      res[num_matches].relname = relname;
      res[num_matches].file = file;
      num_matches++;
    } else {
      w_string_delref(relname);
    }
  }

  *results = res;

  return num_matches;
}

void w_match_results_free(uint32_t num_matches,
    struct watchman_rule_match *matches)
{
  uint32_t i;

  for (i = 0; i < num_matches; i++) {
    w_string_delref(matches[i].relname);
  }
  free(matches);
}

struct w_clockspec_query {
  bool is_timestamp;
  struct timeval tv;
  uint32_t ticks;
};

// may attempt to lock the root!
static bool parse_clockspec(w_root_t *root,
    json_t *value,
    struct w_clockspec_query *since)
{
  const char *str;
  int pid;

  if (json_is_integer(value)) {
    since->is_timestamp = true;
    since->tv.tv_usec = 0;
    since->tv.tv_sec = json_integer_value(value);
    return true;
  }

  str = json_string_value(value);
  if (!str) {
    return false;
  }

  if (str[0] == 'n' && str[1] == ':') {
    w_string_t *name = w_string_new(str);

    since->is_timestamp = false;
    w_root_lock(root);
    w_root_wait_for_settle(root, -1);

    // If we've never seen it before, ticks will be set to 0
    // which is exactly what we want here.
    since->ticks = (uint32_t)w_ht_get(root->cursors, (w_ht_val_t)name);

    // Bump the tick value and record it against the cursor.
    // We need to bump the tick value so that repeated queries
    // when nothing has changed in the filesystem won't continue
    // to return the same set of files; we only want the first
    // of these to return the files and the rest to return nothing
    // until something subsequently changes
    w_ht_replace(root->cursors, (w_ht_val_t)name, ++root->ticks);

    w_string_delref(name);

    w_root_unlock(root);
    return true;
  }

  if (sscanf(str, "c:%d:%" PRIu32, &pid, &since->ticks) == 2) {
    since->is_timestamp = false;
    if (pid == getpid()) {
      return true;
    }
    // If the pid doesn't match, they asked a different
    // incarnation of the server, so we treat them as having
    // never spoken to us before
    since->ticks = 0;
    return true;
  }

  return false;
}

json_t *w_match_results_to_json(
    uint32_t num_matches,
    struct watchman_rule_match *matches)
{
  json_t *file_list = json_array();
  uint32_t i;

  for (i = 0; i < num_matches; i++) {
    struct watchman_file *file = matches[i].file;
    w_string_t *relname = matches[i].relname;

    json_t *record = json_object();

    json_object_set_new(record, "name", json_string(relname->buf));
    json_object_set_new(record, "exists", json_boolean(file->exists));
    // Note: our JSON library supports 64-bit integers, but this may
    // pose a compatibility issue for others.  We'll see if anyone
    // runs into an issue and deal with it then...
    json_object_set_new(record, "size", json_integer(file->st.st_size));
    json_object_set_new(record, "mode", json_integer(file->st.st_mode));
    json_object_set_new(record, "uid", json_integer(file->st.st_uid));
    json_object_set_new(record, "gid", json_integer(file->st.st_gid));
    json_object_set_new(record, "atime", json_integer(file->st.st_atime));
    json_object_set_new(record, "mtime", json_integer(file->st.st_mtime));
    json_object_set_new(record, "ctime", json_integer(file->st.st_ctime));

    json_array_append_new(file_list, record);
  }

  return file_list;
}

static void run_rules(struct watchman_client *client,
    w_root_t *root,
    struct w_clockspec_query *since,
    struct watchman_rule *rules)
{
  uint32_t matches;
  struct watchman_rule_match *results = NULL;
  struct watchman_file *oldest = NULL, *f;
  json_t *response = make_response();
  json_t *file_list;

  w_log(W_LOG_DBG, "running rules!\n");

  w_root_lock(root);
  for (f = root->latest_file; f; f = f->next) {
    if (since) {
      if (since->is_timestamp &&
          w_timeval_compare(f->otime.tv, since->tv) < 0) {
        break;
      }
      if (!since->is_timestamp && f->otime.ticks < since->ticks) {
        break;
      }
    }
    oldest = f;
  }
  annotate_with_clock(root, response);
  matches = w_rules_match(root, oldest, &results, rules);
  w_root_unlock(root);

  w_log(W_LOG_DBG, "rules were run, we have %" PRIu32 " matches\n", matches);

  file_list = w_match_results_to_json(matches, results);
  w_match_results_free(matches, results);

  json_object_set_new(response, "files", file_list);

  send_and_dispose_response(client, response);
}

static w_root_t *resolve_root_or_err(
    struct watchman_client *client,
    json_t *args,
    int root_index,
    bool create)
{
  w_root_t *root;
  const char *root_name;
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

  root = w_root_resolve(root_name, create);
  if (!root) {
    send_error_response(client,
        "unable to resolve root %s",
        root_name);
  }
  return root;
}

/* find /root [patterns] */
static void cmd_find(
    struct watchman_client *client,
    json_t *args)
{
  struct watchman_rule *rules = NULL;
  w_root_t *root;

  /* resolve the root */
  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments for 'find'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  /* parse argv into a chain of watchman_rule */
  if (!parse_watch_params(2, args, &rules, NULL)) {
    send_error_response(client, "invalid rule spec: %s", strerror(errno));
    return;
  }

  /* now find all matching files */
  run_rules(client, root, NULL, rules);
}

/* since /root <timestamp> [patterns] */
static void cmd_since(
    struct watchman_client *client,
    json_t *args)
{
  struct watchman_rule *rules = NULL;
  w_root_t *root;
  json_t *clock_ele;
  struct w_clockspec_query since;

  /* resolve the root */
  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments for 'since'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  clock_ele = json_array_get(args, 2);
  if (!parse_clockspec(root, clock_ele, &since)) {
    send_error_response(client,
        "expected argument 2 to be a valid clockspec");
    return;
  }

  /* parse argv into a chain of watchman_rule */
  if (!parse_watch_params(3, args, &rules, NULL)) {
    send_error_response(client, "invalid rule spec: %s", strerror(errno));
    return;
  }

  /* now find all matching files */
  run_rules(client, root, &since, rules);
}

/* trigger-list /root
 * Displays a list of registered triggers for a given root
 */
static void cmd_trigger_list(
    struct watchman_client *client,
    json_t *args)
{
  w_root_t *root;
  json_t *resp;
  json_t *arr;

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  resp = make_response();
  w_root_lock(root);
  arr = w_root_trigger_list_to_json(root);
  w_root_unlock(root);

  json_object_set_new(resp, "triggers", arr);
  send_and_dispose_response(client, resp);
}

/* trigger /root triggername [watch patterns] -- cmd to run
 * Sets up a trigger so that we can execute a command when a change
 * is detected */
static void cmd_trigger(
    struct watchman_client *client,
    json_t *args)
{
  struct watchman_rule *rules;
  w_root_t *root;
  uint32_t next_arg = 0;
  struct watchman_trigger_command *cmd;
  json_t *resp;
  const char *name;

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments");
    return;
  }
  name = json_string_value(json_array_get(args, 2));
  if (!name) {
    send_error_response(client, "expected 2nd parameter to be trigger name");
    return;
  }

  if (!parse_watch_params(3, args, &rules, &next_arg)) {
    send_error_response(client, "invalid rule spec: %s", strerror(errno));
    return;
  }

  if (next_arg >= json_array_size(args)) {
    send_error_response(client, "no command was specified");
    return;
  }

  cmd = calloc(1, sizeof(*cmd));
  if (!cmd) {
    send_error_response(client, "no memory!");
    return;
  }

  cmd->rules = rules;
  cmd->argc = json_array_size(args) - next_arg;
  cmd->argv = w_argv_copy_from_json(args, next_arg);
  if (!cmd->argv) {
    free(cmd);
    send_error_response(client, "unable to build argv array");
    return;
  }

  cmd->triggername = w_string_new(name);
  w_root_lock(root);
  w_ht_replace(root->commands, (w_ht_val_t)cmd->triggername, (w_ht_val_t)cmd);
  w_root_unlock(root);

  w_state_save();

  resp = make_response();
  json_object_set_new(resp, "triggerid", json_string(name));
  send_and_dispose_response(client, resp);
}

/* watch /root */
static void cmd_watch(
    struct watchman_client *client,
    json_t *args)
{
  w_root_t *root;
  json_t *resp;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  resp = make_response();
  json_object_set_new(resp, "watch", json_string(root->root_path->buf));
  send_and_dispose_response(client, resp);
}

static int parse_log_level(const char *str)
{
  if (!strcmp(str, "debug")) {
    return W_LOG_DBG;
  } else if (!strcmp(str, "error")) {
    return W_LOG_ERR;
  } else if (!strcmp(str, "off")) {
    return W_LOG_OFF;
  }
  return -1;
}

// log-level "debug"
// log-level "error"
// log-level "off"
static void cmd_loglevel(
    struct watchman_client *client,
    json_t *args)
{
  const char *cmd, *str;
  json_t *resp;
  int level;

  if (json_unpack(args, "[ss]", &cmd, &str)) {
    send_error_response(client, "expected a debug level argument");
    return;
  }

  level = parse_log_level(str);
  if (level == -1) {
    send_error_response(client, "invalid debug level %s", str);
    return;
  }

  client->log_level = level;

  resp = make_response();
  json_object_set_new(resp, "log_level", json_string(str));

  send_and_dispose_response(client, resp);
}

// log "debug" "text to log"
static void cmd_log(
    struct watchman_client *client,
    json_t *args)
{
  const char *cmd, *str, *text;
  json_t *resp;
  int level;

  if (json_unpack(args, "[sss]", &cmd, &str, &text)) {
    send_error_response(client, "expected a string to log");
    return;
  }

  level = parse_log_level(str);
  if (level == -1) {
    send_error_response(client, "invalid debug level %s", str);
    return;
  }

  w_log(level, "%s\n", text);

  resp = make_response();
  json_object_set_new(resp, "logged", json_true());
  send_and_dispose_response(client, resp);
}


static void cmd_shutdown(
    struct watchman_client *client,
    json_t *args)
{
  unused_parameter(client);
  unused_parameter(args);

  w_log(W_LOG_ERR, "shutdown-server was requested, exiting!\n");
  exit(0);
}

static struct {
  const char *name;
  watchman_command_func func;
} commands[] = {
  { "find", cmd_find },
  { "since", cmd_since },
  { "watch", cmd_watch },
  { "trigger", cmd_trigger },
  { "trigger-list", cmd_trigger_list },
  { "shutdown-server", cmd_shutdown },
  { "log-level", cmd_loglevel },
  { "log", cmd_log },
  { NULL, NULL }
};


// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
// TODO: want to allow notifications to be sent over
// the socket as we notice them
static void *client_thread(void *ptr)
{
  struct watchman_client *client = ptr;
  watchman_command_func func;
  const char *cmd_name;
  w_string_t *cmd;
  struct pollfd pfd[2];
  json_t *request;
  json_error_t jerr;
  char buf[16];

  w_set_nonblock(client->fd);

  while (true) {
    // Wait for input from either the client socket or
    // via the ping pipe, which signals that some other
    // thread wants to unilaterally send data to the client

    pfd[0].fd = client->fd;
    pfd[0].events = POLLIN|POLLHUP|POLLERR;
    pfd[0].revents = 0;

    pfd[1].fd = client->ping[0];
    pfd[1].events = POLLIN|POLLHUP|POLLERR;
    pfd[1].revents = 0;

    ignore_result(poll(pfd, 2, 200));

    if (pfd[0].revents) {
      request = w_json_buffer_next(&client->reader, client->fd, &jerr);

      if (!request && errno == EAGAIN) {
        // That's fine
      } else if (!request) {
        // Not so cool
        send_error_response(client, "invalid json at position %d: %s",
            jerr.position, jerr.text);

        pthread_mutex_lock(&client_lock);
        w_ht_del(clients, client->fd);
        pthread_mutex_unlock(&client_lock);
        break;
      } else if (request) {
        if (!json_array_size(request)) {
          send_error_response(client,
              "invalid command (expected an array with some elements!)");
          continue;
        }

        cmd_name = json_string_value(json_array_get(request, 0));
        if (!cmd_name) {
          send_error_response(client,
              "invalid command: expected element 0 to be the command name");
          continue;
        }
        cmd = w_string_new(cmd_name);
        func = (watchman_command_func)w_ht_get(command_funcs, (w_ht_val_t)cmd);
        w_string_delref(cmd);

        if (func) {
          func(client, request);
        } else {
          send_error_response(client, "unknown command %s", cmd_name);
        }

        json_decref(request);
      }
    }

    if (pfd[1].revents) {
      ignore_result(read(client->ping[0], buf, sizeof(buf)));
    }

    /* now send our response(s) */
    while (client->head) {
      struct watchman_client_response *resp;

      /* de-queue the first response */
      pthread_mutex_lock(&client_lock);
      resp = client->head;
      if (resp) {
        client->head = resp->next;
        if (client->tail == resp) {
          client->tail = NULL;
        }
      }
      pthread_mutex_unlock(&client_lock);

      if (resp) {
        w_clear_nonblock(client->fd);

        w_json_buffer_write(&client->writer, client->fd,
            resp->json, JSON_COMPACT);
        json_decref(resp->json);
        free(resp);

        w_set_nonblock(client->fd);
      }
    }
  }

  return NULL;
}

void w_log_to_clients(int level, const char *buf)
{
  json_t *json = NULL;
  w_ht_iter_t iter;

  if (!clients) {
    return;
  }

  pthread_mutex_lock(&client_lock);
  if (w_ht_first(clients, &iter)) do {
    struct watchman_client *client = (void*)iter.value;

    if (client->log_level != W_LOG_OFF && client->log_level >= level) {
      json = make_response();
      if (json) {
        json_object_set_new(json, "log", json_string(buf));
        if (!enqueue_response(client, json, true)) {
          json_decref(json);
        }
      }
    }

  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&client_lock);
}

static void *child_reaper(void *arg)
{
  int st;
  pid_t pid;

  unused_parameter(arg);

  while (true) {
    pid = waitpid(-1, &st, 0);

    // Shame that we can't just tell the kernel
    // to block us until we get a child...
    if (pid == -1 && errno == ECHILD) {
      usleep(20000);
    }

    if (pid > 0) {
      w_mark_dead(pid);
    }
  }
  return 0;
}

bool w_start_listener(const char *path)
{
  int fd;
  int i;
  struct sockaddr_un un;
  pthread_t thr;
  pthread_attr_t attr;

#ifdef HAVE_KQUEUE
#include <sys/sysctl.h>
#include <sys/syslimits.h>
  {
    struct rlimit limit;
    int mib[2] = { CTL_KERN, KERN_MAXFILESPERPROC };
    int maxperproc;
    size_t len;

    len = sizeof(maxperproc);
    sysctl(mib, 2, &maxperproc, &len, NULL, 0);

    getrlimit(RLIMIT_NOFILE, &limit);
    w_log(W_LOG_ERR, "file limit is %" PRIu64
        " kern.maxfilesperproc=%i\n",
        limit.rlim_cur, maxperproc);

    if (limit.rlim_cur < (uint64_t)maxperproc) {
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

    if (limit.rlim_cur < 10240) {
      w_log(W_LOG_ERR,
          "Your file descriptor limit is very low (%" PRIu64 "), "
          "please consult the watchman docs on raising the limits\n",
          limit.rlim_cur);
    }
  }
#endif

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "%s: path is too long\n",
        path);
    return false;
  }

  signal(SIGPIPE, SIG_IGN);
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&thr, &attr, child_reaper, NULL)) {
    perror("pthread_create(child_reaper)");
    return false;
  }

  fd = socket(PF_LOCAL, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    return false;
  }

  un.sun_family = PF_LOCAL;
  strcpy(un.sun_path, path);

  if (bind(fd, (struct sockaddr*)&un, sizeof(un)) != 0) {
    w_log(W_LOG_ERR, "bind(%s): %s\n",
      path, strerror(errno));
    close(fd);
    return false;
  }

  if (listen(fd, 200) != 0) {
    w_log(W_LOG_ERR, "listen(%s): %s\n",
        path, strerror(errno));
    close(fd);
    return false;
  }

  w_set_cloexec(fd);

  if (!clients) {
    clients = w_ht_new(2, &client_hash_funcs);
  }

  // Wire up the command handlers
  command_funcs = w_ht_new(16, &w_ht_string_funcs);
  for (i = 0; commands[i].name; i++) {
    w_ht_set(command_funcs,
        (w_ht_val_t)w_string_new(commands[i].name),
        (w_ht_val_t)commands[i].func);
  }

  w_state_load();

  // Now run the dispatch
  while (true) {
    int client_fd;
    struct watchman_client *client;

    client_fd = accept(fd, NULL, 0);
    if (client_fd == -1) {
      continue;
    }
    w_set_cloexec(client_fd);

    client = calloc(1, sizeof(*client));
    client->fd = client_fd;
    if (!w_json_buffer_init(&client->reader)) {
      // FIXME: error handling
    }
    if (!w_json_buffer_init(&client->writer)) {
      // FIXME: error handling
    }
    if (pipe(client->ping)) {
      // FIXME: error handling
    }
    w_set_cloexec(client->ping[0]);
    w_set_nonblock(client->ping[0]);
    w_set_cloexec(client->ping[1]);
    w_set_nonblock(client->ping[1]);

    pthread_mutex_lock(&client_lock);
    w_ht_set(clients, client->fd, (w_ht_val_t)client);
    pthread_mutex_unlock(&client_lock);

    // Start a thread for the client.
    // We used to use libevent for this, but we have
    // a low volume of concurrent clients and the json
    // parse/encode APIs are not easily used in a non-blocking
    // server architecture.
    if (pthread_create(&thr, &attr, client_thread, client)) {
      // It didn't work out, sorry!
      pthread_mutex_lock(&client_lock);
      w_ht_del(clients, client->fd);
      pthread_mutex_unlock(&client_lock);
    }
  }

  pthread_attr_destroy(&attr);
  return true;
}

/* vim:ts=2:sw=2:et:
 */

