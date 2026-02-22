// AOT Module: Build Orchestrator
// ------------------------------
// Emits standalone AOT C programs, optionally compiles executables, and
// supports one-shot `--jit` execution mode.

#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

fn void  aot_emit(const char *c_path, const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg);
fn void  aot_emit_stdout(const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg);
fn char *aot_sanitize(const char *name);
fn void  sys_error(const char *msg);

// Creates .hvm cache directory when missing.
fn void aot_build_cache_dir(void) {
  if (mkdir(".hvm", 0755) == 0) {
    return;
  }
  if (errno == EEXIST) {
    return;
  }
  sys_error("failed to create .hvm cache directory");
}

// Returns the basename of a filesystem path.
fn const char *aot_build_basename(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return "out";
  }
  const char *slash = strrchr(path, '/');
  return slash == NULL ? path : (slash + 1);
}

// Resolves argv0 to an absolute executable path.
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

// Resolves absolute runtime path (`.../clang/hvm4.c`) from argv0.
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
  int n = snprintf(out, out_len, "%s/hvm4.c", exe);
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

// Runs one process and returns its exit code.
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

  return 1;
}

// Compiles one generated C program into an executable.
fn void aot_build_compile(const char *c_path, const char *out_path) {
  char *const cmd[] = {
    "clang",
    "-O2",
    "-o",
    (char *)out_path,
    (char *)c_path,
    NULL,
  };

  int rc = aot_build_spawn(cmd);
  if (rc != 0) {
    fprintf(stderr, "ERROR: failed to compile AOT program '%s'\n", c_path);
    exit(rc);
  }
}

// Writes one AOT C file with an absolute runtime include path.
fn void aot_write_c_file(const char *c_path, const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  char runtime_path[PATH_MAX];
  aot_build_runtime_path(runtime_path, sizeof(runtime_path), argv0);
  aot_emit(c_path, runtime_path, src_path, src_text, cfg);
}

// Builds a deterministic cache C path from an output name.
fn void aot_build_cache_c_path(char *out, u32 out_len, const char *stem, const char *suffix) {
  char *safe = aot_sanitize(stem);
  if (safe == NULL) {
    sys_error("AOT name sanitization failed");
  }

  int n = snprintf(out, out_len, ".hvm/%s_%s.c", safe, suffix);
  free(safe);
  if (n < 0 || n >= (int)out_len) {
    sys_error("AOT cache path too long");
  }
}

// Emits only C code to stdout.
fn void aot_build_to_c(const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  char runtime_path[PATH_MAX];
  aot_build_runtime_path(runtime_path, sizeof(runtime_path), argv0);
  aot_emit_stdout(runtime_path, src_path, src_text, cfg);
}

// Emits + compiles one standalone executable and keeps it.
fn void aot_build_compile_out(const char *out_path, const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  aot_build_cache_dir();

  char c_path[PATH_MAX];
  aot_build_cache_c_path(c_path, sizeof(c_path), aot_build_basename(out_path), "aot");

  aot_write_c_file(c_path, argv0, src_path, src_text, cfg);
  aot_build_compile(c_path, out_path);
}

// Emits + compiles + runs once, then removes the temporary executable.
fn int aot_build_jit_once(const char *argv0, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  aot_build_cache_dir();

  char c_path[PATH_MAX];
  char x_path[PATH_MAX];

  aot_build_cache_c_path(c_path, sizeof(c_path), aot_build_basename(src_path), "jit");

  char *safe = aot_sanitize(aot_build_basename(src_path));
  if (safe == NULL) {
    sys_error("AOT name sanitization failed");
  }
  int n = snprintf(x_path, sizeof(x_path), ".hvm/%s_jit.bin", safe);
  free(safe);
  if (n < 0 || n >= (int)sizeof(x_path)) {
    sys_error("AOT executable path too long");
  }

  aot_write_c_file(c_path, argv0, src_path, src_text, cfg);
  aot_build_compile(c_path, x_path);

  char *const run[] = {
    x_path,
    NULL,
  };

  int rc = aot_build_spawn(run);
  unlink(x_path);
  return rc;
}
