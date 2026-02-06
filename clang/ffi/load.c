#include <dlfcn.h>

#define HVM4_RUNTIME
#include "../hvm4_ffi.h"
#undef HVM4_RUNTIME

typedef void (*HvmFfiInit)(const HvmApi *api);

static void **FFI_HANDLES;
static u32    FFI_HANDLES_LEN;
static u32    FFI_HANDLES_CAP;

fn const HvmApi *ffi_api(void);
fn void sys_error(const char *msg);

fn void ffi_handles_push(void *handle) {
  if (FFI_HANDLES_LEN >= FFI_HANDLES_CAP) {
    u32 new_cap = FFI_HANDLES_CAP == 0 ? 8 : FFI_HANDLES_CAP * 2;
    void **new_handles = realloc(FFI_HANDLES, new_cap * sizeof(void *));
    if (new_handles == NULL) {
      sys_error("FFI handle allocation failed");
    }
    FFI_HANDLES = new_handles;
    FFI_HANDLES_CAP = new_cap;
  }
  FFI_HANDLES[FFI_HANDLES_LEN++] = handle;
}

fn void ffi_load(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    fprintf(stderr, "ERROR: failed to load FFI library '%s': %s\n", path, dlerror());
    exit(1);
  }

  dlerror();
  HvmFfiInit init = (HvmFfiInit)dlsym(handle, "hvm4_ffi_init");
  const char *err = dlerror();
  if (err != NULL) {
    fprintf(stderr, "ERROR: missing hvm4_ffi_init in '%s': %s\n", path, err);
    exit(1);
  }

  ffi_handles_push(handle);
  init(ffi_api());
}
