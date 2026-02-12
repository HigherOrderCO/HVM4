#include <dlfcn.h>

fn void cdef_free(void) {
  cdef_reset(0);
  if (CDEF_HANDLE != NULL) {
    dlclose(CDEF_HANDLE);
    CDEF_HANDLE = NULL;
  }
}
