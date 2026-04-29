// AOT Builder
// ===========
// Emits, compiles, and runs standalone AOT programs.

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || \
    (defined(__APPLE__) && TARGET_OS_IPHONE) || \
    defined(HVM_NO_AOT_BUILD)

// Sandboxed platforms (WASM, iOS, Android) cannot fork/exec processes.
// AOT compilation is a development-time feature for desktop/server.
// Provide stubs that report the error clearly.

fn void aot_build_to_c(const char *argv0, const char *src_path,
                        const char *src_text, const AotBuildCfg *cfg) {
  (void)argv0; (void)src_path; (void)src_text; (void)cfg;
  fprintf(stderr,
    "ERROR: AOT compilation not available on this platform\n");
  exit(1);
}

fn int aot_build_as_c_once(const char *argv0, const char *src_path,
                            const char *src_text, const AotBuildCfg *cfg) {
  (void)argv0; (void)src_path; (void)src_text; (void)cfg;
  fprintf(stderr,
    "ERROR: AOT compilation not available on this platform\n");
  return 1;
}

fn int aot_build_to_output(const char *argv0, const char *src_path,
                            const char *src_text, const char *out_path,
                            const AotBuildCfg *cfg) {
  (void)argv0; (void)src_path; (void)src_text;
  (void)out_path; (void)cfg;
  fprintf(stderr,
    "ERROR: AOT compilation not available on this platform\n");
  return 1;
}

#else

#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

fn void  aot_emit(const char *c_path, const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg);
fn void  aot_emit_stdout(const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg);
fn void  sys_error(const char *msg);

fn double aot_build_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

fn int aot_build_timing_enabled(void) {
  const char *env = getenv("HVM_AOT_TIMING");
  return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

fn void aot_build_timing_log(const char *phase, double ms) {
  fprintf(stderr, "AOT_TIMING %s: %.3f ms\n", phase, ms);
}

// Resolves argv0.
fn char *aot_build_resolve_argv0(const char *argv0) {
  if (argv0 == NULL || argv0[0] == '\0') {
    return NULL;
  }

  if (strchr(argv0, '/') != NULL) {
    return realpath(argv0, NULL);
  }

  const char *path_env = getenv("PATH");
  if (path_env == NULL || path_env[0] == '\0') {
    return NULL;
  }

  char *buf = strdup(path_env);
  if (buf == NULL) {
    return NULL;
  }

  char *save = NULL;
  for (char *dir = strtok_r(buf, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
    if (dir[0] == '\0') {
      continue;
    }

    char cand[PATH_MAX];
    int n = snprintf(cand, sizeof(cand), "%s/%s", dir, argv0);
    if (n < 0 || n >= (int)sizeof(cand)) {
      continue;
    }

    if (access(cand, X_OK) == 0) {
      char *abs = realpath(cand, NULL);
      free(buf);
      return abs;
    }
  }

  free(buf);
  return NULL;
}

// Resolves `hvm.c`.
fn void aot_build_runtime_path(char *out, u32 out_len, const char *argv0) {
  char *exe = aot_build_resolve_argv0(argv0);
  if (exe == NULL) {
    sys_error("failed to resolve executable path from argv[0]");
  }

  char *slash = strrchr(exe, '/');
  if (slash == NULL) {
    free(exe);
    sys_error("invalid executable path");
  }

  *slash = '\0';
  int n = snprintf(out, out_len, "%s/hvm.c", exe);
  free(exe);

  if (n < 0 || n >= (int)out_len) {
    sys_error("runtime path too long");
  }

  char *abs = realpath(out, NULL);
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

// Runs one process.
fn int aot_build_spawn(char *const argv[]) {
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
}

// Compiles one C file.
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

// Emits one C file.
fn void aot_write_c_file(const char *c_path, const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  char runtime_path[PATH_MAX];
  aot_build_runtime_path(runtime_path, sizeof(runtime_path), argv0);
  aot_emit(c_path, runtime_path, src_path, src_text, cfg);
}

// Ensures one dir.
fn void aot_build_ensure_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      return;
    }
    fprintf(stderr, "ERROR: temp path is not a directory '%s'\n", path);
    exit(1);
  }

  if (mkdir(path, 0700) != 0) {
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return;
    }
    fprintf(stderr, "ERROR: failed to create directory '%s'\n", path);
    exit(1);
  }
}

// Ensures one dir tree.
fn void aot_build_ensure_dir_tree(const char *path) {
  char buf[PATH_MAX];
  int n = snprintf(buf, sizeof(buf), "%s", path);
  if (n < 0 || n >= (int)sizeof(buf)) {
    sys_error("temp path too long");
  }

  u32 len = (u32)strlen(buf);
  for (u32 i = 1; i < len; i++) {
    if (buf[i] != '/') {
      continue;
    }
    buf[i] = '\0';
    if (buf[0] != '\0') {
      aot_build_ensure_dir(buf);
    }
    buf[i] = '/';
  }
  if (buf[0] != '\0') {
    aot_build_ensure_dir(buf);
  }
}

// Chooses temp root.
fn void aot_build_temp_root(char *out, u32 out_len) {
  const char *tmp = getenv("HVM_TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
      tmp = "/tmp";
    }
  }

  int n = snprintf(out, out_len, "%s", tmp);
  if (n < 0 || n >= (int)out_len) {
    sys_error("AOT temp root path too long");
  }

  aot_build_ensure_dir_tree(out);
}

// Builds one temp dir.
fn void aot_build_temp_dir(char *out, u32 out_len, const char *name) {
  char tmp_root[PATH_MAX];
  aot_build_temp_root(tmp_root, sizeof(tmp_root));

  char tmpl[PATH_MAX];
  int n = snprintf(tmpl, sizeof(tmpl), "%s/%s.XXXXXX", tmp_root, name);
  if (n < 0 || n >= (int)sizeof(tmpl)) {
    sys_error("AOT temp directory path too long");
  }

  if (mkdtemp(tmpl) == NULL) {
    fprintf(stderr, "ERROR: failed to create temp directory '%s'\n", tmpl);
    exit(1);
  }

  n = snprintf(out, out_len, "%s", tmpl);
  if (n < 0 || n >= (int)out_len) {
    sys_error("AOT temp directory path too long");
  }
}

// Joins one temp path.
fn void aot_build_temp_join(char *out, u32 out_len, const char *dir, const char *name) {
  int n = snprintf(out, out_len, "%s/%s", dir, name);
  if (n < 0 || n >= (int)out_len) {
    sys_error("AOT temp file path too long");
  }
}

// Emits C to stdout.
fn void aot_build_to_c(const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  char runtime_path[PATH_MAX];
  aot_build_runtime_path(runtime_path, sizeof(runtime_path), argv0);
  aot_emit_stdout(runtime_path, src_path, src_text, cfg);
}

// Emits, builds, runs once.
fn int aot_build_as_c_once(const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  int  rc = 1;
  char tmp_dir[PATH_MAX];
  char c_path[PATH_MAX];
  char x_path[PATH_MAX];
  int timed = aot_build_timing_enabled();
  double t0 = timed ? aot_build_now_ms() : 0.0;
  double tp = t0;

  aot_build_temp_dir(tmp_dir, sizeof(tmp_dir), "hvm4-aot-as-c");
  aot_build_temp_join(c_path, sizeof(c_path), tmp_dir, "main.c");
  aot_build_temp_join(x_path, sizeof(x_path), tmp_dir, "main.bin");
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
    if (timed) {
      double now = aot_build_now_ms();
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
    aot_build_timing_log("total", now - t0);
  }
  return rc;
}

// Emits one executable.
fn int aot_build_to_output(const char *argv0, const char *src_path, const char *src_text, const char *out_path, const AotBuildCfg *cfg) {
  int  rc = 1;
  char tmp_dir[PATH_MAX];
  char c_path[PATH_MAX];
  int timed = aot_build_timing_enabled();
  double t0 = timed ? aot_build_now_ms() : 0.0;
  double tp = t0;

  aot_build_temp_dir(tmp_dir, sizeof(tmp_dir), "hvm4-aot-output");
  aot_build_temp_join(c_path, sizeof(c_path), tmp_dir, "main.c");
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

  if (timed) {
    double now = aot_build_now_ms();
    aot_build_timing_log("total", now - t0);
  }
  return rc;
}

#endif
