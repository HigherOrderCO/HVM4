#if defined(__EMSCRIPTEN__) || defined(HVM_NO_MMAP)

#include <stdlib.h>

fn void sys_munmap_anon(void *ptr, size_t bytes) {
  (void)bytes;
  free(ptr);
}

#else

#include <sys/mman.h>

fn void sys_munmap_anon(void *ptr, size_t bytes) {
  if (ptr == NULL || bytes == 0) {
    return;
  }
  munmap(ptr, bytes);
}

#endif
