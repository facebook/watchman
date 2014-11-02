/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <poll.h>

static int show_help = 0;
static int show_version = 0;
static enum w_pdu_type server_pdu = is_json_compact;
static enum w_pdu_type output_pdu = is_json_pretty;
static char *server_encoding = NULL;
static char *output_encoding = NULL;
static char *sock_name = NULL;
static char *log_name = NULL;
#ifdef USE_GIMLI
static char *pid_file = NULL;
#endif
char *watchman_state_file = NULL;
static char **daemon_argv = NULL;
const char *watchman_tmp_dir = NULL;
static int persistent = 0;
int dont_save_state = 0;
static int foreground = 0;
static int no_pretty = 0;
static int no_spawn = 0;
static int no_local = 0;
static struct sockaddr_un un;
static int json_input_arg = 0;

static void run_service(void)
{
  int fd;
  bool res;

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

  watchman_watcher_init();
  res = w_start_listener(sock_name);
  watchman_watcher_dtor();

  if (res) {
    exit(0);
  }
  exit(1);
}

#ifndef USE_GIMLI
static void daemonize(void)
{
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
  run_service();
}
#endif

#ifdef USE_GIMLI

#define MAX_DAEMON_ARGS 64
static void append_argv(char **argv, char *item)
{
  int i;

  for (i = 0; argv[i]; i++) {
    ;
  }
  if (i + 1 >= MAX_DAEMON_ARGS) {
    abort();
  }

  argv[i] = item;
  argv[i+1] = NULL;
}

static void spawn_via_gimli(void)
{
  char *argv[MAX_DAEMON_ARGS] = {
    GIMLI_MONITOR_PATH,
#ifdef WATCHMAN_STATE_DIR
    "--trace-dir=" WATCHMAN_STATE_DIR "/traces",
#endif
    "--pidfile", pid_file,
    "watchman",
    "--foreground",
    NULL
  };
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  pid_t pid;
  int i;

  for (i = 0; daemon_argv[i]; i++) {
    append_argv(argv, daemon_argv[i]);
  }

  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions,
      STDOUT_FILENO, log_name, O_WRONLY|O_CREAT|O_APPEND, 0600);
  posix_spawn_file_actions_adddup2(&actions,
      STDOUT_FILENO, STDERR_FILENO);
  posix_spawnp(&pid, argv[0], &actions, &attr, argv, environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
}
#endif

static void parse_encoding(const char *enc, enum w_pdu_type *pdu)
{
  if (!enc) {
    return;
  }
  if (!strcmp(enc, "json")) {
    *pdu = is_json_compact;
    return;
  }
  if (!strcmp(enc, "bser")) {
    *pdu = is_bser;
    return;
  }
  w_log(W_LOG_ERR, "Invalid encoding '%s', use one of json or bser\n", enc);
  exit(EX_USAGE);
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

static void compute_file_name(char **strp,
    const char *user,
    const char *suffix,
    const char *what)
{
  char *str = NULL;

  str = *strp;

  if (!str) {
#ifdef WATCHMAN_STATE_DIR
    /* avoid redundant naming if they picked something like
     * "/var/watchman" */
    ignore_result(asprintf(&str, "%s/%s%s%s",
          WATCHMAN_STATE_DIR,
          user,
          suffix[0] ? "." : "",
          suffix));
#else
    ignore_result(asprintf(&str, "%s/.watchman.%s%s%s",
          watchman_tmp_dir,
          user,
          suffix[0] ? "." : "",
          suffix));
#endif
  }

  if (!str) {
    w_log(W_LOG_ERR, "out of memory computing %s", what);
    abort();
  }

  if (str[0] != '/') {
    w_log(W_LOG_ERR, "invalid %s: %s", what, str);
    abort();
  }


  *strp = str;
}

static void setup_sock_name(void)
{
  const char *user = get_env_with_fallback("USER", "LOGNAME", NULL);

  watchman_tmp_dir = get_env_with_fallback("TMPDIR", "TMP", "/tmp");

  if (!user) {
    uid_t uid = getuid();
    struct passwd *pw;

    pw = getpwuid(uid);
    if (!pw) {
      w_log(W_LOG_ERR, "getpwuid(%d) failed: %s. I don't know who you are\n",
          uid, strerror(errno));
      abort();
    }

    user = pw->pw_name;

    if (!user) {
      w_log(W_LOG_ERR, "watchman requires that you set $USER in your env\n");
      abort();
    }
  }

  compute_file_name(&sock_name, user, "", "sockname");
  compute_file_name(&watchman_state_file, user, "state", "statefile");
  compute_file_name(&log_name, user, "log", "logname");
#ifdef USE_GIMLI
  compute_file_name(&pid_file, user, "pid", "pidfile");
#endif

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

static bool try_command(json_t *cmd, int timeout)
{
  int fd;
  int res;
  int tries;
  int bufsize;
  w_jbuffer_t buffer;

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
    close(fd);
    return false;
  }

  if (!cmd) {
    close(fd);
    return true;
  }

  bufsize = WATCHMAN_IO_BUF_SIZE;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

  w_json_buffer_init(&buffer);

  // Send command
  if (!w_ser_write_pdu(server_pdu, &buffer, fd, cmd)) {
    w_log(W_LOG_ERR, "error sending PDU to server\n");
    w_json_buffer_free(&buffer);
    close(fd);
    return false;
  }

  w_json_buffer_reset(&buffer);

  do {
    if (!w_json_buffer_passthru(&buffer, output_pdu, fd)) {
      w_json_buffer_free(&buffer);
      close(fd);
      return false;
    }
  } while (persistent);
  w_json_buffer_free(&buffer);
  close(fd);

  return true;
}

static struct watchman_getopt opts[] = {
  { "help",     'h', "Show this help",
    OPT_NONE,   &show_help, NULL, NOT_DAEMON },
  { "version",  'v', "Show version number",
    OPT_NONE,   &show_version, NULL, NOT_DAEMON },
  { "sockname", 'U', "Specify alternate sockname",
    REQ_STRING, &sock_name, "PATH", IS_DAEMON },
  { "logfile", 'o', "Specify path to logfile",
    REQ_STRING, &log_name, "PATH", IS_DAEMON },
  { "log-level", 0, "set the log level (0 = off, default is 1, verbose = 2)",
    REQ_INT, &log_level, NULL, IS_DAEMON },
#ifdef USE_GIMLI
  { "pidfile", 0, "Specify path to gimli monitor pidfile",
    REQ_STRING, &pid_file, "PATH", NOT_DAEMON },
#endif
  { "persistent", 'p', "Persist and wait for further responses",
    OPT_NONE, &persistent, NULL, NOT_DAEMON },
  { "no-save-state", 'n', "Don't save state between invocations",
    OPT_NONE, &dont_save_state, NULL, IS_DAEMON },
  { "statefile", 0, "Specify path to file to hold watch and trigger state",
    REQ_STRING, &watchman_state_file, "PATH", IS_DAEMON },
  { "json-command", 'j', "Instead of parsing CLI arguments, take a single "
    "json object from stdin",
    OPT_NONE, &json_input_arg, NULL, NOT_DAEMON },
  { "output-encoding", 0, "CLI output encoding. json (default) or bser",
    REQ_STRING, &output_encoding, NULL, NOT_DAEMON },
  { "server-encoding", 0, "CLI<->server encoding. bser (default) or json",
    REQ_STRING, &server_encoding, NULL, NOT_DAEMON },
  { "foreground", 'f', "Run the service in the foreground",
    OPT_NONE, &foreground, NULL, NOT_DAEMON },
  { "no-pretty", 0, "Don't pretty print JSON",
    OPT_NONE, &no_pretty, NULL, NOT_DAEMON },
  { "no-spawn", 0, "Don't try to start the service if it is not available",
    OPT_NONE, &no_spawn, NULL, NOT_DAEMON },
  { "no-local", 0, "When no-spawn is enabled, don't try to handle request"
    " in client mode if service is unavailable",
    OPT_NONE, &no_local, NULL, NOT_DAEMON },
  { 0, 0, 0, 0, 0, 0, 0 }
};

static void parse_cmdline(int *argcp, char ***argvp)
{
  cfg_load_global_config_file();
  w_getopt(opts, argcp, argvp, &daemon_argv);
  if (show_help) {
    usage(opts, stdout);
  }
  if (show_version) {
    printf("%s\n", PACKAGE_VERSION);
    exit(0);
  }
  setup_sock_name();
  parse_encoding(server_encoding, &server_pdu);
  parse_encoding(output_encoding, &output_pdu);
  if (!output_encoding) {
    output_pdu = no_pretty ? is_json_compact : is_json_pretty;
  }
}

static json_t *build_command(int argc, char **argv)
{
  json_t *cmd;
  int i;

  // Read blob from stdin
  if (json_input_arg) {
    json_error_t err;

    cmd = json_loadf(stdin, 0, &err);
    if (cmd == NULL) {
      fprintf(stderr, "failed to parse JSON from stdin: %s\n",
          err.text);
      exit(1);
    }
    return cmd;
  }

  // Special case: no arguments means that we just want
  // to verify that the service is up, starting it if
  // needed
  if (argc == 0) {
    return NULL;
  }

  cmd = json_array();
  for (i = 0; i < argc; i++) {
    json_array_append_new(cmd, json_string(argv[i]));
  }

  return cmd;
}

const char *get_sock_name(void)
{
  return sock_name;
}

int main(int argc, char **argv)
{
  bool ran;
  json_t *cmd;

  parse_cmdline(&argc, &argv);

  if (foreground) {
    run_service();
    return 0;
  }

  cmd = build_command(argc, argv);
  preprocess_command(cmd, output_pdu);

  ran = try_command(cmd, 0);
  if (!ran && should_start(errno)) {
    if (no_spawn) {
      if (!no_local) {
        ran = try_client_mode_command(cmd, !no_pretty);
      }
    } else {
#ifdef USE_GIMLI
      spawn_via_gimli();
#else
      daemonize();
#endif
      ran = try_command(cmd, 10);
    }
  }

  json_decref(cmd);

  if (ran) {
    return 0;
  }

  if (!no_spawn) {
    w_log(W_LOG_ERR, "unable to talk to your watchman!\n");
  }
  return 1;
}

/* vim:ts=2:sw=2:et:
 */
