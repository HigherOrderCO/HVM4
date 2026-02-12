#include <dlfcn.h>

fn void cdef_load(const char *so_path) {
  if (CDEF_HANDLE != NULL) {
    dlclose(CDEF_HANDLE);
    CDEF_HANDLE = NULL;
  }

  void *handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    fprintf(stderr, "ERROR: failed to load compiled-def library '%s': %s\n", so_path, dlerror());
    exit(1);
  }

  dlerror();
  CDefRegister reg = (CDefRegister)dlsym(handle, "hvm4_register");
  const char *err = dlerror();
  if (err != NULL) {
    fprintf(stderr, "ERROR: missing hvm4_register in '%s': %s\n", so_path, err);
    exit(1);
  }

  reg(&CDEF_API, CDEF_FUNS, CDEF_ARIS, CDEF_CAP);
  CDEF_HANDLE = handle;
}
