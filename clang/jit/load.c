// JIT Module: Dynamic Loader
// --------------------------
// Loads one compiled JIT shared library and resolves its `hvm4_jit_fn` symbol.

#include <dlfcn.h>

#define HVM4_RUNTIME
#include "runtime.h"
#undef HVM4_RUNTIME

fn void jit_handle_push(void *handle);

// Loads one JIT shared library and returns its entrypoint.
fn HvmJitFn jit_load(const char *lib_path) {
  void *handle = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    fprintf(stderr, "ERROR: failed to load JIT library '%s': %s\n", lib_path, dlerror());
    exit(1);
  }

  dlerror();
  HvmJitFn fun = (HvmJitFn)dlsym(handle, "hvm4_jit_fn");
  const char *err = dlerror();
  if (err != NULL) {
    fprintf(stderr, "ERROR: missing hvm4_jit_fn in '%s': %s\n", lib_path, err);
    exit(1);
  }

  jit_handle_push(handle);
  return fun;
}
