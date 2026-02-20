// JIT Module: Build Orchestrator
// ------------------------------
// Coordinates end-to-end JIT setup for one run: emit C per definition, compile
// to shared libraries, load symbols, and register function pointers.

#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#define HVM4_RUNTIME
#include "runtime.h"
#undef HVM4_RUNTIME

fn char *table_get(u32 id);
fn char *jit_sanitize(const char *name);
fn void jit_emit(const char *c_path, u32 id);
fn void jit_compile(const char *c_path, const char *lib_path, const char *jit_include_dir);
fn HvmJitFn jit_load(const char *lib_path);
fn void jit_set(u32 id, HvmJitFn fun);
fn void sys_error(const char *msg);

// Returns the platform shared-library extension.
fn const char *jit_build_lib_ext(void) {
#if defined(__APPLE__)
  return "dylib";
#else
  return "so";
#endif
}

// Resolves argv[0] to the clang directory path.
fn char *jit_build_clang_dir(const char *argv0) {
  if (argv0 == NULL) {
    return NULL;
  }

  char *abs = realpath(argv0, NULL);
  if (abs == NULL) {
    return NULL;
  }

  char *slash = strrchr(abs, '/');
  if (slash == NULL) {
    free(abs);
    return NULL;
  }

  *slash = '\0';
  return abs;
}

// Creates the local JIT cache directory if missing.
fn void jit_build_ensure_cache_dir(void) {
  if (mkdir(".hvm", 0755) == 0) {
    return;
  }

  if (errno == EEXIST) {
    return;
  }

  fprintf(stderr, "ERROR: failed to create .hvm cache directory\n");
  exit(1);
}

// Emits, compiles, and loads JIT code for all top-level definitions.
fn void jit_build(const char *argv0) {
  jit_build_ensure_cache_dir();

  char include_dir[PATH_MAX];
  char *clang_dir = jit_build_clang_dir(argv0);
  if (clang_dir == NULL) {
    snprintf(include_dir, sizeof(include_dir), "clang/jit");
  } else {
    int n = snprintf(include_dir, sizeof(include_dir), "%s/jit", clang_dir);
    free(clang_dir);
    if (n < 0 || n >= (int)sizeof(include_dir)) {
      sys_error("JIT include dir path too long");
    }
  }

  const char *lib_ext = jit_build_lib_ext();

  for (u32 id = 0; id < TABLE.len; id++) {
    if (BOOK[id] == 0) {
      continue;
    }

    char *name = table_get(id);
    if (name == NULL) {
      continue;
    }

    char *safe = jit_sanitize(name);
    if (safe == NULL) {
      sys_error("JIT name sanitization failed");
    }

    char c_path[PATH_MAX];
    char lib_path[PATH_MAX];
    int  cn = snprintf(c_path, sizeof(c_path), ".hvm/fn_%s.c", safe);
    int  ln = snprintf(lib_path, sizeof(lib_path), ".hvm/fn_%s.%s", safe, lib_ext);

    if (cn < 0 || cn >= (int)sizeof(c_path) || ln < 0 || ln >= (int)sizeof(lib_path)) {
      free(safe);
      sys_error("JIT cache path too long");
    }

    jit_emit(c_path, id);
    jit_compile(c_path, lib_path, include_dir);
    jit_set(id, jit_load(lib_path));

    free(safe);
  }
}
