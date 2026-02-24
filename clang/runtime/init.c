// Runtime Session Init
// --------------------
// Initializes process-global state for one program execution.

#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
  #define MAP_ANONYMOUS MAP_ANON
#endif

// Initializes runtime globals and evaluator flags.
fn void runtime_init(u32 threads, int debug, int silent, int steps_enable) {
  thread_set_count(threads);
  wnf_set_tid(0);

  BOOK       = calloc(BOOK_CAP, sizeof(u64));
  HEAP       = NULL;
  TABLE.data = calloc(BOOK_CAP, sizeof(char *));

  if (HEAP_CAP <= ((u64)SIZE_MAX / sizeof(Term))) {
    size_t heap_bytes = (size_t)(HEAP_CAP * sizeof(Term));
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    mmap_flags |= MAP_NORESERVE;
#endif
    void *heap_map = mmap(NULL, heap_bytes, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (heap_map != MAP_FAILED) {
      HEAP = (Term *)heap_map;
    }
  }

  if (!BOOK || !HEAP || !TABLE.data) {
    sys_error("Memory allocation failed");
  }

  heap_init_slices();
  symbols_init();
  prim_init();

  DEBUG = debug;
  SILENT = silent;
  STEPS_ENABLE = steps_enable;
}
