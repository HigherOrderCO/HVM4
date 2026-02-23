#if HVM_WINDOWS
  #include <windows.h>
  typedef HMODULE hvm_dl_handle_t;
#else
  #include <dlfcn.h>
  typedef void *hvm_dl_handle_t;
#endif

#define HVM_RUNTIME
#include "../hvm_ffi.h"
#undef HVM_RUNTIME

typedef void (*HvmFfiInit)(const HvmApi *api);

static hvm_dl_handle_t *FFI_HANDLES;
static u32              FFI_HANDLES_LEN;
static u32              FFI_HANDLES_CAP;

fn const HvmApi *ffi_api(void);
fn void sys_error(const char *msg);

fn void ffi_handles_push(hvm_dl_handle_t handle) {
  if (FFI_HANDLES_LEN >= FFI_HANDLES_CAP) {
    u32 new_cap = FFI_HANDLES_CAP == 0 ? 8 : FFI_HANDLES_CAP * 2;
    hvm_dl_handle_t *new_handles = realloc(FFI_HANDLES, new_cap * sizeof(hvm_dl_handle_t));
    if (new_handles == NULL) {
      sys_error("FFI handle allocation failed");
    }
    FFI_HANDLES = new_handles;
    FFI_HANDLES_CAP = new_cap;
  }
  FFI_HANDLES[FFI_HANDLES_LEN++] = handle;
}

fn void ffi_load(const char *path) {
#if HVM_WINDOWS
  hvm_dl_handle_t handle = LoadLibraryA(path);
  if (handle == NULL) {
    fprintf(stderr, "ERROR: failed to load FFI library '%s': LoadLibraryA failed\n", path);
    exit(1);
  }

  HvmFfiInit init = (HvmFfiInit)GetProcAddress(handle, "hvm_ffi_init");
  if (init == NULL) {
    fprintf(stderr, "ERROR: missing hvm_ffi_init in '%s': GetProcAddress failed\n", path);
    FreeLibrary(handle);
    exit(1);
  }
#else
  hvm_dl_handle_t handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    fprintf(stderr, "ERROR: failed to load FFI library '%s': %s\n", path, dlerror());
    exit(1);
  }

  dlerror();
  HvmFfiInit init = (HvmFfiInit)dlsym(handle, "hvm_ffi_init");
  const char *err = dlerror();
  if (err != NULL) {
    fprintf(stderr, "ERROR: missing hvm_ffi_init in '%s': %s\n", path, err);
    dlclose(handle);
    exit(1);
  }
#endif

  ffi_handles_push(handle);
  init(ffi_api());
}
