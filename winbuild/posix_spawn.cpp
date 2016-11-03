/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include "watchman_synchronized.h"

// Maps pid => process handle
// This is so that we can wait/poll/query the termination status
static watchman::Synchronized<std::unordered_map<DWORD, HANDLE>> child_procs;

BOOL w_wait_for_any_child(DWORD timeoutms, DWORD *pid) {
  HANDLE handles[MAXIMUM_WAIT_OBJECTS];
  DWORD pids[MAXIMUM_WAIT_OBJECTS];
  int i = 0;
  DWORD res;

  *pid = 0;

  {
    auto rlock = child_procs.rlock();
    for (auto& it : *rlock) {
      pids[i] = it.first;
      handles[i++] = it.second;
    }
  }

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

  child_procs.wlock()->erase(pids[i]);

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
  struct _posix_spawn_file_action *acts, *act;
  acts = (_posix_spawn_file_action *)realloc(
      actions->acts, (actions->nacts + 1) * sizeof(*acts));
  if (!acts) {
    return ENOMEM;
  }
  act = &acts[actions->nacts];
  act->action = _posix_spawn_file_action::dup_fd;
  act->u.source_fd = fd;
  act->target_fd = target_fd;
  actions->acts = acts;
  actions->nacts++;
  return 0;
}

int posix_spawn_file_actions_adddup2_handle_np(
    posix_spawn_file_actions_t *actions,
    HANDLE handle, int target_fd) {
  struct _posix_spawn_file_action *acts, *act;
  acts = (_posix_spawn_file_action *)realloc(
      actions->acts, (actions->nacts + 1) * sizeof(*acts));
  if (!acts) {
    return ENOMEM;
  }
  act = &acts[actions->nacts];
  act->action = _posix_spawn_file_action::dup_handle;
  act->u.dup_local_handle = handle;
  act->target_fd = target_fd;
  actions->acts = acts;
  actions->nacts++;
  return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
    int target_fd, const char *name, int flags, int mode) {
  struct _posix_spawn_file_action *acts, *act;
  char *name_dup = strdup(name);
  if (!name_dup) {
    return ENOMEM;
  }
  acts = (_posix_spawn_file_action *)realloc(
      actions->acts, (actions->nacts + 1) * sizeof(*acts));
  if (!acts) {
    free(name_dup);
    return ENOMEM;
  }
  act = &acts[actions->nacts];
  act->action = _posix_spawn_file_action::open_file;
  act->target_fd = target_fd;
  act->u.open_info.name = name_dup;
  act->u.open_info.flags = flags;
  act->u.open_info.mode = mode;

  actions->acts = acts;
  actions->nacts++;
  return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) {
  int i;
  for (i = 0; i < actions->nacts; i++) {
    if (actions->acts[i].action != _posix_spawn_file_action::open_file) {
      continue;
    }
    free(actions->acts[i].u.open_info.name);
  }
  free(actions->acts);
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

  cmdbuf = (char*)malloc(size);
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

  block = (char*)malloc(total_len);
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
  STARTUPINFOEX sinfo;
  SECURITY_ATTRIBUTES sec;
  PROCESS_INFORMATION pinfo;
  char *cmdbuf;
  char *env_block;
  DWORD create_flags = CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT;
  int ret;
  int i;
  HANDLE inherited_handles[3] = {0, 0, 0};

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
  sinfo.StartupInfo.cb = sizeof(sinfo);
  sinfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
  sinfo.StartupInfo.wShowWindow = SW_HIDE;

  memset(&sec, 0, sizeof(sec));
  sec.nLength = sizeof(sec);
  sec.bInheritHandle = TRUE;

  memset(&pinfo, 0, sizeof(pinfo));

  if (attrp->flags & POSIX_SPAWN_SETPGROUP) {
    create_flags |= CREATE_NEW_PROCESS_GROUP;
  }

  for (i = 0; i < file_actions->nacts; i++) {
    struct _posix_spawn_file_action *act = &file_actions->acts[i];
    HANDLE *target = NULL;

    switch (act->target_fd) {
      case 0:
        target = &sinfo.StartupInfo.hStdInput;
        break;
      case 1:
        target = &sinfo.StartupInfo.hStdOutput;
        break;
      case 2:
        target = &sinfo.StartupInfo.hStdError;
        break;
    }

    if (!target) {
      w_log(W_LOG_ERR, "posix_spawn: can't target fd outside range [0-2]\n");
      ret = ENOSYS;
      goto done;
    }

    if (act->action != _posix_spawn_file_action::open_file) {
      // Process a dup(2) action
      DWORD err;

      if (*target) {
        CloseHandle(*target);
        *target = INVALID_HANDLE_VALUE;
      }

      if (act->action == _posix_spawn_file_action::dup_fd) {
        HANDLE src = NULL;
        switch (act->u.source_fd) {
          case 0:
            src = sinfo.StartupInfo.hStdInput;
            break;
          case 1:
            src = sinfo.StartupInfo.hStdOutput;
            break;
          case 2:
            src = sinfo.StartupInfo.hStdError;
            break;
        }
        if (!src) {
          src = (HANDLE)_get_osfhandle(act->u.source_fd);
        }
        act->u.dup_local_handle = src;
      }

      if (!DuplicateHandle(GetCurrentProcess(), act->u.dup_local_handle,
            GetCurrentProcess(), target, 0,
            TRUE, DUPLICATE_SAME_ACCESS)) {
        err = GetLastError();
        w_log(W_LOG_ERR, "posix_spawn: failed to duplicate handle: %s\n",
            win32_strerror(err));
        ret = map_win32_err(err);
        goto done;
      }
    } else {
      // Process an open(2) action
      HANDLE h;

      h = w_handle_open(act->u.open_info.name,
              act->u.open_info.flags & ~O_CLOEXEC);
      if (h == INVALID_HANDLE_VALUE) {
        ret = errno;
        w_log(W_LOG_ERR, "posix_spawn: failed to open %s:\n",
            act->u.open_info.name);
        goto done;
      }

      if (*target) {
        CloseHandle(*target);
      }
      *target = h;
    }
  }

  if (!sinfo.StartupInfo.hStdInput) {
    sinfo.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  if (!sinfo.StartupInfo.hStdOutput) {
    sinfo.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  }
  if (!sinfo.StartupInfo.hStdError) {
    sinfo.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }

  // Ensure that we only pass the stdio handles to the child.
  {
    SIZE_T size = 0;

    inherited_handles[0] = sinfo.StartupInfo.hStdInput;
    inherited_handles[1] = sinfo.StartupInfo.hStdOutput;
    inherited_handles[2] = sinfo.StartupInfo.hStdError;
    sinfo.lpAttributeList = NULL;

    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    sinfo.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(size);
    InitializeProcThreadAttributeList(sinfo.lpAttributeList, 1, 0, &size);
    UpdateProcThreadAttribute(sinfo.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited_handles, 3 * sizeof(HANDLE),
        NULL, NULL);
  }

  if (!CreateProcess(search_path ? NULL : path,
        cmdbuf, &sec, &sec, TRUE, create_flags, env_block,
        attrp->working_dir, &sinfo.StartupInfo, &pinfo)) {
    w_log(W_LOG_ERR, "CreateProcess: `%s`: (cwd=%s) %s\n", cmdbuf,
        attrp->working_dir ? attrp->working_dir : "<process cwd>",
        win32_strerror(GetLastError()));
    ret = EACCES;
  } else {
    *pid = (pid_t)pinfo.dwProcessId;

    // Record the pid -> handle mapping for later wait/reap
    child_procs.wlock()->emplace(pinfo.dwProcessId, pinfo.hProcess);

    CloseHandle(pinfo.hThread);
    ret = 0;
  }

  free(sinfo.lpAttributeList);

done:
  free(cmdbuf);
  free(env_block);

  // If we manufactured any handles, close them out now
  if (sinfo.StartupInfo.hStdInput != GetStdHandle(STD_INPUT_HANDLE)) {
    CloseHandle(sinfo.StartupInfo.hStdInput);
  }
  if (sinfo.StartupInfo.hStdOutput != GetStdHandle(STD_OUTPUT_HANDLE)) {
    CloseHandle(sinfo.StartupInfo.hStdOutput);
  }
  if (sinfo.StartupInfo.hStdError != GetStdHandle(STD_ERROR_HANDLE)) {
    CloseHandle(sinfo.StartupInfo.hStdError);
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
