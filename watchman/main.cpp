/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Exception.h>
#include <folly/ScopeGuard.h>
#include <folly/Singleton.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/net/NetworkSocket.h>

#include <stdio.h>
#include <variant>

#include "watchman/ChildProcess.h"
#include "watchman/Clock.h"
#include "watchman/GroupLookup.h"
#include "watchman/LogConfig.h"
#include "watchman/Logging.h"
#include "watchman/Options.h"
#include "watchman/PDU.h"
#include "watchman/PerfSample.h"
#include "watchman/ProcessLock.h"
#include "watchman/ThreadPool.h"
#include "watchman/UserDir.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/fs/DirHandle.h"
#include "watchman/fs/FileSystem.h"
#include "watchman/listener.h"
#include "watchman/root/watchlist.h"
#include "watchman/sockname.h"
#include "watchman/state.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_stream.h"

#ifdef _WIN32
#include <Lmcons.h> // @manual
#include <Shlobj.h> // @manual
#include <deelevate.h> // @manual
#endif

#ifndef _WIN32
#include <poll.h> // @manual
#endif

using namespace watchman;

namespace {
w_pdu_type server_pdu = is_bser;
w_pdu_type output_pdu = is_json_pretty;
uint32_t server_capabilities = 0;
uint32_t output_capabilities = 0;
char** daemon_argv = NULL;
struct sockaddr_un un;
} // namespace

#ifdef __APPLE__
#include <mach-o/dyld.h> // @manual
#endif

static void compute_file_name(
    std::string& str,
    const std::string& user,
    const char* suffix,
    const char* what,
    bool require_absolute = true);

namespace {
const std::string& get_pid_file() {
  // We defer computing this path until we're in the server context because
  // eager evaluation can trigger integration test failures unless all clients
  // are aware of both the pidfile and the sockpath being used in the tests.
  compute_file_name(flags.pid_file, computeUserName(), "pid", "pidfile");
  return flags.pid_file;
}
} // namespace

/**
 * Log and fatal if Watchman was started with a low priority, which can cause a
 * poor experience, as Watchman is unable to keep up with the filesystem's
 * change notifications, triggering recrawls.
 */
void detect_low_process_priority() {
#ifndef _WIN32
  // Since `-1` is a valid nice level, in order to detect an
  // error we clear errno first and then test whether it is
  // non-zero after we have retrieved the nice value.
  errno = 0;
  auto nice_value = nice(0);
  folly::checkPosixError(errno, "failed to get `nice` value");

  if (nice_value > cfg_get_int("min_acceptable_nice_value", 0)) {
    log(watchman::FATAL,
        "Watchman is running at a lower than normal priority. Since that "
        "results in poor performance that is otherwise very difficult to "
        "trace, diagnose and debug, Watchman is refusing to start.\n");
  }
#endif
}

[[noreturn]] static void run_service(ProcessLock::Handle&&) {
#ifndef _WIN32
  // Before we redirect stdin/stdout to the log files, move any inetd-provided
  // socket to a different descriptor number.
  if (flags.inetd_style) {
    w_listener_prep_inetd();
  }
#endif

  // redirect std{in,out,err}
  int fd = ::open("/dev/null", O_RDONLY);
  if (fd != -1) {
    ignore_result(::dup2(fd, STDIN_FILENO));
    ::close(fd);
  }

  if (logging::log_name != "-") {
    fd = open(logging::log_name.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd != -1) {
      ignore_result(::dup2(fd, STDOUT_FILENO));
      ignore_result(::dup2(fd, STDERR_FILENO));
      ::close(fd);
    }
  }

  // If we weren't attached to a tty, check this now that we've opened
  // the log files so that we can log the problem there.
  //
  // This is unlikely to trip, as both foreground and daemonized execution
  // check process priority prior.
  detect_low_process_priority();

#ifndef _WIN32
  /* we are the child, let's set things up */
  ignore_result(chdir("/"));
#endif

  w_set_thread_name("listener");
  {
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';
    logf(
        ERR,
        "Watchman {} {} starting up on {}\n",
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
  bool res = w_start_listener();
  w_root_free_watched_roots();
  perf_shutdown();
  cfg_shutdown();

  log(ERR, "Exiting from service with res=", res, "\n");

  if (res) {
    exit(0);
  }
  exit(1);
}

// close any random descriptors that we may have inherited,
// leaving only the main stdio descriptors open, if we execute a
// child process.
static void close_random_fds() {
#ifndef _WIN32
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
#endif
}

[[noreturn]] static void run_service_in_foreground() {
  detect_low_process_priority();
  close_random_fds();

  auto& pid_file = get_pid_file();
  auto processLock = ProcessLock::acquire(pid_file);
  run_service(processLock.writePid(pid_file));
}

namespace {
struct [[nodiscard]] SpawnResult {
  enum Status {
    Spawned,
    FailedToLock,
  };

  SpawnResult() = delete;

  /* implicit */ SpawnResult(Status s, std::string r = {})
      : status{s}, reason{std::move(r)} {}

  void exitIfFailed() {
    if (status == FailedToLock) {
      fprintf(stderr, "%s\n", reason.c_str());
      exit(1);
    }
  }

  Status status;

  /**
   * If status is not Spawned, then this contains the error message.
   */
  std::string reason;
};
} // namespace

#ifndef _WIN32
/**
 * Forks and daemonizes, starting the Watchman service in the child.
 */
static SpawnResult run_service_as_daemon() {
  detect_low_process_priority();
  close_random_fds();

  // Lock the pidfile before we daemonize so that errors can be detected
  // and returned (for logging) before we drop stderr. This prevents failure to
  // lock from causing the daemonize process to start and immediately exit with
  // an error, making it hard to track down why a command isn't succeeding.
  auto acquireResult = ProcessLock::tryAcquire(get_pid_file());
  if (auto* reason = std::get_if<std::string>(&acquireResult)) {
    return SpawnResult{SpawnResult::FailedToLock, *reason};
  }

  auto& processLock = std::get<ProcessLock>(acquireResult);

  // the double-fork-and-setsid trick establishes a
  // child process that runs in its own process group
  // with its own session and that won't get killed
  // off when your shell exits (for example).
  if (fork()) {
    // The parent of the first fork is the client
    // process that is being run by the user, and
    // we want to allow that to continue.
    return SpawnResult::Spawned;
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

  // We are the child. Let's populate the pid file and start listening on the
  // socket.
  run_service(processLock.writePid(get_pid_file()));
}
#endif

#ifdef _WIN32
static SpawnResult spawn_win32() {
  char module_name[WATCHMAN_NAME_MAX];
  GetModuleFileName(NULL, module_name, sizeof(module_name));

  ChildProcess::Options opts;
  opts.setFlags(POSIX_SPAWN_SETPGROUP);
  opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
  opts.open(
      STDOUT_FILENO,
      logging::log_name.c_str(),
      O_WRONLY | O_CREAT | O_APPEND,
      0600);
  opts.dup2(STDOUT_FILENO, STDERR_FILENO);
  opts.chdir("/");

  std::vector<std::string_view> args{module_name, "--foreground"};
  for (size_t i = 0; daemon_argv[i]; i++) {
    args.push_back(daemon_argv[i]);
  }

  ChildProcess proc(args, std::move(opts));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  if (proc.terminated()) {
    logf(
        ERR,
        "Failed to spawn watchman server; it exited with code {}.\n"
        "Check the log file at {} for more information\n",
        proc.wait(),
        logging::log_name);
    exit(1);
  }
  proc.disown();
  return SpawnResult::Spawned;
}
#endif

#ifndef _WIN32
// Spawn watchman via a site-specific spawn helper program.
// We'll pass along any daemon-appropriate arguments that
// we noticed during argument parsing.
static SpawnResult spawn_site_specific(const char* spawner) {
  std::vector<std::string_view> args{
      spawner,
  };

  for (size_t i = 0; daemon_argv[i]; i++) {
    args.push_back(daemon_argv[i]);
  }

  close_random_fds();

  // Note that we're not setting up the output to go to the log files
  // here.  This is intentional; we'd like any failures in the spawner
  // to bubble up to the user as having things silently fail and get
  // logged to the server log doesn't provide any obvious cues to the
  // user about what went wrong.  Watchman will open and redirect output
  // to its log files when it ultimately is launched and enters the
  // run_service() function above.
  // However, we do need to make sure that any output from both stdout
  // and stderr goes to stderr of the end user.
  ChildProcess::Options opts;
  opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
  opts.dup2(STDERR_FILENO, STDOUT_FILENO);
  opts.dup2(STDERR_FILENO, STDERR_FILENO);

  try {
    ChildProcess proc(args, std::move(opts));

    auto res = proc.wait();

    if (WIFEXITED(res) && WEXITSTATUS(res) == 0) {
      return SpawnResult::Spawned;
    }

    if (WIFEXITED(res)) {
      log(FATAL, spawner, ": exited with status ", WEXITSTATUS(res), "\n");
    } else if (WIFSIGNALED(res)) {
      log(FATAL, spawner, ": signaled with ", WTERMSIG(res), "\n");
    }
    log(FATAL, spawner, ": failed to start, exit status ", res, "\n");

  } catch (const std::exception& exc) {
    log(FATAL,
        "Failed to spawn watchman via `",
        spawner,
        "': ",
        exc.what(),
        "\n");
  }

  return SpawnResult::Spawned;
}
#endif

#ifdef __APPLE__
static SpawnResult spawn_via_launchd() {
  char watchman_path[WATCHMAN_NAME_MAX];
  uint32_t size = sizeof(watchman_path);
  char plist_path[WATCHMAN_NAME_MAX];
  FILE* fp;
  struct passwd* pw;
  uid_t uid;

  close_random_fds();

  if (_NSGetExecutablePath(watchman_path, &size) == -1) {
    log(FATAL, "_NSGetExecutablePath: path too long; size ", size, "\n");
  }

  uid = getuid();
  pw = getpwuid(uid);
  if (!pw) {
    log(FATAL,
        "getpwuid(",
        uid,
        ") failed: ",
        folly::errnoStr(errno),
        ".  I don't know who you are\n");
  }

  snprintf(
      plist_path, sizeof(plist_path), "%s/Library/LaunchAgents", pw->pw_dir);
  // Best effort attempt to ensure that the agents dir exists.  We'll detect
  // and report the failure in the fopen call below.
  mkdir(plist_path, 0755);
  snprintf(
      plist_path,
      sizeof(plist_path),
      "%s/Library/LaunchAgents/com.github.facebook.watchman.plist",
      pw->pw_dir);

  if (access(plist_path, R_OK) == 0) {
    // Unload any that may already exist, as it is likely wrong

    ChildProcess unload_proc(
        {"/bin/launchctl", "unload", "-F", plist_path},
        ChildProcess::Options());
    unload_proc.wait();

    // Forcibly remove the plist.  In some cases it may have some attributes
    // set that prevent launchd from loading it.  This can happen where
    // the system was re-imaged or restored from a backup
    unlink(plist_path);
  }

  fp = fopen(plist_path, "w");
  if (!fp) {
    log(FATAL,
        "Failed to open ",
        plist_path,
        " for write: ",
        folly::errnoStr(errno),
        "\n");
  }

  compute_file_name(flags.pid_file, computeUserName(), "pid", "pidfile");

  auto plist_content = folly::to<std::string>(
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
      logging::log_name,
      "</string>\n"
      "        <string>--log-level=",
      logging::log_level,
      "</string>\n"
      // TODO: switch from `--sockname` to `--unix-listener-path`
      // after a grace period to allow for sane results if we
      // roll back to an earlier version
      "        <string>--sockname=",
      get_unix_sock_name(),
      "</string>\n"
      "        <string>--statefile=",
      flags.watchman_state_file,
      "</string>\n"
      "        <string>--pidfile=",
      flags.pid_file,
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
      {"/bin/launchctl", "load", "-F", plist_path}, ChildProcess::Options());
  auto res = load_proc.wait();

  if (WIFEXITED(res) && WEXITSTATUS(res) == 0) {
    return SpawnResult::Spawned;
  }

  // Most likely cause is "headless" operation with no GUI context
  if (WIFEXITED(res)) {
    logf(ERR, "launchctl: exited with status {}\n", WEXITSTATUS(res));
  } else if (WIFSIGNALED(res)) {
    logf(ERR, "launchctl: signaled with {}\n", WTERMSIG(res));
  }
  logf(ERR, "Falling back to daemonize\n");
  return run_service_as_daemon();
}
#endif

static void parse_encoding(const std::string& enc, enum w_pdu_type* pdu) {
  if (enc.empty()) {
    return;
  }
  if (enc == "json") {
    *pdu = is_json_compact;
    return;
  }
  if (enc == "bser") {
    *pdu = is_bser;
    return;
  }
  if (enc == "bser-v2") {
    *pdu = is_bser_v2;
    return;
  }
  log(ERR, "Invalid encoding '", enc, "', use one of json, bser or bser-v2\n");
  exit(EX_USAGE);
}

static void verify_dir_ownership(const std::string& state_dir) {
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
      openDir(state_dir.c_str(), false /* don't need strict symlink rules */);

  dir_fd = dirp->getFd();
  if (dir_fd == -1) {
    log(ERR, "dirfd(", state_dir, "): ", folly::errnoStr(errno), "\n");
    goto bail;
  }

  if (fstat(dir_fd, &st) != 0) {
    log(ERR, "fstat(", state_dir, "): ", folly::errnoStr(errno), "\n");
    ret = 1;
    goto bail;
  }
  if (euid != st.st_uid) {
    log(ERR,
        "the owner of ",
        state_dir,
        " is uid ",
        st.st_uid,
        " and doesn't match your euid ",
        euid,
        "\n");
    ret = 1;
    goto bail;
  }
  if (st.st_mode & 0022) {
    log(ERR,
        "the permissions on ",
        state_dir,
        " allow others to write to it. "
        "Verify that you own the contents and then fix its "
        "permissions by running `chmod 0700 '",
        state_dir,
        "'`\n");
    ret = 1;
    goto bail;
  }

  if (sock_group_name) {
    const struct group* sock_group = w_get_group(sock_group_name);
    if (!sock_group) {
      ret = 1;
      goto bail;
    }

    if (fchown(dir_fd, -1, sock_group->gr_gid) == -1) {
      log(ERR,
          "setting up group '",
          sock_group_name,
          "' failed: ",
          folly::errnoStr(errno),
          "\n");
      ret = 1;
      goto bail;
    }
  }

  // Depending on group and world accessibility, change permissions on the
  // directory. We can't leave the directory open and set permissions on the
  // socket because not all POSIX systems respect permissions on UNIX domain
  // sockets, but all POSIX systems respect permissions on the containing
  // directory.
  logf(DBG, "Setting permissions on state dir to {:o}\n", dir_perms);
  if (fchmod(dir_fd, dir_perms) == -1) {
    logf(
        ERR,
        "fchmod({}, {:o}): {}\n",
        state_dir,
        dir_perms,
        folly::errnoStr(errno));
    ret = 1;
    goto bail;
  }

bail:
  if (ret) {
    exit(ret);
  }
#endif
}

static void compute_file_name(
    std::string& str,
    const std::string& user,
    const char* suffix,
    const char* what,
    bool require_absolute) {
  bool str_computed = false;
  if (str.empty()) {
    str_computed = true;
    /* We'll put our various artifacts in a user specific dir
     * within the state dir location */
    auto state_dir = computeWatchmanStateDirectory(user);

    if (mkdir(state_dir.c_str(), 0700) == 0 || errno == EEXIST) {
      verify_dir_ownership(state_dir.c_str());
    } else {
      log(ERR,
          "while computing ",
          what,
          ": failed to create ",
          state_dir,
          ": ",
          folly::errnoStr(errno),
          "\n");
      exit(1);
    }

    str = folly::to<std::string>(state_dir, "/", suffix);
  }
#ifndef _WIN32
  if (require_absolute && !w_string_piece(str).pathIsAbsolute()) {
    log(FATAL,
        what,
        " must be an absolute file path but ",
        str,
        " was",
        str_computed ? " computed." : " provided.",
        "\n");
  }
#endif
}

#ifdef _WIN32
bool initialize_winsock() {
  WSADATA wsaData;
  if ((WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) ||
      (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)) {
    return false;
  }
  return true;
}

bool initialize_uds() {
  if (!initialize_winsock()) {
    log(DBG, "unable to initialize winsock, disabling UDS support\n");
  }

  // Test if UDS support is present
  FileDescriptor fd(
      ::socket(PF_LOCAL, SOCK_STREAM, 0), FileDescriptor::FDType::Socket);

  bool fd_initialized = (bool)fd;

  if (!fd_initialized) {
    log(DBG, "unable to create UNIX domain socket, disabling UDS support\n");
    return false;
  }

  return true;
}
#endif

static void setup_sock_name() {
#ifdef _WIN32
  if (!initialize_uds()) {
    // if we can't create UNIX domain socket, disable it.
    disable_unix_socket = true;
  }
#endif

  auto user = computeUserName();

  // Precompute the temporary directory path in case this process's environment
  // changes.
  (void)getTemporaryDirectory();

#ifdef _WIN32
  // On Windows, if an application uses --sockname to override the named
  // pipe path so that it can isolate its watchman integration tests,
  // but doesn't also specify --unix-listener-path then we need to
  // take care to prevent using the default unix domain path which would
  // otherwise break their isolation.
  // If either option is specified without the other, then we disable
  // the use of the other.
  if (!flags.named_pipe_path.empty() || !flags.unix_sock_name.empty()) {
    disable_named_pipe = flags.named_pipe_path.empty();
    disable_unix_socket = flags.unix_sock_name.empty();
  }

  if (flags.named_pipe_path.empty()) {
    flags.named_pipe_path =
        folly::to<std::string>("\\\\.\\pipe\\watchman-", user);
  }
#endif
  compute_file_name(flags.unix_sock_name, user, "sock", "sockname");

  compute_file_name(flags.watchman_state_file, user, "state", "statefile");
  compute_file_name(
      logging::log_name,
      user,
      "log",
      "logfile",
      /*require_absolute=*/logging::log_name != "-");

  if (flags.unix_sock_name.size() >= sizeof(un.sun_path) - 1) {
    log(FATAL, flags.unix_sock_name, ": path is too long\n");
  }
  un.sun_family = PF_LOCAL;
  memcpy(
      un.sun_path,
      flags.unix_sock_name.c_str(),
      flags.unix_sock_name.size() + 1);
}

static bool should_start(int err) {
  if (err == ECONNREFUSED) {
    return true;
  }
  if (err == ENOENT) {
    return true;
  }
  return false;
}

static bool try_command(json_t* cmd, int timeout) {
  auto client = w_stm_connect(timeout * 1000);
  if (!client) {
    return false;
  }

  // Start in a well-defined non-blocking state as we can't tell
  // what mode we're in on windows until we've set it to something
  // explicitly at least once before!
  client->setNonBlock(false);

  if (!cmd) {
    return true;
  }

  w_jbuffer_t buffer;
  w_jbuffer_t output_pdu_buffer;

  // Send command
  if (!buffer.pduEncodeToStream(
          server_pdu, server_capabilities, cmd, client.get())) {
    int err = errno;
    logf(ERR, "error sending PDU to server\n");
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
  } while (flags.persistent);

  return true;
}

static void parse_cmdline(int* argcp, char*** argvp) {
  cfg_load_global_config_file();

  watchman::parseOptions(argcp, argvp, &daemon_argv);
  watchman::getLog().setStdErrLoggingLevel(
      static_cast<enum watchman::LogLevel>(logging::log_level));
  setup_sock_name();
  parse_encoding(flags.server_encoding, &server_pdu);
  parse_encoding(flags.output_encoding, &output_pdu);
  if (flags.output_encoding.empty()) {
    output_pdu = flags.no_pretty ? is_json_compact : is_json_pretty;
  }

  // Prevent integration tests that call the watchman cli from
  // accidentally spawning a server.
  if (getenv("WATCHMAN_NO_SPAWN")) {
    flags.no_spawn = true;
  }

  if (Configuration().getBool("tcp-listener-enable", false)) {
    // hg requires the state-enter/state-leave commands, which are disabled over
    // TCP by default since at present it is unauthenticated. This should be
    // removed once TLS authentication is added to the TCP listener.
    // TODO: When this code is removed, lookup() can be changed to return a
    // const pointer.
    lookup_command("state-enter", CMD_DAEMON)->flags.set(CMD_ALLOW_ANY_USER);
    lookup_command("state-leave", CMD_DAEMON)->flags.set(CMD_ALLOW_ANY_USER);
  }
}

static json_ref build_command(int argc, char** argv) {
  // Read blob from stdin
  if (flags.json_input_arg) {
    auto err = json_error_t();
    w_jbuffer_t buf;

    auto cmd = buf.decodeNext(w_stm_stdin(), &err);

    if (buf.pdu_type == is_bser) {
      // If they used bser for the input, select bser for output
      // unless they explicitly requested something else
      if (flags.server_encoding.empty()) {
        server_pdu = is_bser;
      }
      if (flags.output_encoding.empty()) {
        output_pdu = is_bser;
      }
    } else if (buf.pdu_type == is_bser_v2) {
      // If they used bser v2 for the input, select bser v2 for output
      // unless they explicitly requested something else
      if (flags.server_encoding.empty()) {
        server_pdu = is_bser_v2;
      }
      if (flags.output_encoding.empty()) {
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
  for (int i = 0; i < argc; i++) {
    json_array_append_new(cmd, typed_string_to_json(argv[i], W_STRING_UNICODE));
  }

  return cmd;
}

static SpawnResult try_spawn_watchman() {
  // Every spawner that doesn't fork() this client process is susceptible to a
  // race condition if `watchman shutdown-server` and `watchman <command>` are
  // run in short order. The latter tries to spawn a daemon while the former is
  // still shutting down, holding the pid lock, and this causes it to time out
  // and fail. The solution would be to implement some kind of startup pipe that
  // allows the server to indicate to the client when it's done starting up,
  // communicating errors that are worthy of a retry.

#ifndef _WIN32
  if (flags.no_site_spawner) {
    // The astute reader will notice this we're calling run_service_as_daemon()
    // here and not the various other platform spawning functions in the block
    // further below in this function.  This is deliberate: we want
    // to do the most simple background running possible when the
    // no_site_spawner flag is used.   In the future we plan to
    // migrate the platform spawning functions to use the site_spawn
    // functionality.
    return run_service_as_daemon();
  }
  // If we have a site-specific spawning requirement, then we'll
  // invoke that spawner rather than using any of the built-in
  // spawning functionality.
  const char* site_spawn = cfg_get_string("spawn_watchman_service", nullptr);
  if (site_spawn) {
    return spawn_site_specific(site_spawn);
  }
#endif

#if defined(__APPLE__)
  return spawn_via_launchd();
#elif defined(_WIN32)
  return spawn_win32();
#else
  return run_service_as_daemon();
#endif
}

static int inner_main(int argc, char** argv) {
  // Since we don't fully integrate with folly, but may pull
  // in dependencies that do, we need to perform a little bit
  // of bootstrapping.  We don't want to run the full folly
  // init today because it will interfere with our own signal
  // handling.  In the future we will integrate this properly.
  folly::SingletonVault::singleton()->registrationComplete();
  SCOPE_EXIT {
    folly::SingletonVault::singleton()->destroyInstances();
  };

  parse_cmdline(&argc, &argv);

#ifdef _WIN32
  // On Windows its not possible to connect to elevated Watchman daemon from
  // non-elevated processes. To ensure that Watchman daemon will always be
  // accessible, deelevate by default if needed.
  // Note watchman runs in some environments which require elevated
  // permissions, so we can not always de-elevate.
  if (Configuration().getBool("should_deelevate_on_startup", false)) {
    deelevate_requires_normal_privileges();
  }
#endif

  if (flags.foreground) {
    run_service_in_foreground();
    return 0;
  }

  w_set_thread_name("cli");
  auto cmd = build_command(argc, argv);
  preprocess_command(cmd, output_pdu, output_capabilities);

  bool ran = try_command(cmd, 0);
  if (!ran && should_start(errno)) {
    if (flags.no_spawn) {
      if (!flags.no_local) {
        ran = try_client_mode_command(cmd, !flags.no_pretty);
      }
    } else {
      // Failed to run command. Try to spawn a daemon.

      // Some site spawner scripts will asynchronously launch the service.
      // When that happens we may encounter ECONNREFUSED.  We need to
      // tolerate this, so we add some retries.
      int attempts = 10;
      std::chrono::milliseconds interval{10};

      bool spawned = false;
      while (true) {
        if (!spawned) {
          auto spawn_result = try_spawn_watchman();
          switch (spawn_result.status) {
            case SpawnResult::Spawned:
              spawned = true;
              break;
            case SpawnResult::FailedToLock:
              // Otherwise, it's possible another daemon is still shutting down,
              // and we should try to start again next time. Alternatively,
              // another daemon is starting up, and when it's ready, the command
              // should succeed.
              break;
          }
        }

        ran = try_command(cmd, 10);
        if (!ran && should_start(errno) && attempts-- > 0) {
          /* sleep override */ std::this_thread::sleep_for(interval);
          // 10 doublings of 10 ms is about 10 seconds total.
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

  if (!flags.no_spawn) {
    log(ERR,
        "unable to talk to your watchman on ",
        get_sock_name_legacy(),
        "! (",
        folly::errnoStr(errno),
        ")\n");
#ifdef __APPLE__
    if (getenv("TMUX")) {
      logf(
          ERR,
          "\n"
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

int main(int argc, char** argv) {
  try {
    return inner_main(argc, argv);
  } catch (const std::exception& e) {
    log(ERR,
        "Uncaught C++ exception: ",
        folly::exceptionStr(e).toStdString(),
        "\n");
    return 1;
  } catch (...) {
    log(ERR, "Uncaught C++ exception: ...\n");
    return 1;
  }
}

/* vim:ts=2:sw=2:et:
 */
