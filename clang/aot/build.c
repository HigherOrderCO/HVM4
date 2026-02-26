// AOT Module: Build Orchestrator
// ------------------------------
// Emits standalone AOT C programs, supports one-shot `--as-c` execution,
// and supports writing a native executable with `-o/--output`.

#include <time.h>

#if HVM_WINDOWS
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <process.h>
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

fn int aot_build_is_path_sep(char ch) {
  return ch == '/' || ch == '\\';
}

fn char *aot_build_path_join(const char *dir, const char *file) {
  if (dir == NULL) {
    dir = "";
  }
  if (file == NULL) {
    file = "";
  }

  size_t dir_len = strlen(dir);
  size_t file_len = strlen(file);
  int need_sep = dir_len > 0 && !aot_build_is_path_sep(dir[dir_len - 1]);

  size_t out_len = dir_len + (size_t)need_sep + file_len + 1;
  char *out = malloc(out_len);
  if (out == NULL) {
    return NULL;
  }

  memcpy(out, dir, dir_len);
  size_t at = dir_len;
  if (need_sep) {
    out[at++] = PATH_SEP;
  }
  memcpy(out + at, file, file_len);
  out[at + file_len] = '\0';
  return out;
}

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

#if HVM_WINDOWS
fn char *aot_build_realpath(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return NULL;
  }

  DWORD need = GetFullPathNameA(path, 0, NULL, NULL);
  if (need == 0) {
    return NULL;
  }

  char *abs = malloc((size_t)need);
  if (abs == NULL) {
    return NULL;
  }

  DWORD got = GetFullPathNameA(path, need, abs, NULL);
  if (got == 0 || got >= need) {
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

// Resolves argv0 to an absolute executable path.
fn char *aot_build_resolve_argv0(const char *argv0) {
  if (argv0 == NULL || argv0[0] == '\0') {
    return NULL;
  }

#if HVM_WINDOWS
  if (strchr(argv0, '/') != NULL || strchr(argv0, '\\') != NULL) {
    return aot_build_realpath(argv0);
  }
#else
  if (strchr(argv0, '/') != NULL) {
    return aot_build_realpath(argv0);
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

    char *cand = aot_build_path_join(dir, argv0);
    if (cand == NULL) {
      free(buf);
      return NULL;
    }

    if (access(cand, X_OK) == 0) {
      char *abs = aot_build_realpath(cand);
      free(cand);
      free(buf);
      return abs;
    }

    free(cand);
  }

  free(buf);
  return NULL;
}

// Resolves absolute runtime path (`.../clang/hvm.c`) from argv0.
fn char *aot_build_runtime_path(const char *argv0) {
  char *exe = aot_build_resolve_argv0(argv0);
  if (exe == NULL) {
    sys_error("failed to resolve executable path from argv[0]");
  }

  char *slash = strrchr(exe, '/');
  char *backslash = strrchr(exe, '\\');
  char *sep = slash;
  if (backslash != NULL && (sep == NULL || backslash > sep)) {
    sep = backslash;
  }
  if (sep == NULL) {
    free(exe);
    sys_error("invalid executable path");
  }

  if (sep == exe && aot_build_is_path_sep(exe[0])) {
    sep[1] = '\0';
  } else {
    *sep = '\0';
  }
  char *runtime_rel = aot_build_path_join(exe, "hvm.c");
  free(exe);
  if (runtime_rel == NULL) {
    sys_error("failed to allocate runtime path");
  }

  char *runtime_abs = aot_build_realpath(runtime_rel);
  if (runtime_abs == NULL) {
    fprintf(stderr, "ERROR: failed to resolve runtime file '%s'\n", runtime_rel);
    free(runtime_rel);
    exit(1);
  }

  free(runtime_rel);
  return runtime_abs;
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
#if HVM_WINDOWS
  char *const cmd[] = {
    "clang",
    "-O2",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-D_CRT_NONSTDC_NO_WARNINGS",
    "-Wno-deprecated-declarations",
    "-o",
    (char *)out_path,
    (char *)c_path,
    NULL,
  };
#else
  char *const cmd[] = {
    "clang",
    "-O2",
    "-o",
    (char *)out_path,
    (char *)c_path,
    NULL,
  };
#endif

  return aot_build_spawn(cmd);
}

// Writes one AOT C file with an absolute runtime include path.
fn void aot_write_c_file(const char *c_path, const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  char *runtime_path = aot_build_runtime_path(argv0);
  aot_emit(c_path, runtime_path, src_path, src_text, cfg);
  free(runtime_path);
}

// Creates one temporary build directory and returns it.
fn char *aot_build_temp_dir(void) {
  const char *tmp = getenv("HVM_TMPDIR");
#if HVM_WINDOWS
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
  char name[64];
  int name_n = snprintf(name, sizeof(name), "hvm-aot.%lu.%u", (unsigned long)GetCurrentProcessId(), counter++);
  if (name_n < 0 || name_n >= (int)sizeof(name)) {
    sys_error("failed to build AOT temp dir name");
  }

  char *dir = aot_build_path_join(tmp, name);
  if (dir == NULL) {
    sys_error("malloc failed");
  }

  if (_mkdir(dir) != 0) {
    free(dir);
    sys_error("failed to create AOT temp directory");
  }
  return dir;
#else
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = getenv("TMPDIR");
  }
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = "/tmp";
  }

  char *templ = aot_build_path_join(tmp, "hvm-aot.XXXXXX");
  if (templ == NULL) {
    sys_error("malloc failed");
  }

  if (mkdtemp(templ) == NULL) {
    free(templ);
    sys_error("failed to create AOT temp directory");
  }
  return templ;
#endif
}

// Joins one directory + filename.
fn char *aot_build_join(const char *dir, const char *file) {
  char *path = aot_build_path_join(dir, file);
  if (path == NULL) {
    sys_error("malloc failed");
  }
  return path;
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
  char *runtime_path = aot_build_runtime_path(argv0);
  aot_emit_stdout(runtime_path, src_path, src_text, cfg);
  free(runtime_path);
}

// Emits + compiles + runs once, then removes all temporary files.
fn int aot_build_as_c_once(const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  int rc = 1;
  char *tmp_dir = aot_build_temp_dir();
  char *c_path = aot_build_join(tmp_dir, "main.c");
#if HVM_WINDOWS
  char *x_path = aot_build_join(tmp_dir, "main.exe");
#else
  char *x_path = aot_build_join(tmp_dir, "main.bin");
#endif
  int timed = aot_build_timing_enabled();
  double t0 = timed ? aot_build_now_ms() : 0.0;
  double tp = t0;

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
    free(x_path);
    free(c_path);
    free(tmp_dir);
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
  free(x_path);
  free(c_path);
  free(tmp_dir);
  return rc;
}

// Emits + compiles a native executable to `out_path`.
fn int aot_build_to_output(const char *argv0, const char *src_path, const char *src_text, const char *out_path, const AotBuildCfg *cfg) {
  int rc = 1;
  char *tmp_dir = aot_build_temp_dir();
  char *c_path = aot_build_join(tmp_dir, "main.c");
  int timed = aot_build_timing_enabled();
  double t0 = timed ? aot_build_now_ms() : 0.0;
  double tp = t0;

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
  free(c_path);
  free(tmp_dir);
  return rc;
}
