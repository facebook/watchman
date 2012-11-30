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
#include <fnmatch.h>

static struct event listener_ev;
static w_ht_t *clients = NULL;
static w_ht_t *command_funcs = NULL;

static void client_delete(w_ht_val_t val)
{
  struct watchman_client *client = (struct watchman_client*)val;

  bufferevent_disable(client->bev, EV_READ|EV_WRITE);
  bufferevent_free(client->bev);
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
static bool parse_watch_params(int start, int argc, char **argv,
    struct watchman_rule **head_ptr,
    int *next_arg)
{
  bool include = true;
  bool negated = false;
  struct watchman_rule *rule, *prior = NULL;
  int i;

  *head_ptr = NULL;

  for (i = start; i < argc; i++) {
    if (!strcmp(argv[i], "--")) {
      i++;
      break;
    }
    if (!strcmp(argv[i], "-X")) {
      include = false;
      continue;
    }
    if (!strcmp(argv[i], "-I")) {
      include = true;
      continue;
    }
    if (!strcmp(argv[i], "!")) {
      negated = true;
      continue;
    }

    rule = calloc(1, sizeof(*rule));
    if (!rule) {
      return false;
    }

    rule->include = include;
    rule->negated = negated;
    // We default the fnmatch so that we can match against paths that include
    // slashes.
    // To recursively match the contents of a dir, use "dir/*".  To match all
    // "C" source files, use "*.c".  To match all makefiles, use
    // "*/Makefile" + "Makefile" (include the latter if the Makefile might
    // be at the top level).
    rule->pattern = strdup(argv[i]);
    rule->flags = FNM_PERIOD;

    if (!prior) {
      *head_ptr = rule;
    } else {
      prior->next = rule;
    }
    prior = rule;

    printf("made rule %s %s %s\n",
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
    w_ht_t *uniq, struct watchman_rule *head)
{
  struct watchman_file *file;
  struct watchman_rule *rule;
  w_string_t *full_name;
  w_string_t *relname;
  uint32_t num_matches = 0;
  uint32_t name_start;

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

      w_ht_set(uniq, (w_ht_val_t)relname, (w_ht_val_t)file);
      num_matches++;
    }

    w_string_delref(relname);
  }

  return num_matches;
}

typedef void (*watchman_command_func)(
    struct watchman_client *client,
    int argc,
    char **argv);

static void run_rules(struct watchman_client *client,
    w_root_t *root,
    w_clock_t *since,
    struct watchman_rule *rules)
{
  w_ht_iter_t iter;
  uint32_t matches;
  w_ht_t *uniq;
  struct watchman_file *oldest = NULL, *f;

  uniq = w_ht_new(8, &w_ht_string_funcs);
  printf("running rules!\n");

  w_root_lock(root);
  for (f = root->latest_file; f; f = f->next) {
    if (since && f->otime.seconds < since->seconds) {
      break;
    }
    oldest = f;
  }
  matches = w_rules_match(root, oldest, uniq, rules);
  w_root_unlock(root);
  /* advise client of success and tell them how many lines follow */
  w_client_printf(client, "OK %" PRIu32 " matches\n", matches);
  printf("rules were run, we have %" PRIu32 " matches\n", matches);

  if (w_ht_first(uniq, &iter)) do {
    struct watchman_file *file = (struct watchman_file*)iter.value;
    w_string_t *relname = (w_string_t*)iter.key;

    w_client_printf(client, "%c %.*s\n",
        file->exists ? 'M' : 'D',
        relname->len, relname->buf);

  } while (w_ht_next(uniq, &iter));
  w_client_printf(client, "DONE\n");

  w_ht_free(uniq);
}

/* find /root [patterns] */
static void cmd_find(
    struct watchman_client *client,
    int argc,
    char **argv)
{
  struct watchman_rule *rules = NULL, *rule;
  w_root_t *root;

  /* resolve the root */
  if (argc < 2) {
    w_client_printf(client, "ERR: not enough arguments to find\n");
    return;
  }

  root = w_root_resolve(argv[1], false);
  if (!root) {
    w_client_printf(client, "ERR: not watching %s\n", argv[1]);
    return;
  }

  /* parse argv into a chain of watchman_rule */
  if (!parse_watch_params(2, argc, argv, &rules, NULL)) {
    w_client_printf(client, "ERR: %s\n", strerror(errno));
    return;
  }

  /* now find all matching files */
  run_rules(client, root, NULL, rules);
}

/* since /root <timestamp> [patterns] */
static void cmd_since(
    struct watchman_client *client,
    int argc,
    char **argv)
{
  struct watchman_rule *rules = NULL, *rule;
  w_root_t *root;
  w_clock_t since;

  /* resolve the root */
  if (argc < 3) {
    w_client_printf(client, "ERR: not enough arguments to find\n");
    return;
  }

  root = w_root_resolve(argv[1], false);
  if (!root) {
    w_client_printf(client, "ERR: not watching %s\n", argv[1]);
    return;
  }

  // FIXME: allow using a safer clock representation instead.
  since.seconds = atoi(argv[2]);

  /* parse argv into a chain of watchman_rule */
  if (!parse_watch_params(3, argc, argv, &rules, NULL)) {
    w_client_printf(client, "ERR: %s\n", strerror(errno));
    return;
  }

  /* now find all matching files */
  run_rules(client, root, &since, rules);
}

/* trigger /root [watch patterns] -- cmd to run
 * Sets up a trigger so that we can execute a command when a change
 * is detected */
static void cmd_trigger(
    struct watchman_client *client,
    int argc,
    char **argv)
{
  struct watchman_rule *rules;
  w_root_t *root;
  int next_arg = 0;
  struct watchman_trigger_command *cmd;

  root = w_root_resolve(argv[1], true);
  if (!root) {
    w_client_printf(client, "ERR: can't watch %s\n", argv[1]);
    return;
  }

  if (!parse_watch_params(2, argc, argv, &rules, &next_arg)) {
    w_client_printf(client, "ERR: %s\n", strerror(errno));
    return;
  }

  if (next_arg >= argc) {
    w_client_printf(client, "ERR: no command was specified\n");
    return;
  }

  cmd = calloc(1, sizeof(*cmd));
  if (!cmd) {
    w_client_printf(client, "ERR: no memory!\n");
    return;
  }

  cmd->rules = rules;
  cmd->argc = argc - next_arg;
  cmd->argv = w_argv_dup(cmd->argc, argv + next_arg);
  if (!cmd->argv) {
    free(cmd);
    w_client_printf(client, "ERR: no memory!\n");
    return;
  }

  w_root_lock(root);
  cmd->triggerid = ++root->next_cmd_id;
  w_ht_set(root->commands, cmd->triggerid, (w_ht_val_t)cmd);
  w_root_unlock(root);

  w_client_printf(client, "OK: %" PRIu32 " registered\n",
      cmd->triggerid);
}

/* watch /root */
static void cmd_watch(
    struct watchman_client *client,
    int argc,
    char **argv)
{
  struct watchman_rule *rules = NULL, *rule;
  w_root_t *root;
  w_clock_t since;

  /* resolve the root */
  if (argc != 2) {
    w_client_printf(client, "ERR: wrong number of arguments to watch\n");
    return;
  }

  root = w_root_resolve(argv[1], true);
  if (!root) {
    w_client_printf(client, "ERR: not watching %s\n", argv[1]);
    return;
  }

  w_client_printf(client, "OK: watching %s\n", argv[1]);
}

static struct {
  const char *name;
  watchman_command_func func;
} commands[] = {
  { "find", cmd_find },
  { "since", cmd_since },
  { "watch", cmd_watch },
  { "trigger", cmd_trigger },
  { NULL, NULL }
};

int w_client_printf(struct watchman_client *client,
    const char *fmt, ...)
{
  int ret;
  va_list ap;

  va_start(ap, fmt);
  ret = evbuffer_add_vprintf(
      client->bev->output,
      fmt, ap);
  va_end(ap);

  return ret;
}

int w_client_vprintf(struct watchman_client *client,
    const char *fmt, va_list ap)
{
  return evbuffer_add_vprintf(
      client->bev->output,
      fmt, ap);
}

static void client_read(struct bufferevent *bev, void *arg)
{
  struct watchman_client *client = arg;
  char *line;
  size_t len;
  int i;
  char **argv;
  int argc;
  watchman_command_func func;
  w_string_t *cmd;

  line = evbuffer_readline(bev->input);
  if (!line) {
    return;
  }
  len = strlen(line);
  printf("[%d] %" PRIi64 " %s\n", client->fd, (int64_t)len, line);

  if (!w_argv_parse(line, &argc, &argv)) {
    printf("bad quoting or something\n");
    free(line);
    w_ht_del(clients, client->fd);
    return;
  }

  printf("argc=%d\n", argc);
  for (i = 0; i < argc; i++) {
    printf("[%d] %s\n", i, argv[i]);
  }

  cmd = w_string_new(argv[0]);
  func = (watchman_command_func)w_ht_get(command_funcs, (w_ht_val_t)cmd);
  w_string_delref(cmd);

  if (func) {
    func(client, argc, argv);
  }

  /* there's no implicit flush so we need to explicitly re-enable
   * writes so that our buffered output goes out */
  bufferevent_enable(client->bev, EV_READ|EV_WRITE);

  free(argv);
  free(line);
}

static void client_error(struct bufferevent *bev, short what, void *arg)
{
  struct watchman_client *client = arg;

  printf("client error: %d what=%x\n", client->fd, what);

  w_ht_del(clients, client->fd);
}

static void accept_client(int listener_fd, short mask, void *ignore)
{
  struct watchman_client *client;
  int fd = accept(listener_fd, NULL, 0);

  if (fd == -1) {
    return;
  }

  client = calloc(1, sizeof(*client));
  client->fd = fd;
  w_set_cloexec(fd);

  client->bev = bufferevent_new(client->fd,
      client_read, NULL, client_error,
      client);

  bufferevent_enable(client->bev, EV_READ|EV_WRITE);

  w_client_printf(client, "WATCHMAN 1.0\n");

  w_ht_set(clients, client->fd, (w_ht_val_t)client);
}

bool w_start_listener(const char *path)
{
  int fd;
  int i;
  struct sockaddr_un un;

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    fprintf(stderr, "%s: path is too long\n",
        path);
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
    fprintf(stderr, "bind(%s): %s\n",
      path, strerror(errno));
    close(fd);
    return false;
  }

  if (listen(fd, 200) != 0) {
    fprintf(stderr, "listen(%s): %s\n",
        path, strerror(errno));
    close(fd);
    return false;
  }

  w_set_cloexec(fd);
  event_set(&listener_ev, fd, EV_PERSIST|EV_READ, accept_client, NULL);
  if (event_add(&listener_ev, NULL) != 0) {
    fprintf(stderr, "event_add failed: %s\n",
      strerror(errno));
    close(fd);
    return false;
  }

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

  return true;
}

/* vim:ts=2:sw=2:et:
 */

