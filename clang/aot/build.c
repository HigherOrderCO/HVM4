// AOT Module: Build Orchestrator
// ------------------------------
// Emits standalone AOT C programs, supports one-shot `--as-c` execution,
// and supports writing a native executable with `-o/--output`.

#include <limits.h>
#include <time.h>

#if HVM_WINDOWS
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <process.h>
  #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
  #endif
  #ifndef X_OK
    #define X_OK 0
  #endif
#else
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

fn void  aot_emit(const char *c_path, const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg);
fn void  aot_emit_stdout(const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg);
fn void  sys_error(const char *msg);

#if HVM_WINDOWS
fn double aot_build_now_ms(void) {
  static LARGE_INTEGER freq;
  static int has_freq = 0;
  LARGE_INTEGER now;
  if (!has_freq) {
    if (!QueryPerformanceFrequency(&freq)) {
      return 0.0;
    }
    has_freq = 1;
  }
  if (!QueryPerformanceCounter(&now)) {
    return 0.0;
  }
  return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
fn double aot_build_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

fn int aot_build_timing_enabled(void) {
  const char *env = getenv("HVM_AOT_TIMING");
  return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

fn void aot_build_timing_log(const char *phase, double ms) {
  fprintf(stderr, "AOT_TIMING %s: %.3f ms\n", phase, ms);
}

// Resolves argv0 to an absolute executable path.
fn char *aot_build_resolve_argv0(const char *argv0) {
  if (argv0 == NULL || argv0[0] == '\0') {
    return NULL;
  }

#if HVM_WINDOWS
  if (strchr(argv0, '/') != NULL || strchr(argv0, '\\') != NULL) {
    char *abs = malloc(PATH_MAX);
    if (abs == NULL) {
      return NULL;
    }
    if (GetFullPathNameA(argv0, PATH_MAX, abs, NULL) == 0) {
      free(abs);
      return NULL;
    }
    return abs;
  }
#else
  if (strchr(argv0, '/') != NULL) {
    return realpath(argv0, NULL);
  }
#endif

  const char *path_env = getenv("PATH");
  if (path_env == NULL || path_env[0] == '\0') {
    return NULL;
  }

  char *buf = strdup(path_env);
  if (buf == NULL) {
    return NULL;
  }

  char *save = NULL;
#if HVM_WINDOWS
  for (char *dir = strtok_s(buf, ";", &save); dir != NULL; dir = strtok_s(NULL, ";", &save)) {
#else
  for (char *dir = strtok_r(buf, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
#endif
    if (dir[0] == '\0') {
      continue;
    }

    char cand[PATH_MAX];
#if HVM_WINDOWS
    int n = snprintf(cand, sizeof(cand), "%s\\%s", dir, argv0);
#else
    int n = snprintf(cand, sizeof(cand), "%s/%s", dir, argv0);
#endif
    if (n < 0 || n >= (int)sizeof(cand)) {
      continue;
    }

    if (access(cand, X_OK) == 0) {
#if HVM_WINDOWS
      char *abs = malloc(PATH_MAX);
      if (abs == NULL) {
        free(buf);
        return NULL;
      }
      if (GetFullPathNameA(cand, PATH_MAX, abs, NULL) == 0) {
        free(abs);
        free(buf);
        return NULL;
      }
      free(buf);
      return abs;
#else
      char *abs = realpath(cand, NULL);
      free(buf);
      return abs;
#endif
    }
  }

  free(buf);
  return NULL;
}

#if HVM_WINDOWS
fn char *aot_build_realpath(const char *path) {
  char *abs = malloc(PATH_MAX);
  if (abs == NULL) {
    return NULL;
  }
  if (GetFullPathNameA(path, PATH_MAX, abs, NULL) == 0) {
    free(abs);
    return NULL;
  }
  return abs;
}
#else
fn char *aot_build_realpath(const char *path) {
  return realpath(path, NULL);
}
#endif

// Resolves absolute runtime path (`.../clang/hvm.c`) from argv0.
fn void aot_build_runtime_path(char *out, u32 out_len, const char *argv0) {
  char *exe = aot_build_resolve_argv0(argv0);
  if (exe == NULL) {
    sys_error("failed to resolve executable path from argv[0]");
  }

#if HVM_WINDOWS
  char *slash = strrchr(exe, '\\');
  if (slash == NULL) {
    slash = strrchr(exe, '/');
  }
#else
  char *slash = strrchr(exe, '/');
#endif
  if (slash == NULL) {
    free(exe);
    sys_error("invalid executable path");
  }

  *slash = '\0';
#if HVM_WINDOWS
  int n = snprintf(out, out_len, "%s\\hvm.c", exe);
#else
  int n = snprintf(out, out_len, "%s/hvm.c", exe);
#endif
  free(exe);

  if (n < 0 || n >= (int)out_len) {
    sys_error("runtime path too long");
  }

  char *abs = aot_build_realpath(out);
  if (abs == NULL) {
    fprintf(stderr, "ERROR: failed to resolve runtime file '%s'\n", out);
    exit(1);
  }

  n = snprintf(out, out_len, "%s", abs);
  free(abs);
  if (n < 0 || n >= (int)out_len) {
    sys_error("runtime path too long");
  }
}

// Runs one process and returns its exit code.
fn int aot_build_spawn(char *const argv[]) {
#if HVM_WINDOWS
  int len = 1;
  for (int i = 0; argv[i] != NULL; ++i) {
    len += (int)strlen(argv[i]) + 3;
  }

  char *cmdline = malloc((size_t)len);
  if (cmdline == NULL) {
    sys_error("malloc failed");
  }
  cmdline[0] = '\0';

  for (int i = 0; argv[i] != NULL; ++i) {
    if (i > 0) {
      strcat(cmdline, " ");
    }
    if (strchr(argv[i], ' ') != NULL) {
      strcat(cmdline, "\"");
      strcat(cmdline, argv[i]);
      strcat(cmdline, "\"");
    } else {
      strcat(cmdline, argv[i]);
    }
  }

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);

  if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    free(cmdline);
    return 127;
  }

  free(cmdline);

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD code = 1;
  if (!GetExitCodeProcess(pi.hProcess, &code)) {
    code = 1;
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)code;
#else
  pid_t pid = fork();
  if (pid < 0) {
    sys_error("fork failed");
  }

  if (pid == 0) {
    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    sys_error("waitpid failed");
  }

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }

  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    const char *sig_name = strsignal(sig);
    if (sig_name == NULL) {
      sig_name = "unknown signal";
    }
    fprintf(stderr, "ERROR: child process terminated by signal. %s\n", sig_name);
    return 128 + sig;
  }

  return 1;
#endif
}

// Compiles one generated C program into an executable.
fn int aot_build_compile(const char *c_path, const char *out_path) {
  char *const cmd[] = {
    "clang",
    "-O2",
    "-o",
    (char *)out_path,
    (char *)c_path,
    NULL,
  };

  return aot_build_spawn(cmd);
}

// Writes one AOT C file with an absolute runtime include path.
fn void aot_write_c_file(const char *c_path, const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  char runtime_path[PATH_MAX];
  aot_build_runtime_path(runtime_path, sizeof(runtime_path), argv0);
  aot_emit(c_path, runtime_path, src_path, src_text, cfg);
}

// Builds one mkdtemp template path using TMPDIR or /tmp.
fn void aot_build_temp_template(char *out, u32 out_len) {
#if HVM_WINDOWS
  const char *tmp = getenv("HVM_TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = getenv("TEMP");
  }
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = getenv("TMP");
  }
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = ".";
  }

  static unsigned counter = 0;
  int n = snprintf(out, out_len, "%s\\hvm-aot.%lu.%u", tmp, (unsigned long)GetCurrentProcessId(), counter++);
  if (n < 0 || n >= (int)out_len) {
    sys_error("AOT temp directory path too long");
  }
#else
  const char *tmp = getenv("HVM_TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = getenv("TMPDIR");
  }
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = "/tmp";
  }

  int n = snprintf(out, out_len, "%s/hvm-aot.XXXXXX", tmp);
  if (n < 0 || n >= (int)out_len) {
    sys_error("AOT temp directory path too long");
  }
#endif
}

// Creates one temporary build directory and writes it to `out`.
fn void aot_build_temp_dir(char *out, u32 out_len) {
  aot_build_temp_template(out, out_len);
#if HVM_WINDOWS
  if (_mkdir(out) != 0) {
    sys_error("failed to create AOT temp directory");
  }
#else
  if (mkdtemp(out) == NULL) {
    sys_error("failed to create AOT temp directory");
  }
#endif
}

// Joins one directory + filename into `out`.
fn void aot_build_join(char *out, u32 out_len, const char *dir, const char *file) {
#if HVM_WINDOWS
  int n = snprintf(out, out_len, "%s\\%s", dir, file);
#else
  int n = snprintf(out, out_len, "%s/%s", dir, file);
#endif
  if (n < 0 || n >= (int)out_len) {
    sys_error("AOT temp path too long");
  }
}

// Removes temporary C/executable files and then removes the temp directory.
fn void aot_build_cleanup(const char *tmp_dir, const char *c_path, const char *x_path) {
  if (x_path != NULL && x_path[0] != '\0') {
#if HVM_WINDOWS
    _unlink(x_path);
#else
    unlink(x_path);
#endif
  }
  if (c_path != NULL && c_path[0] != '\0') {
#if HVM_WINDOWS
    _unlink(c_path);
#else
    unlink(c_path);
#endif
  }
  if (tmp_dir != NULL && tmp_dir[0] != '\0') {
#if HVM_WINDOWS
    _rmdir(tmp_dir);
#else
    rmdir(tmp_dir);
#endif
  }
}

// Emits only C code to stdout.
fn void aot_build_to_c(const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  char runtime_path[PATH_MAX];
  aot_build_runtime_path(runtime_path, sizeof(runtime_path), argv0);
  aot_emit_stdout(runtime_path, src_path, src_text, cfg);
}

// Emits + compiles + runs once, then removes all temporary files.
fn int aot_build_as_c_once(const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  int  rc = 1;
  char tmp_dir[PATH_MAX];
  char c_path[PATH_MAX];
  char x_path[PATH_MAX];
  int timed = aot_build_timing_enabled();
  double t0 = timed ? aot_build_now_ms() : 0.0;
  double tp = t0;

  aot_build_temp_dir(tmp_dir, sizeof(tmp_dir));
  aot_build_join(c_path, sizeof(c_path), tmp_dir, "main.c");
#if HVM_WINDOWS
  aot_build_join(x_path, sizeof(x_path), tmp_dir, "main.exe");
#else
  aot_build_join(x_path, sizeof(x_path), tmp_dir, "main.bin");
#endif
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("prepare", now - tp);
    tp = now;
  }

  aot_write_c_file(c_path, argv0, src_path, src_text, cfg);
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("emit", now - tp);
    tp = now;
  }
  rc = aot_build_compile(c_path, x_path);
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("clang", now - tp);
    tp = now;
  }
  if (rc != 0) {
    fprintf(stderr, "ERROR: failed to compile AOT program '%s'\n", c_path);
    aot_build_cleanup(tmp_dir, c_path, x_path);
    if (timed) {
      double now = aot_build_now_ms();
      aot_build_timing_log("cleanup", now - tp);
      aot_build_timing_log("total", now - t0);
    }
    return rc;
  }

  char *const run[] = {
    x_path,
    NULL,
  };

  rc = aot_build_spawn(run);
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("run", now - tp);
    tp = now;
  }
  aot_build_cleanup(tmp_dir, c_path, x_path);
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("cleanup", now - tp);
    aot_build_timing_log("total", now - t0);
  }
  return rc;
}

// Emits + compiles a native executable to `out_path`.
fn int aot_build_to_output(const char *argv0, const char *src_path, const char *src_text, const char *out_path, const AotBuildCfg *cfg) {
  int  rc = 1;
  char tmp_dir[PATH_MAX];
  char c_path[PATH_MAX];
  int timed = aot_build_timing_enabled();
  double t0 = timed ? aot_build_now_ms() : 0.0;
  double tp = t0;

  aot_build_temp_dir(tmp_dir, sizeof(tmp_dir));
  aot_build_join(c_path, sizeof(c_path), tmp_dir, "main.c");
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("prepare", now - tp);
    tp = now;
  }
  aot_write_c_file(c_path, argv0, src_path, src_text, cfg);
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("emit", now - tp);
    tp = now;
  }

  rc = aot_build_compile(c_path, out_path);
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("clang", now - tp);
    tp = now;
  }
  if (rc != 0) {
    fprintf(stderr, "ERROR: failed to compile AOT executable '%s'\n", out_path);
  }

  aot_build_cleanup(tmp_dir, c_path, NULL);
  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("cleanup", now - tp);
    aot_build_timing_log("total", now - t0);
  }
  return rc;
}
