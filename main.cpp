/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "ChildProcess.h"
#include "Logging.h"
#include "ThreadPool.h"
#include "watchman.h"
#ifndef _WIN32
#include <poll.h>
#endif

using watchman::ChildProcess;
using watchman::FileDescriptor;
using Options = ChildProcess::Options;

static int show_help = 0;
static int show_version = 0;
static enum w_pdu_type server_pdu = is_bser;
static enum w_pdu_type output_pdu = is_json_pretty;
static uint32_t server_capabilities = 0;
static uint32_t output_capabilities = 0;
static char *server_encoding = NULL;
static char *output_encoding = NULL;
static char *test_state_dir = NULL;
static char *sock_name = NULL;
char *log_name = NULL;
static char *pid_file = NULL;
char *watchman_state_file = NULL;
static char **daemon_argv = NULL;
const char *watchman_tmp_dir = NULL;
static int persistent = 0;
int dont_save_state = 0;
static int foreground = 0;
static int no_pretty = 0;
static int no_spawn = 0;
static int no_local = 0;
#ifndef _WIN32
static int inetd_style = 0;
static struct sockaddr_un un;
#endif
static int json_input_arg = 0;

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static const char *compute_user_name(void);
static void compute_file_name(char **strp, const char *user, const char *suffix,
                              const char *what);

static bool lock_pidfile(void) {
#if !defined(USE_GIMLI) && !defined(_WIN32)
  struct flock lock;
  pid_t mypid;

  // We defer computing this path until we're in the server context because
  // eager evaluation can trigger integration test failures unless all clients
  // are aware of both the pidfile and the sockpath being used in the tests.
  compute_file_name(&pid_file, compute_user_name(), "pid", "pidfile");

  mypid = getpid();
  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_WRLCK;
  lock.l_start = 0;
  lock.l_whence = SEEK_SET;
  lock.l_len = 0;

  FileDescriptor fd(open(pid_file, O_RDWR | O_CREAT, 0644));

  if (!fd) {
    w_log(W_LOG_ERR, "Failed to open pidfile %s for write: %s\n", pid_file,
          strerror(errno));
    return false;
  }
  // Ensure that no children inherit the locked pidfile descriptor
  fd.setCloExec();

  if (fcntl(fd.fd(), F_SETLK, &lock) != 0) {
    char pidstr[32];
    int len;

    len = read(fd.fd(), pidstr, sizeof(pidstr) - 1);
    pidstr[len] = '\0';

    w_log(W_LOG_ERR, "Failed to lock pidfile %s: process %s owns it: %s\n",
          pid_file, pidstr, strerror(errno));
    return false;
  }

  // Replace contents of the pidfile with our pid string
  if (ftruncate(fd.fd(), 0)) {
    w_log(W_LOG_ERR, "Failed to truncate pidfile %s: %s\n",
        pid_file, strerror(errno));
    return false;
  }

  dprintf(fd.fd(), "%d", mypid);
  fsync(fd.fd());

  /* We are intentionally not closing the fd and intentionally not storing
   * a reference to it anywhere: the intention is that it remain locked
   * for the rest of the lifetime of our process.
   * close(fd); // NOPE!
   */
  fd.release();
  return true;
#else
  // ze-googles, they do nothing!!
  return true;
#endif
}

static void run_service(void)
{
  int fd;
  bool res;

#ifndef _WIN32
  // Before we redirect stdin/stdout to the log files, move any inetd-provided
  // socket to a different descriptor number.
  if (inetd_style) {
    w_listener_prep_inetd();
  }
#endif

  // redirect std{in,out,err}
  fd = open("/dev/null", O_RDONLY);
  if (fd != -1) {
    ignore_result(dup2(fd, STDIN_FILENO));
    close(fd);
  }
  fd = open(log_name, O_WRONLY|O_APPEND|O_CREAT, 0600);
  if (fd != -1) {
    ignore_result(dup2(fd, STDOUT_FILENO));
    ignore_result(dup2(fd, STDERR_FILENO));
    close(fd);
  }

  if (!lock_pidfile()) {
    return;
  }

#ifndef _WIN32
  /* we are the child, let's set things up */
  ignore_result(chdir("/"));
#endif

  w_set_thread_name("listener");
  {
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';
    w_log(W_LOG_ERR, "Watchman %s %s starting up on %s\n",
        PACKAGE_VERSION,
#ifdef WATCHMAN_BUILD_INFO
        WATCHMAN_BUILD_INFO,
#else
        "<no build info set>",
#endif
        hostname);
  }

#ifndef _WIN32
  // Block SIGCHLD by default; we only want it to be delivered
  // to the reaper thread and only when it is ready to reap.
  // This MUST happen before we spawn any threads so that they
  // can pick up our default blocked signal mask.
  {
    sigset_t sigset;

    sigemptyset(&sigset);
    sigaddset(&sigset, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sigset, NULL);
  }
#endif

  watchman::getThreadPool().start(
      cfg_get_int("thread_pool_worker_threads", 16),
      cfg_get_int("thread_pool_max_items", 1024 * 1024));

  ClockSpec::init();
  w_state_load();
  res = w_start_listener(sock_name);
  w_root_free_watched_roots();
  cfg_shutdown();

  watchman::log(watchman::ERR, "Exiting from service with res=", res, "\n");

  if (res) {
    exit(0);
  }
  exit(1);
}

#ifndef _WIN32
// close any random descriptors that we may have inherited,
// leaving only the main stdio descriptors open, if we execute a
// child process.
static void close_random_fds(void) {
  struct rlimit limit;
  long open_max = 0;
  int max_fd;

  // Deduce the upper bound for number of descriptors
  limit.rlim_cur = 0;
#ifdef RLIMIT_NOFILE
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    limit.rlim_cur = 0;
  }
#elif defined(RLIM_OFILE)
  if (getrlimit(RLIMIT_OFILE, &limit) != 0) {
    limit.rlim_cur = 0;
  }
#endif
#ifdef _SC_OPEN_MAX
  open_max = sysconf(_SC_OPEN_MAX);
#endif
  if (open_max <= 0) {
    open_max = 36; /* POSIX_OPEN_MAX (20) + some padding */
  }
  if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > INT_MAX) {
    // "no limit", which seems unlikely
    limit.rlim_cur = INT_MAX;
  }
  // Take the larger of the two values we compute
  if (limit.rlim_cur > (rlim_t)open_max) {
    open_max = limit.rlim_cur;
  }

  for (max_fd = open_max; max_fd > STDERR_FILENO; --max_fd) {
    close(max_fd);
  }
}
#endif

#if !defined(USE_GIMLI) && !defined(_WIN32)
static void daemonize(void)
{
  close_random_fds();

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

#ifdef _WIN32
static void spawn_win32(void) {
  char module_name[WATCHMAN_NAME_MAX];
  GetModuleFileName(NULL, module_name, sizeof(module_name));

  Options opts;
  opts.setFlags(POSIX_SPAWN_SETPGROUP);
  opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
  opts.open(STDOUT_FILENO, log_name, O_WRONLY | O_CREAT | O_APPEND, 0600);
  opts.dup2(STDOUT_FILENO, STDERR_FILENO);

  std::vector<w_string_piece> args{module_name, "--foreground"};
  for (size_t i = 0; daemon_argv[i]; i++) {
    args.push_back(daemon_argv[i]);
  }

  ChildProcess proc(args, std::move(opts));
  proc.disown();
}
#endif

#ifdef USE_GIMLI
static void spawn_via_gimli(void)
{
  std::vector<w_string_piece> args{
      GIMLI_MONITOR_PATH,
#ifdef WATCHMAN_STATE_DIR
      "--trace-dir=" WATCHMAN_STATE_DIR "/traces",
#endif
      "--pidfile",
      pid_file,
      "watchman",
      "--foreground",
  };

  for (size_t i = 0; daemon_argv[i]; i++) {
    args.push_back(daemon_argv[i]);
  }

  close_random_fds();

  Options opts;
  opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
  opts.open(STDOUT_FILENO, log_name, O_WRONLY | O_CREAT | O_APPEND, 0600);
  opts.dup2(STDOUT_FILENO, STDERR_FILENO);

  ChildProcess proc(args, std::move(opts));
  proc.disown();
}
#endif

#ifndef _WIN32
// Spawn watchman via a site-specific spawn helper program.
// We'll pass along any daemon-appropriate arguments that
// we noticed during argument parsing.
static void spawn_site_specific(const char *spawner)
{
  std::vector<w_string_piece> args{
      spawner,
  };

  for (size_t i = 0; daemon_argv[i]; i++) {
    args.push_back(daemon_argv[i]);
  }

  close_random_fds();

  Options opts;
  opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
  opts.open(STDOUT_FILENO, log_name, O_WRONLY | O_CREAT | O_APPEND, 0600);
  opts.dup2(STDOUT_FILENO, STDERR_FILENO);

  try {
    ChildProcess proc(args, std::move(opts));

    auto res = proc.wait();

    if (WIFEXITED(res) && WEXITSTATUS(res) == 0) {
      return;
    }

    if (WIFEXITED(res)) {
      watchman::log(
          watchman::FATAL,
          spawner,
          ": exited with status ",
          WEXITSTATUS(res),
          "\n");
    } else if (WIFSIGNALED(res)) {
      watchman::log(
          watchman::FATAL, spawner, ": signaled with ", WTERMSIG(res), "\n");
    }
    watchman::log(
        watchman::ERR, spawner, ": failed to start, exit status ", res, "\n");

  } catch (const std::exception& exc) {
    watchman::log(
        watchman::FATAL,
        "Failed to spawn watchman via `",
        spawner,
        "': ",
        exc.what(),
        "\n");
  }
}
#endif

#ifdef __APPLE__
static void spawn_via_launchd(void)
{
  char watchman_path[WATCHMAN_NAME_MAX];
  uint32_t size = sizeof(watchman_path);
  char plist_path[WATCHMAN_NAME_MAX];
  FILE *fp;
  struct passwd *pw;
  uid_t uid;

  close_random_fds();

  if (_NSGetExecutablePath(watchman_path, &size) == -1) {
    w_log(W_LOG_ERR, "_NSGetExecutablePath: path too long; size %u\n", size);
    abort();
  }

  uid = getuid();
  pw = getpwuid(uid);
  if (!pw) {
    w_log(W_LOG_ERR, "getpwuid(%d) failed: %s.  I don't know who you are\n",
        uid, strerror(errno));
    abort();
  }

  snprintf(plist_path, sizeof(plist_path),
      "%s/Library/LaunchAgents", pw->pw_dir);
  // Best effort attempt to ensure that the agents dir exists.  We'll detect
  // and report the failure in the fopen call below.
  mkdir(plist_path, 0755);
  snprintf(plist_path, sizeof(plist_path),
      "%s/Library/LaunchAgents/com.github.facebook.watchman.plist", pw->pw_dir);

  if (access(plist_path, R_OK) == 0) {
    // Unload any that may already exist, as it is likely wrong

    ChildProcess unload_proc(
        {"/bin/launchctl", "unload", "-F", plist_path}, Options());
    unload_proc.wait();

    // Forcibly remove the plist.  In some cases it may have some attributes
    // set that prevent launchd from loading it.  This can happen where
    // the system was re-imaged or restored from a backup
    unlink(plist_path);
  }

  fp = fopen(plist_path, "w");
  if (!fp) {
    w_log(W_LOG_ERR, "Failed to open %s for write: %s\n",
        plist_path, strerror(errno));
    abort();
  }

  compute_file_name(&pid_file, compute_user_name(), "pid", "pidfile");

  auto plist_content = watchman::to<std::string>(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n"
      "<dict>\n"
      "    <key>Label</key>\n"
      "    <string>com.github.facebook.watchman</string>\n"
      "    <key>Disabled</key>\n"
      "    <false/>\n"
      "    <key>ProgramArguments</key>\n"
      "    <array>\n"
      "        <string>",
      watchman_path,
      "</string>\n"
      "        <string>--foreground</string>\n"
      "        <string>--logfile=",
      log_name,
      "</string>\n"
      "        <string>--log-level=",
      log_level,
      "</string>\n"
      "        <string>--sockname=",
      sock_name,
      "</string>\n"
      "        <string>--statefile=",
      watchman_state_file,
      "</string>\n"
      "        <string>--pidfile=",
      pid_file,
      "</string>\n"
      "    </array>\n"
      "    <key>KeepAlive</key>\n"
      "    <dict>\n"
      "        <key>Crashed</key>\n"
      "        <true/>\n"
      "    </dict>\n"
      "    <key>RunAtLoad</key>\n"
      "    <true/>\n"
      "    <key>EnvironmentVariables</key>\n"
      "    <dict>\n"
      "        <key>PATH</key>\n"
      "        <string><![CDATA[",
      getenv("PATH"),
      "]]></string>\n"
      "    </dict>\n"
      "    <key>ProcessType</key>\n"
      "    <string>Interactive</string>\n"
      "    <key>Nice</key>\n"
      "    <integer>-5</integer>\n"
      "</dict>\n"
      "</plist>\n");
  fwrite(plist_content.data(), 1, plist_content.size(), fp);
  fclose(fp);
  // Don't rely on umask, ensure we have the correct perms
  chmod(plist_path, 0644);

  ChildProcess load_proc(
      {"/bin/launchctl", "load", "-F", plist_path}, Options());
  auto res = load_proc.wait();

  if (WIFEXITED(res) && WEXITSTATUS(res) == 0) {
    return;
  }

  // Most likely cause is "headless" operation with no GUI context
  if (WIFEXITED(res)) {
    w_log(W_LOG_ERR, "launchctl: exited with status %d\n", WEXITSTATUS(res));
  } else if (WIFSIGNALED(res)) {
    w_log(W_LOG_ERR, "launchctl: signaled with %d\n", WTERMSIG(res));
  }
  w_log(W_LOG_ERR, "Falling back to daemonize\n");
  daemonize();
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
  if (!strcmp(enc, "bser-v2")) {
    *pdu = is_bser_v2;
    return;
  }
  w_log(W_LOG_ERR, "Invalid encoding '%s', use one of json, bser or bser-v2\n",
      enc);
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
    /* We'll put our various artifacts in a user specific dir
     * within the state dir location */
    char *state_dir = NULL;
    const char *state_parent = test_state_dir ? test_state_dir :
#ifdef WATCHMAN_STATE_DIR
          WATCHMAN_STATE_DIR
#else
          watchman_tmp_dir
#endif
          ;

    ignore_result(asprintf(&state_dir, "%s/%s-state", state_parent, user));

    if (!state_dir) {
      w_log(W_LOG_ERR, "out of memory computing %s\n", what);
      exit(1);
    }

    if (mkdir(state_dir, 0700) == 0 || errno == EEXIST) {
#ifndef _WIN32
      // verify ownership
      struct stat st;
      int dir_fd;
      int ret = 0;
      uid_t euid = geteuid();
      // TODO: also allow a gid to be specified here
      const char* sock_group_name = cfg_get_string("sock_group", nullptr);
      // S_ISGID is set so that files inside this directory inherit the group
      // name
      mode_t dir_perms =
          cfg_get_perms(
              "sock_access", false /* write bits */, true /* execute bits */) |
          S_ISGID;

      auto dirp =
          w_dir_open(state_dir, false /* don't need strict symlink rules */);

      dir_fd = dirp->getFd();
      if (dir_fd == -1) {
        w_log(W_LOG_ERR, "dirfd(%s): %s\n", state_dir, strerror(errno));
        goto bail;
      }

      if (fstat(dir_fd, &st) != 0) {
        w_log(W_LOG_ERR, "fstat(%s): %s\n", state_dir, strerror(errno));
        ret = 1;
        goto bail;
      }
      if (euid != st.st_uid) {
        w_log(W_LOG_ERR,
            "the owner of %s is uid %d and doesn't match your euid %d\n",
            state_dir, st.st_uid, euid);
        ret = 1;
        goto bail;
      }
      if (st.st_mode & 0022) {
        w_log(W_LOG_ERR,
            "the permissions on %s allow others to write to it. "
            "Verify that you own the contents and then fix its "
            "permissions by running `chmod 0700 %s`\n",
            state_dir,
            state_dir);
        ret = 1;
        goto bail;
      }

      if (sock_group_name) {
        const struct group *sock_group = w_get_group(sock_group_name);
        if (!sock_group) {
          ret = 1;
          goto bail;
        }

        if (fchown(dir_fd, -1, sock_group->gr_gid) == -1) {
          w_log(W_LOG_ERR, "setting up group '%s' failed: %s\n",
                sock_group_name, strerror(errno));
          ret = 1;
          goto bail;
        }
      }

      // Depending on group and world accessibility, change permissions on the
      // directory. We can't leave the directory open and set permissions on the
      // socket because not all POSIX systems respect permissions on UNIX domain
      // sockets, but all POSIX systems respect permissions on the containing
      // directory.
      w_log(W_LOG_DBG, "Setting permissions on state dir to 0%o\n", dir_perms);
      if (fchmod(dir_fd, dir_perms) == -1) {
        w_log(W_LOG_ERR, "fchmod(%s, %#o): %s\n", state_dir, dir_perms,
              strerror(errno));
        ret = 1;
        goto bail;
      }

    bail:
      if (ret) {
        exit(ret);
      }
#endif
    } else {
      w_log(W_LOG_ERR, "while computing %s: failed to create %s: %s\n", what,
            state_dir, strerror(errno));
      exit(1);
    }

    ignore_result(asprintf(&str, "%s/%s", state_dir, suffix));

    if (!str) {
      w_log(W_LOG_ERR, "out of memory computing %s", what);
      abort();
    }

    free(state_dir);
  }

#ifndef _WIN32
  if (str[0] != '/') {
    w_log(W_LOG_ERR, "invalid %s: %s", what, str);
    abort();
  }
#endif

  *strp = str;
}

#ifndef _WIN32
const struct group *w_get_group(const char *group_name) {
  // This explicit errno statement is necessary to distinguish between the
  // group not existing and an error.
  errno = 0;
  struct group *group = getgrnam(group_name);
  if (!group) {
    if (errno == 0) {
      w_log(W_LOG_ERR, "group '%s' does not exist\n", group_name);
    } else {
      w_log(W_LOG_ERR, "getting gid for '%s' failed: %s\n", group_name,
            strerror(errno));
    }
    return nullptr;
  }
  return group;
}
#endif // ndef _WIN32

static const char *compute_user_name(void) {
  const char *user = get_env_with_fallback("USER", "LOGNAME", NULL);
#ifdef _WIN32
  static char user_buf[256];
#endif

  if (!user) {
#ifdef _WIN32
    DWORD size = sizeof(user_buf);
    if (GetUserName(user_buf, &size)) {
      user_buf[size] = 0;
      user = user_buf;
    } else {
      w_log(W_LOG_FATAL, "GetUserName failed: %s. I don't know who you are\n",
          win32_strerror(GetLastError()));
    }
#else
    uid_t uid = getuid();
    struct passwd *pw;

    pw = getpwuid(uid);
    if (!pw) {
      w_log(W_LOG_FATAL, "getpwuid(%d) failed: %s. I don't know who you are\n",
          uid, strerror(errno));
    }

    user = pw->pw_name;
#endif

    if (!user) {
      w_log(W_LOG_ERR, "watchman requires that you set $USER in your env\n");
      abort();
    }
  }

  return user;
}

static void setup_sock_name(void)
{
  const char *user = compute_user_name();

  watchman_tmp_dir = get_env_with_fallback("TMPDIR", "TMP", "/tmp");

#ifdef _WIN32
  if (!sock_name) {
    asprintf(&sock_name, "\\\\.\\pipe\\watchman-%s", user);
  }
#else
  compute_file_name(&sock_name, user, "sock", "sockname");
#endif
  compute_file_name(&watchman_state_file, user, "state", "statefile");
  compute_file_name(&log_name, user, "log", "logname");
#ifdef USE_GIMLI
  compute_file_name(&pid_file, user, "pid", "pidfile");
#endif

#ifndef _WIN32
  if (strlen(sock_name) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "%s: path is too long\n",
        sock_name);
    abort();
  }

  un.sun_family = PF_LOCAL;
  memcpy(un.sun_path, sock_name, strlen(sock_name) + 1);
#endif
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
  w_jbuffer_t buffer;
  w_jbuffer_t output_pdu_buffer;
  int err;

  auto client = w_stm_connect(sock_name, timeout * 1000);
  if (!client) {
    return false;
  }

  if (!cmd) {
    return true;
  }

  // Send command
  if (!buffer.pduEncodeToStream(
          server_pdu, server_capabilities, cmd, client.get())) {
    err = errno;
    w_log(W_LOG_ERR, "error sending PDU to server\n");
    errno = err;
    return false;
  }

  buffer.clear();

  do {
    if (!buffer.passThru(
            output_pdu,
            output_capabilities,
            &output_pdu_buffer,
            client.get())) {
      return false;
    }
  } while (persistent);

  return true;
}

static struct watchman_getopt opts[] = {
  { "help",     'h', "Show this help",
    OPT_NONE,   &show_help, NULL, NOT_DAEMON },
#ifndef _WIN32
  { "inetd",    0,   "Spawning from an inetd style supervisor",
    OPT_NONE,   &inetd_style, NULL, IS_DAEMON },
#endif
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
#else
  { "pidfile", 0, "Specify path to pidfile",
    REQ_STRING, &pid_file, "PATH", IS_DAEMON },
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
  // test-state-dir is for testing only and should not be used in production:
  // instead, use the compile-time WATCHMAN_STATE_DIR option
  { "test-state-dir", 0, NULL, REQ_STRING, &test_state_dir, "DIR", NOT_DAEMON },
  { 0, 0, 0, OPT_NONE, 0, 0, 0 }
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
  watchman::getLog().setStdErrLoggingLevel(
      static_cast<enum watchman::LogLevel>(log_level));
  setup_sock_name();
  parse_encoding(server_encoding, &server_pdu);
  parse_encoding(output_encoding, &output_pdu);
  if (!output_encoding) {
    output_pdu = no_pretty ? is_json_compact : is_json_pretty;
  }
}

static json_ref build_command(int argc, char** argv) {
  int i;

  // Read blob from stdin
  if (json_input_arg) {
    json_error_t err;
    w_jbuffer_t buf;

    memset(&err, 0, sizeof(err));
    auto cmd = buf.decodeNext(w_stm_stdin(), &err);

    if (buf.pdu_type == is_bser) {
      // If they used bser for the input, select bser for output
      // unless they explicitly requested something else
      if (!server_encoding) {
        server_pdu = is_bser;
      }
      if (!output_encoding) {
        output_pdu = is_bser;
      }
    } else if (buf.pdu_type == is_bser_v2) {
      // If they used bser v2 for the input, select bser v2 for output
      // unless they explicitly requested something else
      if (!server_encoding) {
        server_pdu = is_bser_v2;
      }
      if (!output_encoding) {
        output_pdu = is_bser_v2;
      }
    }

    if (!cmd) {
      fprintf(
          stderr,
          "failed to parse command from stdin: "
          "line %d, column %d, position %d: %s\n",
          err.line,
          err.column,
          err.position,
          err.text);
      exit(1);
    }
    return cmd;
  }

  // Special case: no arguments means that we just want
  // to verify that the service is up, starting it if
  // needed
  if (argc == 0) {
    return nullptr;
  }

  auto cmd = json_array();
  for (i = 0; i < argc; i++) {
    json_array_append_new(cmd, typed_string_to_json(argv[i], W_STRING_UNICODE));
  }

  return cmd;
}

const char *get_sock_name(void)
{
  return sock_name;
}

static void spawn_watchman(void) {
#ifndef _WIN32
  // If we have a site-specific spawning requirement, then we'll
  // invoke that spawner rather than using any of the built-in
  // spawning functionality.
  const char* site_spawn = cfg_get_string("spawn_watchman_service", nullptr);
  if (site_spawn) {
    spawn_site_specific(site_spawn);
    return;
  }
#endif

#ifdef USE_GIMLI
  spawn_via_gimli();
#elif defined(__APPLE__)
  spawn_via_launchd();
#elif defined(_WIN32)
  spawn_win32();
#else
  daemonize();
#endif
}

int main(int argc, char **argv)
{
  bool ran;

  parse_cmdline(&argc, &argv);

  if (foreground) {
    run_service();
    return 0;
  }

  w_set_thread_name("cli");
  auto cmd = build_command(argc, argv);
  preprocess_command(cmd, output_pdu, output_capabilities);

  ran = try_command(cmd, 0);
  if (!ran && should_start(errno)) {
    if (no_spawn) {
      if (!no_local) {
        ran = try_client_mode_command(cmd, !no_pretty);
      }
    } else {
      spawn_watchman();
      // Some site spawner scripts will asynchronously launch the service.
      // When that happens we may encounter ECONNREFUSED.  We need to
      // tolerate this, so we add some retries.
      int attempts = 10;
      std::chrono::milliseconds interval(10);
      while (true) {
        ran = try_command(cmd, 10);
        if (!ran && should_start(errno) && attempts-- > 0) {
          /* sleep override */ std::this_thread::sleep_for(interval);
          interval *= 2;
          continue;
        }
        // Success or terminal failure
        break;
      }
    }
  }

  if (ran) {
    return 0;
  }

  if (!no_spawn) {
    w_log(W_LOG_ERR, "unable to talk to your watchman on %s! (%s)\n",
        sock_name, strerror(errno));
#ifdef __APPLE__
    if (getenv("TMUX")) {
      w_log(W_LOG_ERR, "\n"
"You may be hitting a tmux related session issue.\n"
"An immediate workaround is to run:\n"
"\n"
"    watchman version\n"
"\n"
"just once, from *outside* your tmux session, to allow the launchd\n"
"registration to be setup.  Once done, you can continue to access\n"
"watchman from inside your tmux sessions as usual.\n"
"\n"
"Longer term, you may wish to install this tool:\n"
"\n"
"    https://github.com/ChrisJohnsen/tmux-MacOSX-pasteboard\n"
"\n"
"and configure tmux to use `reattach-to-user-namespace`\n"
"when it launches your shell.\n");
    }
#endif
  }
  return 1;
}

/* vim:ts=2:sw=2:et:
 */
