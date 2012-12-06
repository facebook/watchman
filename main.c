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
#include <poll.h>

static char *sock_name = NULL;
static char *log_name = NULL;
static int persistent = 0;
static struct sockaddr_un un;

static void daemonize(void)
{
  int fd;

  // the double-fork-and-setsid trick establishes a
  // child process that runs in its own process group
  // with its own session and that won't get killed
  // off when your shell exits (for example).
  if (fork()) {
    // The parent of the first fork is the client
    // process that is being run by the user, and
    // we want to allow that to continue.
    return;
  }
  setsid();
  if (fork()) {
    // The parent of the second fork has served its
    // purpose, so we simply exit here, otherwise
    // we'll duplicate the effort of either the
    // client or the server depending on if we
    // return or not.
    _exit(0);
  }

  // we are the child, let's set things up

  // redirect std{in,out,err}
  fd = open("/dev/null", O_RDONLY);
  if (fd != -1) {
    dup2(fd, STDIN_FILENO);
    close(fd);
  }
  fd = open(log_name, O_WRONLY|O_APPEND|O_CREAT, 0600);
  if (fd != -1) {
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
  }

  /* we are the child, let's set things up */
  ignore_result(chdir("/"));
  w_start_listener(sock_name);
  exit(1);
}

static const char *get_env_with_fallback(const char *name1,
    const char *name2, const char *fallback)
{
  const char *val;

  val = getenv(name1);
  if (!val || *val == 0) {
    val = getenv(name2);
  }
  if (!val || *val == 0) {
    val = fallback;
  }

  return val;
}

static void setup_sock_name(void)
{
  const char *tmp = get_env_with_fallback("TMPDIR", "TMP", "/tmp");
  const char *user = get_env_with_fallback("USER", "LOGNAME", NULL);

  if (!user) {
    w_log(W_LOG_ERR, "watchman requires that you set $USER in your env\n");
    abort();
  }

  if (!sock_name) {
    asprintf(&sock_name, "%s/.watchman.%s", tmp, user);
  }
  if (!sock_name || sock_name[0] != '/') {
    w_log(W_LOG_ERR, "invalid or missing sockname!\n");
    abort();
  }

  if (!log_name) {
    asprintf(&log_name, "%s/.watchman.%s.log", tmp, user);
  }
  if (!log_name) {
    w_log(W_LOG_ERR, "out of memory while processing log name\n");
    abort();
  }

  un.sun_family = PF_LOCAL;
  strcpy(un.sun_path, sock_name);

  if (strlen(sock_name) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "%s: path is too long\n",
        sock_name);
    abort();
  }
}

static bool should_start(int err)
{
  if (err == ECONNREFUSED) {
    return true;
  }
  if (err == ENOENT) {
    return true;
  }
  return false;
}

static int cmd_write(const char *buffer, size_t size, void *ptr)
{
  int fd = (intptr_t)ptr;

  return (size_t)write(fd, buffer, size) == size ? 0 : -1;
}

static bool read_response(w_jreader_t *reader, int fd)
{
  json_t *j;
  json_error_t jerr;

  j = w_json_reader_next(reader, fd, &jerr);

  if (!j) {
    w_log(W_LOG_ERR, "failed to parse response: %s\n",
        jerr.text);
    return false;
  }

  // Let's just pretty print the JSON response
  json_dumpf(j, stdout, JSON_INDENT(4));
  printf("\n");

  json_decref(j);

  return true;
}

static bool try_command(int argc, char **argv, int timeout)
{
  int fd;
  int res;
  int tries;
  int i;
  json_t *j;
  w_jreader_t reader;

  fd = socket(PF_LOCAL, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    return false;
  }

  tries = 0;
  do {
    res = connect(fd, (struct sockaddr*)&un, sizeof(un));
    if (res == 0) {
      break;
    }

    if (timeout && tries < timeout && should_start(errno)) {
      // Wait for socket to come up
      sleep(1);
      continue;
    }

  } while (++tries < timeout);

  if (res) {
    return false;
  }

  if (argc == 0) {
    return true;
  }

  // Send command
  j = json_array();
  for (i = 0; i < argc; i++) {
    json_array_append_new(j, json_string(argv[i]));
  }
  json_dump_callback(j, cmd_write, (void*)(intptr_t)fd, JSON_COMPACT);
  json_decref(j);
  ignore_result(write(fd, "\n", 1));

  w_json_reader_init(&reader);

  do {
    if (!read_response(&reader, fd)) {
      w_json_reader_free(&reader);
      return false;
    }
  } while (persistent);
  w_json_reader_free(&reader);

  return true;
}

static struct watchman_getopt opts[] = {
  { "sockname", 'U', "Specify alternate sockname",
    REQ_STRING, &sock_name, "PATH" },
  { "logfile", 'o', "Specify path to logfile",
    REQ_STRING, &log_name, "PATH" },
  { "persistent", 'p', "Persist and wait for further responses",
    OPT_NONE, &persistent, NULL },
  { 0, 0, 0, 0, 0, 0 }
};

static void parse_cmdline(int *argcp, char ***argvp)
{
  w_getopt(opts, argcp, argvp);

  setup_sock_name();
}

int main(int argc, char **argv)
{
  bool ran;

  parse_cmdline(&argc, &argv);

  ran = try_command(argc, argv, 0);
  if (!ran && should_start(errno)) {
    unlink(sock_name);
    daemonize();

    ran = try_command(argc, argv, 10);
  }

  if (ran) {
    return 0;
  }

  w_log(W_LOG_ERR, "unable to talk to your watchman!\n");
  return 1;
}

/* vim:ts=2:sw=2:et:
 */

