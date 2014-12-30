/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

// Maps pid => process handle
// This is so that we can wait/poll/query the termination status
static w_ht_t *child_procs = NULL;
static pthread_mutex_t child_proc_lock = PTHREAD_MUTEX_INITIALIZER;

BOOL w_wait_for_any_child(DWORD timeoutms, DWORD *pid) {
  HANDLE handles[MAXIMUM_WAIT_OBJECTS];
  DWORD pids[MAXIMUM_WAIT_OBJECTS];
  int i = 0;
  w_ht_iter_t iter;
  DWORD res;

  *pid = 0;

  pthread_mutex_lock(&child_proc_lock);
  if (child_procs && w_ht_first(child_procs, &iter)) do {
    HANDLE proc = w_ht_val_ptr(iter.value);
    pids[i] = (DWORD)iter.key;
    handles[i++] = proc;
  } while (w_ht_next(child_procs, &iter));
  pthread_mutex_unlock(&child_proc_lock);

  if (i == 0) {
    return false;
  }

  w_log(W_LOG_DBG, "w_wait_for_any_child: waiting for %d handles\n", i);
  res = WaitForMultipleObjectsEx(i, handles, false, timeoutms, true);
  if (res == WAIT_FAILED) {
    errno = map_win32_err(GetLastError());
    return false;
  }

  if (res < WAIT_OBJECT_0 + i) {
    i = res - WAIT_OBJECT_0;
  } else if (res >= WAIT_ABANDONED_0 && res < WAIT_ABANDONED_0 + i) {
    i = res - WAIT_ABANDONED_0;
  } else {
    return false;
  }

  pthread_mutex_lock(&child_proc_lock);
  w_ht_del(child_procs, pids[i]);
  pthread_mutex_unlock(&child_proc_lock);

  CloseHandle(handles[i]);
  *pid = pids[i];
  return true;
}

int posix_spawnattr_init(posix_spawnattr_t *attrp) {
  memset(attrp, 0, sizeof(*attrp));
  return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attrp, int flags) {
  attrp->flags = flags;
  return 0;
}

int posix_spawnattr_setcwd_np(posix_spawnattr_t *attrp, const char *path) {
  char *path_dup = NULL;

  if (path) {
    path_dup = strdup(path);
    if (!path_dup) {
      return ENOMEM;
    }
  }

  free(attrp->working_dir);
  attrp->working_dir = path_dup;
  return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attrp) {
  free(attrp->working_dir);
  return 0;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions) {
  memset(actions, 0, sizeof(*actions));
  return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
    int fd, int target_fd) {
  struct _posix_spawn_file_dup *dups;
  dups = realloc(actions->dups, (actions->ndups + 1) * sizeof(*dups));
  if (!dups) {
    return ENOMEM;
  }
  dups[actions->ndups].local_handle = (HANDLE)_get_osfhandle(fd);
  dups[actions->ndups].target_fd = target_fd;
  actions->dups = dups;
  actions->ndups++;
  return 0;
}

int posix_spawn_file_actions_adddup2_handle_np(
    posix_spawn_file_actions_t *actions,
    HANDLE handle, int target_fd) {
  struct _posix_spawn_file_dup *dups;
  dups = realloc(actions->dups, (actions->ndups + 1) * sizeof(*dups));
  if (!dups) {
    return ENOMEM;
  }
  dups[actions->ndups].local_handle = handle,
  dups[actions->ndups].target_fd = target_fd;
  actions->dups = dups;
  actions->ndups++;
  return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
    int target_fd, const char *name, int flags, int mode) {
  struct _posix_spawn_file_open *opens;
  char *name_dup = strdup(name);
  if (!name_dup) {
    return ENOMEM;
  }
  opens = realloc(actions->opens, (actions->nopens + 1) * sizeof(*opens));
  if (!opens) {
    free(name_dup);
    return ENOMEM;
  }
  opens[actions->nopens].target_fd = target_fd;
  opens[actions->nopens].name = name_dup;
  opens[actions->nopens].flags = flags;
  opens[actions->nopens].mode = mode;
  actions->opens = opens;
  actions->nopens++;
  return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) {
  free(actions->dups);
  free(actions->opens);
  return 0;
}

#define CMD_EXE_PREFIX "cmd.exe /c \""
static char *build_command_line(char *const argv[]) {
  int argc = 0, i = 0;
  size_t size = 0;
  char *cmdbuf = NULL, *cur = NULL;

  // Note: includes trailing NUL which we count as the closing quote
  size = sizeof(CMD_EXE_PREFIX);

  for (argc = 0; argv[argc] != NULL; argc++) {
    size += 4 * (strlen(argv[argc]) + 1);
  }

  cmdbuf = malloc(size);
  if (!cmdbuf) {
    return NULL;
  }

  // Here be dragons.  More gory details in http://stackoverflow.com/q/4094699
  // Surely not complete here by any means

  strcpy_s(cmdbuf, size, CMD_EXE_PREFIX);
  cur = cmdbuf + strlen(CMD_EXE_PREFIX);
  for (i = 0; i < argc; i++) {
    int j;
    char *arg = argv[i];

    // Space separated
    if (i > 0) {
      *cur = ' ';
      cur++;
    }

    *cur = '"';
    cur++;

    // FIXME: multibyte
    for (j = 0; arg[j]; j++) {
      switch (arg[j]) {
        case '"':
          strcpy(cur, "\"\"\"");
          cur += 3;
          break;
        default:
          *cur = arg[j];
          cur++;
      }
    }
    *cur = '"';
    cur++;
  }
  *cur = '"';
  cur++;
  *cur = 0;
  return cmdbuf;
}

static char *make_env_block(char *const envp[]) {
  int i;
  size_t total_len = 1; /* for final NUL */
  char *block = NULL;
  char *target = NULL;

  for (i = 0; envp[i]; i++) {
    total_len += strlen(envp[i]) + 1;
  }

  block = malloc(total_len);
  if (!block) {
    return NULL;
  }

  target = block;

  for (i = 0; envp[i]; i++) {
    size_t len = strlen(envp[i]);
    // Also copy the NULL
    memcpy(target, envp[i], len + 1);
    target += len + 1;
  }

  // Final NUL terminator
  *target = 0;

  return block;
}

static int posix_spawn_common(
    bool search_path,
    pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *file_actions,
    const posix_spawnattr_t *attrp,
    char *const argv[], char *const envp[]) {
  STARTUPINFO sinfo;
  SECURITY_ATTRIBUTES sec;
  PROCESS_INFORMATION pinfo;
  char *cmdbuf;
  char *env_block;
  DWORD create_flags = CREATE_NO_WINDOW;
  int ret;
  int i;
  unused_parameter(envp); // FIXME

  cmdbuf = build_command_line(argv);
  if (!cmdbuf) {
    return ENOMEM;
  }

  env_block = make_env_block(envp);
  if (!env_block) {
    free(cmdbuf);
    return ENOMEM;
  }

  memset(&sinfo, 0, sizeof(sinfo));
  sinfo.cb = sizeof(sinfo);
  sinfo.dwFlags = STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
  sinfo.wShowWindow = SW_HIDE;

  memset(&sec, 0, sizeof(sec));
  sec.nLength = sizeof(sec);
  sec.bInheritHandle = TRUE;

  memset(&pinfo, 0, sizeof(pinfo));

  if (attrp->flags & POSIX_SPAWN_SETPGROUP) {
    create_flags |= CREATE_NEW_PROCESS_GROUP;
  }

  // Process any dup(2) actions
  for (i = 0; i < file_actions->ndups; i++) {
    struct _posix_spawn_file_dup *dup = &file_actions->dups[i];
    HANDLE *target = NULL;
    DWORD err;

    switch (dup->target_fd) {
      case 0:
        target = &sinfo.hStdInput;
        break;
      case 1:
        target = &sinfo.hStdOutput;
        break;
      case 2:
        target = &sinfo.hStdError;
        break;
    }

    if (!target) {
      w_log(W_LOG_ERR, "posix_spawn: can't target fd outside range [0-2]\n");
      ret = ENOSYS;
      goto done;
    }

    if (*target) {
      CloseHandle(*target);
      *target = INVALID_HANDLE_VALUE;
    }

    if (!DuplicateHandle(GetCurrentProcess(), dup->local_handle,
          GetCurrentProcess(), target, 0,
          TRUE, DUPLICATE_SAME_ACCESS)) {
      err = GetLastError();
      w_log(W_LOG_ERR, "posix_spawn: failed to duplicate handle: %s\n",
          win32_strerror(err));
      ret = map_win32_err(err);
      goto done;
    }

    w_log(W_LOG_ERR, "duplicated handle %p -> handle %p for fd=%d\n",
        dup->local_handle, *target, dup->target_fd);
  }

  // Process any file opening actions
  for (i = 0; i < file_actions->nopens; i++) {
    struct _posix_spawn_file_open *op = &file_actions->opens[i];
    HANDLE h;
    HANDLE *target = NULL;

    switch (op->target_fd) {
      case 0:
        target = &sinfo.hStdInput;
        break;
      case 1:
        target = &sinfo.hStdOutput;
        break;
      case 2:
        target = &sinfo.hStdError;
        break;
    }

    if (!target) {
      w_log(W_LOG_ERR, "posix_spawn: can't target fd outside range [0-2]\n");
      ret = ENOSYS;
      goto done;
    }

    h = w_handle_open(op->name, op->flags);
    if (h == INVALID_HANDLE_VALUE) {
      ret = errno;
      w_log(W_LOG_ERR, "posix_spawn: failed to open %s:\n",
          op->name);
      goto done;
    }

    if (*target) {
      CloseHandle(*target);
    }
    *target = h;
    w_log(W_LOG_ERR, "opened file %s as handle %p fd=%d\n",
        op->name, h, op->target_fd);
  }

  if (!sinfo.hStdInput) {
    sinfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  if (!sinfo.hStdOutput) {
    sinfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  }
  if (!sinfo.hStdError) {
    sinfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }

  if (!CreateProcess(search_path ? NULL : path,
        cmdbuf, &sec, &sec, TRUE, create_flags, env_block,
        attrp->working_dir, &sinfo, &pinfo)) {
    w_log(W_LOG_ERR, "CreateProcess: `%s`: (cwd=%s) %s\n", cmdbuf,
        attrp->working_dir ? attrp->working_dir : "<process cwd>",
        win32_strerror(GetLastError()));
    ret = EACCES;
  } else {
    *pid = (pid_t)pinfo.dwProcessId;

    // Record the pid -> handle mapping for later wait/reap
    pthread_mutex_lock(&child_proc_lock);
    if (!child_procs) {
      child_procs = w_ht_new(2, NULL);
    }
    w_ht_set(child_procs, pinfo.dwProcessId, w_ht_ptr_val(pinfo.hProcess));
    pthread_mutex_unlock(&child_proc_lock);

    CloseHandle(pinfo.hThread);
    ret = 0;
  }

done:
  free(cmdbuf);
  free(env_block);

  // If we manufactured any handles, close them out now
  if (sinfo.hStdInput != GetStdHandle(STD_INPUT_HANDLE)) {
    CloseHandle(sinfo.hStdInput);
  }
  if (sinfo.hStdOutput != GetStdHandle(STD_OUTPUT_HANDLE)) {
    CloseHandle(sinfo.hStdOutput);
  }
  if (sinfo.hStdError != GetStdHandle(STD_ERROR_HANDLE)) {
    CloseHandle(sinfo.hStdError);
  }

  return ret;
}

int posix_spawn(pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *file_actions,
    const posix_spawnattr_t *attrp,
    char *const argv[], char *const envp[]) {

  return posix_spawn_common(false, pid, path, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
    const posix_spawn_file_actions_t *file_actions,
    const posix_spawnattr_t *attrp,
    char *const argv[], char *const envp[]) {

  return posix_spawn_common(true, pid, file, file_actions, attrp, argv, envp);
}
